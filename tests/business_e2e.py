#!/usr/bin/env python3
"""Independent, opt-in OneDrive for Business E2E tests using the real CLI."""

from __future__ import annotations

import argparse
import copy
from contextlib import closing
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import signal
import sqlite3
import stat
import subprocess
import sys
import tempfile
import time
import tomllib
import uuid


class E2EError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise E2EError(message)


def private_read(path: Path, *, token=False) -> bytes:
    for component in (path, *path.parents):
        require(not component.is_symlink(), "Credential paths must not use symlinks")
    descriptor = os.open(
        path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC
    )
    try:
        metadata = os.fstat(descriptor)
        require(
            stat.S_ISREG(metadata.st_mode) and metadata.st_uid == os.geteuid(),
            "Credentials must be regular files owned by the current user",
        )
        if token:
            require(stat.S_IMODE(metadata.st_mode) == 0o600, "Token must be mode 0600")
        with os.fdopen(descriptor, "rb", closefd=False) as source:
            contents = source.read(1024 * 1024 + 1)
        require(0 < len(contents) <= 1024 * 1024, "Credential file is empty or too large")
        return contents
    finally:
        os.close(descriptor)


def credentials_snapshot(state: Path) -> tuple[str, bytes, bytes]:
    lock = state / "onedrive-cpp.lock"
    private_read(lock)
    descriptor = os.open(lock, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    with os.fdopen(descriptor, "rb") as source_lock:
        try:
            fcntl.flock(source_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise E2EError("Source account is busy; stop sync/watch before copying credentials") from error
        marker = private_read(state / "active_account").decode("utf-8").rstrip("\r\n")
        require(
            marker not in ("", ".", "..")
            and "/" not in marker
            and "\\" not in marker
            and "\0" not in marker,
            "Invalid active-account marker",
        )
        account = state / "accounts" / marker
        token = private_read(account / "refresh_token", token=True)
        require(bool(token.strip()), "Refresh token contains only whitespace")
        return (
            marker,
            private_read(account / "account.json"),
            token,
        )


def private_write(path: Path, contents: bytes):
    descriptor = os.open(
        path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600
    )
    with os.fdopen(descriptor, "wb") as destination:
        os.fchmod(destination.fileno(), 0o600)
        destination.write(contents)


def toml_document(document: dict) -> str:
    def value(item):
        if isinstance(item, bool):
            return "true" if item else "false"
        if isinstance(item, str):
            return json.dumps(item, ensure_ascii=False)
        if isinstance(item, int):
            return str(item)
        if isinstance(item, list):
            return "[" + ", ".join(value(entry) for entry in item) + "]"
        raise E2EError("Unsupported value in source configuration")

    lines = []

    def table(entries, prefix=()):
        if prefix:
            lines.append("[" + ".".join(json.dumps(key) for key in prefix) + "]")
        for key, entry in entries.items():
            if not isinstance(entry, dict):
                lines.append(f"{json.dumps(key)} = {value(entry)}")
        for key, entry in entries.items():
            if isinstance(entry, dict):
                table(entry, (*prefix, key))

    table(document)
    return "\n".join(lines) + "\n"


def select_drive(document: dict, requested: str | None) -> str:
    sync = document.get("sync", {})
    require(not ("drive_id" in sync and "drive_ids" in sync), "Conflicting Drive settings")
    drives = sync.get("drive_ids", [sync.get("drive_id", "me")])
    require(
        isinstance(drives, list)
        and bool(drives)
        and all(isinstance(drive, str) and bool(drive) for drive in drives),
        "Invalid configured Drive list",
    )
    if requested is not None:
        require(requested in drives, "--drive must select a configured Drive reference")
        return requested
    require(len(drives) == 1, "Multiple Drives configured; select one with --drive")
    return drives[0]


def parse_events(completed: subprocess.CompletedProcess) -> list[dict]:
    result = []
    for line in (completed.stdout + "\n" + completed.stderr).splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict):
            result.append(event)
    return result


def section(completed, name):
    matches = [event for event in parse_events(completed) if event.get("event") == name]
    require(len(matches) == 1, f"Expected one {name} JSON event")
    return matches[0].get("values", {})


def wait_until(predicate, timeout, description):
    deadline = time.monotonic() + timeout
    while True:
        result = predicate()
        if result:
            return result
        require(time.monotonic() < deadline, f"Timed out waiting for {description}")
        time.sleep(1)


class Client:
    def __init__(self, workspace, name, source, credentials, drive, scope, executable, timeout):
        require(
            re.fullmatch(r"__onedrive_cpp_business_e2e_[0-9a-f]{32}", scope),
            "Invalid test subtree",
        )
        self.home = workspace / name
        self.home.mkdir(mode=0o700)
        self.state = self.home / "state"
        self.data = self.home / "data"
        self.state.mkdir(mode=0o700)
        self.data.mkdir(mode=0o700)
        marker, account, token = credentials
        accounts = self.state / "accounts"
        accounts.mkdir(mode=0o700)
        account_directory = accounts / marker
        account_directory.mkdir(mode=0o700)
        private_write(self.state / "active_account", (marker + "\n").encode())
        private_write(account_directory / "account.json", account)
        private_write(account_directory / "refresh_token", token)
        self.scope = scope
        self.executable = executable
        self.timeout = timeout
        self.config_path = self.home / "config.toml"
        rules = self.home / "sync-list"
        private_write(rules, f"/{scope}/\n".encode())
        self.config = copy.deepcopy(source)
        self.config.setdefault("console", {})["ui"] = "console"
        self.config["state"] = {"directory": str(self.state)}
        self.config["logging"] = {"level": "warn"}
        self.config["filesystem"] = {"metadata": "database"}
        self.config["sync"] = {
            "data_directory": str(self.data),
            "drive_id": drive,
            "sync_list": str(rules),
            "sync_root_files": False,
            "dry_run": False,
            "dotfiles": "include",
            "nosync_enabled": False,
            "maximum_file_size_bytes": 0,
            "local_conflict": "block",
            "mode": "download_only",
            "delete_policy": "propagate",
            "maximum_remote_deletions": 100,
        }
        self.config["download"] = {
            "concurrency": 1,
            "checkpoint_interval_bytes": 65536,
            "chunk_threshold_bytes": 64 * 1024 * 1024,
            "maximum_rate_bytes_per_second": 0,
            "maximum_total_rate_bytes_per_second": 0,
        }
        self.config["upload"] = {
            "concurrency": 1,
            "maximum_rate_bytes_per_second": 0,
            "maximum_total_rate_bytes_per_second": 0,
        }
        self.config["monitor"] = {
            "poll_interval_seconds": 2,
            "settle_delay_milliseconds": 100,
            "websocket_enabled": True,
        }
        self.processes = []
        self.configure()

    def configure(self, *, mode=None, rate=None, websocket=None):
        if mode is not None:
            self.config["sync"]["mode"] = mode
        if rate is not None:
            self.config["download"]["maximum_rate_bytes_per_second"] = rate
        if websocket is not None:
            self.config["monitor"]["websocket_enabled"] = websocket
        private_write(self.config_path, toml_document(self.config).encode())

    def command(self, *arguments):
        return [
            str(self.executable), *arguments,
            "--config", str(self.config_path), "--output", "json",
        ]

    def run(self, *arguments, expected=(0,)):
        try:
            completed = subprocess.run(
                self.command(*arguments), stdin=subprocess.DEVNULL,
                capture_output=True, text=True, timeout=self.timeout,
            )
        except subprocess.TimeoutExpired as error:
            raise E2EError(f"{' '.join(arguments[:2])} exceeded the command timeout") from error
        if completed.returncode not in expected:
            codes = {
                event.get("reason_code", event.get("event", ""))
                for event in parse_events(completed)
                if event.get("level") in ("error", "critical", "warning")
            }
            safe_codes = sorted(
                code for code in codes
                if isinstance(code, str) and re.fullmatch(r"[a-z_]{1,64}", code)
            )
            raise E2EError(
                f"{' '.join(arguments[:2])} exited {completed.returncode}; "
                f"diagnostic codes: {', '.join(safe_codes) or 'unavailable'}"
            )
        return completed

    def start(self, operation):
        output = tempfile.TemporaryFile(dir=self.home)
        try:
            process = subprocess.Popen(
                self.command("transfer", operation), stdin=subprocess.DEVNULL,
                stdout=output, stderr=subprocess.STDOUT,
            )
        except OSError:
            output.close()
            raise
        self.processes.append((process, output))
        return process

    def stop_all(self):
        forced = False
        failures = []
        for process, output in self.processes:
            try:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        forced = True
                        process.kill()
                        process.wait(timeout=10)
            except (OSError, subprocess.SubprocessError) as error:
                failures.append(str(error))
            finally:
                output.close()
        self.processes = [
            (process, output) for process, output in self.processes
            if process.poll() is None
        ]
        require(not failures, "Client shutdown failed: " + "; ".join(failures))
        require(not forced, "Client required a forced kill during shutdown")

    def only(self, pattern):
        paths = list(self.home.glob(pattern))
        require(len(paths) == 1, f"Expected exactly one isolated {pattern}")
        require(paths[0].resolve().is_relative_to(self.home.resolve()), "Path escaped workspace")
        return paths[0]

    @property
    def root(self):
        return self.only("data/accounts/*/drives/*")

    @property
    def database(self):
        return self.only("state/accounts/*/drives/*/items.sqlite3")

    def path(self, relative=""):
        path = self.root / self.scope / relative
        require(
            path.resolve().is_relative_to((self.root / self.scope).resolve()),
            "Path escaped test subtree",
        )
        return path

    def rows(self, query, parameters=()):
        with closing(sqlite3.connect(self.database.as_uri() + "?mode=ro", uri=True)) as database:
            return database.execute(query, parameters).fetchall()

    def assert_scope(self):
        for remote_path, local_path in self.rows("SELECT remote_path,local_path FROM item"):
            require(
                remote_path == self.scope or remote_path.startswith(self.scope + "/"),
                "Snapshot includes an item outside the test subtree",
            )
            require(
                Path(local_path).resolve().is_relative_to(self.root.resolve()),
                "Snapshot includes a path outside the isolated Drive",
            )

    def cursor(self):
        return tuple(
            self.rows(f"SELECT * FROM {table} ORDER BY 1,2")
            for table in ("drive_state", "pending_delta", "pending_delta_change")
        )

    def remote(self, relative):
        rows = self.rows(
            "SELECT remote_id,etag,ctag FROM item WHERE remote_path=?",
            (self.scope + "/" + relative,),
        )
        require(len(rows) == 1, "Expected one tracked remote item")
        return rows[0]

    def sync(self, mode="bidirectional", expected=(0,)):
        self.configure(mode=mode)
        result = self.run("transfer", "sync", expected=expected)
        self.assert_scope()
        return result

    def pull_until(self, predicate):
        def pull():
            self.sync("download_only")
            return predicate()
        return wait_until(pull, self.timeout, "cloud changes on the independent client")


class BusinessE2E:
    def __init__(self, workspace, source, credentials, drive, executable, timeout):
        self.scope = "__onedrive_cpp_business_e2e_" + uuid.uuid4().hex
        self.owner = (uuid.uuid4().hex + "\n").encode()
        self.workspace = workspace
        self.source = source
        self.drive = drive
        self.executable = executable
        self.timeout = timeout
        self.a = Client(workspace, "a", source, credentials, drive, self.scope, executable, timeout)
        self.b = Client(workspace, "b", source, credentials, drive, self.scope, executable, timeout)
        self.upload_attempted = False
        self.fixture = {
            "owner.txt": self.owner,
            "nested/unicode-\u6d4b\u8bd5.txt": "Business \u6d4b\u8bd5\n".encode(),
            "empty.bin": b"",
            "preserve.txt": b"cloud-original\n",
            "conflict.txt": b"original\n",
            "local-delete.txt": b"local deletion\n",
            "remote-delete.txt": b"remote deletion\n",
        }

    def identity(self):
        identities = [
            section(client.run("inspect", "status"), "status")
            for client in (self.a, self.b)
        ]
        require(
            all(identity.get("drive_type") == "business" for identity in identities),
            "Selected live Drive must report driveType=business (not personal/documentLibrary)",
        )
        require(
            isinstance(identities[0].get("drive_id"), str)
            and bool(identities[0]["drive_id"])
            and identities[0]["drive_id"] == identities[1].get("drive_id"),
            "The two clients resolved different Drives",
        )
        self.drive = identities[0]["drive_id"]
        for client in (self.a, self.b):
            client.config["sync"]["drive_id"] = self.drive
            client.configure()
        self.a.sync("download_only")
        self.b.sync("download_only")
        require(
            not self.a.path().exists() and not self.b.path().exists(),
            "Random test subtree already exists; refusing to adopt it",
        )

    def roundtrip(self):
        for name, contents in self.fixture.items():
            path = self.a.path(name)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        self.a.path("empty-dir").mkdir()
        self.upload_attempted = True
        self.a.sync()
        self.b.pull_until(
            lambda: all(self.b.path(name).is_file() for name in self.fixture)
            and self.b.path("empty-dir").is_dir()
        )
        for name, contents in self.fixture.items():
            require(self.b.path(name).read_bytes() == contents, "Uploaded/downloaded bytes differ")
        require(self.b.path("empty-dir").is_dir(), "Empty directory did not round-trip")

    def idempotence(self):
        paths = [self.a.path(name) for name in self.fixture]
        local = [(path.stat().st_ino, path.stat().st_mtime_ns) for path in paths]
        remote = {name: self.b.remote(name) for name in self.fixture}
        self.a.sync()
        self.b.sync("download_only")
        require(
            local == [(path.stat().st_ino, path.stat().st_mtime_ns) for path in paths],
            "Unchanged synchronization replaced or touched a local file",
        )
        require(
            remote == {name: self.b.remote(name) for name in self.fixture},
            "Unchanged synchronization changed remote identity/content tags",
        )

    def moves(self):
        previous = "nested/unicode-\u6d4b\u8bd5.txt"
        identity = self.a.remote(previous)[0]
        self.a.path(previous).rename(self.a.path("nested/renamed.txt"))
        self.a.sync()
        self.b.pull_until(
            lambda: self.b.path("nested/renamed.txt").is_file()
            and not self.b.path(previous).exists()
        )
        require(not self.b.path(previous).exists(), "Local rename left the old remote path")
        require(self.b.remote("nested/renamed.txt")[0] == identity, "File rename replaced its ID")
        self.b.path("nested").rename(self.b.path("moved"))
        self.b.sync()
        self.a.pull_until(
            lambda: self.a.path("moved/renamed.txt").is_file()
            and not self.a.path("nested").exists()
        )
        require(not self.a.path("nested").exists(), "Remote directory move left the old path")
        require(self.a.remote("moved/renamed.txt")[0] == identity, "Directory move replaced child ID")

    def conflict(self):
        self.a.path("conflict.txt").write_bytes(b"keep-local\n")
        self.b.path("conflict.txt").write_bytes(b"keep-cloud\n")
        self.b.sync()

        def blocked():
            completed = self.a.sync("download_only", expected=(0, 3))
            return completed if completed.returncode == 3 else None

        completed = wait_until(blocked, self.timeout, "a local-modification conflict")
        require(
            any(
                event.get("event") == "item_blocked"
                and event.get("path") == self.scope + "/conflict.txt"
                for event in parse_events(completed)
            ),
            "Conflict did not identify the expected file",
        )
        require(self.a.path("conflict.txt").read_bytes() == b"keep-local\n", "Conflict overwrote local bytes")
        self.b.sync("download_only")
        require(self.b.path("conflict.txt").read_bytes() == b"keep-cloud\n", "Conflict overwrote cloud bytes")
        self.a.path("conflict.txt").unlink()
        self.a.run("transfer", "restore", self.scope + "/conflict.txt")
        require(self.a.path("conflict.txt").read_bytes() == b"keep-cloud\n", "Conflict recovery failed")
        self.a.sync("download_only")

    def restore(self):
        self.a.path("moved/renamed.txt").unlink()
        self.a.path("empty-dir").rmdir()
        self.a.path("preserve.txt").write_bytes(b"leave-existing-local-alone\n")
        cursor = self.a.cursor()
        remote = self.b.remote("preserve.txt")
        preview = self.a.run("transfer", "restore", self.scope, "--dry-run")
        plan = section(preview, "restore_plan")
        require(
            plan.get("files") == "1" and plan.get("directories") == "1",
            "Restore preview did not report exactly one missing file and directory",
        )
        require(not self.a.path("moved/renamed.txt").exists(), "Preview restored a file")
        require(not self.a.path("empty-dir").exists(), "Preview restored a directory")
        require(cursor == self.a.cursor(), "Restore preview advanced or consumed delta state")
        self.a.run("transfer", "restore", self.scope)
        require(self.a.path("moved/renamed.txt").read_bytes() == "Business \u6d4b\u8bd5\n".encode(), "Restore bytes differ")
        require(self.a.path("empty-dir").is_dir(), "Restore omitted the empty directory")
        require(self.a.path("preserve.txt").read_bytes() == b"leave-existing-local-alone\n", "Restore overwrote an existing file")
        require(cursor == self.a.cursor(), "Restore execution advanced or consumed delta state")
        self.b.sync("download_only")
        require(self.b.remote("preserve.txt") == remote, "Restore modified the cloud file")
        require(self.b.path("preserve.txt").read_bytes() == b"cloud-original\n", "Restore uploaded local bytes")
        self.a.path("preserve.txt").write_bytes(b"cloud-original\n")

    def busy_restore(self, process):
        for flags in ((), ("--dry-run",)):
            completed = self.a.run("transfer", "restore", self.scope, *flags, expected=(1,))
            require(
                any(
                    event.get("event") == "restore_busy"
                    and str(process.pid) in event.get("message", "")
                    for event in parse_events(completed)
                ),
                "Busy restore omitted its structured refusal/owner PID",
            )
            require(process.poll() is None, "Restore stopped the owning sync/watch process")

    def interrupted_sync(self):
        payload = bytes(range(256)) * 32768
        self.b.path("resume.bin").write_bytes(payload)
        self.b.sync()
        self.a.configure(mode="download_only", rate=65536)
        process = self.a.start("sync")

        def partial():
            require(process.poll() is None, "Slow sync exited before its durable checkpoint")
            records = self.a.rows(
                "SELECT temporary_path,completed_bytes FROM partial_download WHERE remote_path=?",
                (self.scope + "/resume.bin",),
            )
            return records[0] if records and 0 < records[0][1] < len(payload) else None

        wait_until(partial, self.timeout, "a durable partial download")
        self.busy_restore(process)
        process.send_signal(signal.SIGTERM)
        process.wait(timeout=10)
        records = self.a.rows(
            "SELECT temporary_path,completed_bytes FROM partial_download WHERE remote_path=?",
            (self.scope + "/resume.bin",),
        )
        require(len(records) == 1 and records[0][1] > 0, "Interrupted sync lost its checkpoint")
        temporary = Path(records[0][0])
        require(temporary.resolve().is_relative_to(self.a.root.resolve()), "Partial escaped the isolated Drive")
        with temporary.open("rb") as checkpoint:
            original = os.fstat(checkpoint.fileno())
            self.a.configure(rate=0)
            self.a.sync("download_only")
            installed = self.a.path("resume.bin").stat()
            require(
                (original.st_dev, original.st_ino) == (installed.st_dev, installed.st_ino),
                "Download restarted in a new file instead of reusing its checkpoint",
            )
        require(self.a.path("resume.bin").read_bytes() == payload, "Resumed content differs")
        require(
            not self.a.rows("SELECT remote_id FROM partial_download"),
            "Completed resume left a partial-download record",
        )

    def watch(self, *, websocket):
        self.a.configure(mode="bidirectional", websocket=websocket)
        process = self.a.start("watch")

        def ready():
            require(process.poll() is None, "Watch exited before its control socket was ready")
            completed = self.a.run("monitor", "status", expected=(0, 1))
            if completed.returncode != 0:
                return False
            reply = section(completed, "monitor_control")
            return (
                reply.get("pid") == str(process.pid)
                and int(reply.get("completed_rounds", "0")) > 0
                and reply.get("last_result") == "0"
            )

        wait_until(ready, self.timeout, "a successful watch round")
        self.busy_restore(process)
        name = "watch-notifications.txt" if websocket else "watch-polling.txt"
        self.b.path(name).write_bytes(b"remote-watch\n")
        self.b.sync()

        def received():
            require(process.poll() is None, "Watch exited while awaiting a remote change")
            return self.a.path(name).is_file() and self.a.path(name).read_bytes() == b"remote-watch\n"

        wait_until(received, self.timeout, "automatic watch download")
        self.a.path(name).write_bytes(b"local-watch\n")
        self.b.pull_until(lambda: self.b.path(name).read_bytes() == b"local-watch\n")
        self.a.run("monitor", "stop")
        result = process.wait(timeout=self.timeout)
        require(result in (0, 130), "Watch did not stop cleanly")
        self.a.assert_scope()

    def deletions(self):
        self.a.path("local-delete.txt").unlink()
        self.a.sync()
        self.b.pull_until(lambda: not self.b.path("local-delete.txt").exists())
        require(
            not self.b.rows("SELECT remote_id FROM item WHERE remote_path=?", (self.scope + "/local-delete.txt",)),
            "Local deletion left a remote snapshot",
        )
        self.b.path("remote-delete.txt").unlink()
        self.b.sync()
        self.a.pull_until(lambda: not self.a.path("remote-delete.txt").exists())
        require(
            not self.a.rows("SELECT remote_id FROM item WHERE remote_path=?", (self.scope + "/remote-delete.txt",)),
            "Remote deletion left a local snapshot",
        )

    def cleanup(self):
        if not self.upload_attempted:
            return
        credentials = credentials_snapshot(self.b.state)
        cleaner = Client(
            self.workspace, "cleanup", self.source, credentials, self.drive,
            self.scope, self.executable, self.timeout,
        )
        cleaner.sync("download_only")
        if not cleaner.path().exists():
            return
        marker = cleaner.path("owner.txt")
        require(
            marker.is_file() and marker.read_bytes() == self.owner,
            "Cleanup refused: remote subtree ownership marker is missing or different",
        )
        shutil.rmtree(cleaner.path())
        cleaner.sync()
        verifier = Client(
            self.workspace, "cleanup-verifier", self.source,
            credentials_snapshot(cleaner.state), self.drive,
            self.scope, self.executable, self.timeout,
        )
        verifier.sync("download_only")
        require(not verifier.path().exists(), "Remote test subtree still exists after cleanup")

    def run(self):
        scenarios = [
            ("business-identity-and-isolation", self.identity),
            ("upload-download-unicode-empty-files-and-directories", self.roundtrip),
            ("unchanged-sync-is-idempotent", self.idempotence),
            ("local-file-and-remote-directory-moves", self.moves),
            ("local-conflict-and-explicit-recovery", self.conflict),
            ("restore-preview-execution-and-no-overwrite", self.restore),
            ("sync-busy-refusal-interruption-and-resume", self.interrupted_sync),
            ("watch-notification-enabled-compatibility-and-busy-refusal", lambda: self.watch(websocket=True)),
            ("watch-with-explicit-polling-fallback", lambda: self.watch(websocket=False)),
            ("local-and-remote-deletion-propagation", self.deletions),
        ]
        print(f"Remote test subtree: {self.scope}", flush=True)
        errors = []
        interrupted = False
        try:
            for index, (name, scenario) in enumerate(scenarios, 1):
                print(f"[{index}/{len(scenarios)}] {name}", flush=True)
                scenario()
        except (E2EError, OSError, sqlite3.Error, subprocess.SubprocessError) as error:
            errors.append(error)
        except KeyboardInterrupt:
            interrupted = True
        finally:
            for client in (self.a, self.b):
                try:
                    client.stop_all()
                except (E2EError, OSError, subprocess.SubprocessError) as error:
                    errors.append(error)
            if any(
                process.poll() is None
                for client in (self.a, self.b) for process, _ in client.processes
            ):
                errors.append(E2EError(f"Remote cleanup refused while a test process is running: {self.scope}"))
            else:
                try:
                    self.cleanup()
                except (E2EError, OSError, sqlite3.Error, subprocess.SubprocessError) as error:
                    errors.append(E2EError(f"Remote cleanup failed for {self.scope}: {error}"))
        if errors:
            raise E2EError("\n".join(str(error) for error in errors))
        if interrupted:
            raise KeyboardInterrupt
        print("PASS: 10 Business scenarios; remote subtree removed and verified.", flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=Path, default=Path("build/release/onedrive-cpp"))
    parser.add_argument("--config", type=Path, default=Path.home() / ".config/onedrive-cpp/config.toml")
    parser.add_argument("--drive", help="One configured Drive reference; mandatory for multiple Drives")
    parser.add_argument("--timeout", type=int, default=600, help="Command/eventual-consistency timeout in seconds")
    parser.add_argument(
        "--allow-remote-writes", action="store_true",
        help="Permit creating, changing and deleting only the random test subtree",
    )
    arguments = parser.parse_args(argv)
    if not arguments.allow_remote_writes:
        parser.error("Live E2E requires --allow-remote-writes; no files or credentials were read")
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        executable = arguments.client.resolve(strict=True)
        require(executable.is_file() and os.access(executable, os.X_OK), "CLI is not executable")
        config_path = arguments.config.resolve(strict=True)
        with config_path.open("rb") as source_file:
            source = tomllib.load(source_file)
        require(source.get("config_version") == 2, "Source config must have config_version = 2")
        for table in ("sync", "state", "proxy"):
            require(isinstance(source.get(table, {}), dict), f"{table} must be a table")
        drive = select_drive(source, arguments.drive)
        proxy = source.get("proxy", {})
        for key in ("password_file", "ca_file"):
            if key in proxy:
                require(isinstance(proxy[key], str) and proxy[key], f"proxy.{key} must be a path")
                proxy[key] = str((config_path.parent / proxy[key]).resolve(strict=True))
        state_value = source.get("state", {}).get("directory", str(Path.home() / ".local/state/onedrive-cpp"))
        data_value = source.get("sync", {}).get("data_directory", str(Path.home() / "OneDrive"))
        require(
            isinstance(state_value, str) and bool(state_value)
            and isinstance(data_value, str) and bool(data_value),
            "Source state and synchronization directories must be paths",
        )
        state = Path(state_value).absolute()
        data = Path(data_value).resolve()
        credentials = credentials_snapshot(state)
        with tempfile.TemporaryDirectory(prefix="onedrivecpp-business-e2e-", dir="/tmp") as temporary:
            workspace = Path(temporary)
            require(
                not workspace.is_relative_to(data)
                and not data.is_relative_to(workspace)
                and not workspace.is_relative_to(state.resolve())
                and not state.resolve().is_relative_to(workspace),
                "Test workspace overlaps the source state or synchronization directory",
            )
            BusinessE2E(
                workspace, source, credentials, drive, executable, arguments.timeout
            ).run()
    except KeyboardInterrupt:
        print("CANCELLED: Business E2E interrupted.", file=sys.stderr)
        return 130
    except (E2EError, OSError, ValueError, sqlite3.Error, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
