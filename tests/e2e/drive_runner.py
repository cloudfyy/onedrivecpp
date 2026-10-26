from __future__ import annotations

import argparse
from collections.abc import Callable
from contextlib import closing
import copy
import os
from pathlib import Path
import re
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
import tomllib
import uuid

from .base import (
    DEFAULT_CLIENT_PATH,
    DEFAULT_CONFIG_PATH,
    E2EError,
    require,
    toml_document,
)
from .state import (
    credentials_snapshot,
    private_write,
    write_isolated_credentials,
)
from .util import parse_events, section


def wait_for_condition(
    predicate: Callable[[], object],
    timeout: float,
    description: str,
) -> object:
    deadline = time.monotonic() + timeout
    while True:
        result = predicate()
        if result:
            return result
        require(
            time.monotonic() < deadline,
            f"Timed out waiting for {description}",
        )
        time.sleep(1)


class IsolatedDriveClient:
    SCOPE_PATTERN = r"__onedrive_cpp_(?:business|sharepoint)_e2e_[0-9a-f]{32}"

    def __init__(
        self,
        workspace: Path,
        name: str,
        source: dict[str, object],
        credentials: tuple[str, bytes, bytes],
        drive: str,
        scope: str,
        executable: Path,
        timeout: int,
    ) -> None:
        require(
            re.fullmatch(self.SCOPE_PATTERN, scope),
            "Invalid test subtree",
        )
        self.home = workspace / name
        self.home.mkdir(mode=0o700)
        self.state = self.home / "state"
        self.data = self.home / "data"
        self.state.mkdir(mode=0o700)
        self.data.mkdir(mode=0o700)
        write_isolated_credentials(self.state, credentials)
        self.scope = scope
        self.executable = executable
        self.timeout = timeout
        self.config_path = self.home / "config.toml"
        rules = self.home / "sync-list"
        private_write(rules, f"/{scope}/\n".encode())
        self.config = copy.deepcopy(source)
        console_table = self.config.setdefault("console", {})
        if isinstance(console_table, dict):
            console_table["ui"] = "console"
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
        self.processes: list[tuple[subprocess.Popen[bytes], object]] = []
        self.configure()

    def configure(
        self,
        *,
        mode: str | None = None,
        rate: int | None = None,
        websocket: bool | None = None,
        drive_id: str | None = None,
    ) -> None:
        sync_table = self.config["sync"]
        download_table = self.config["download"]
        monitor_table = self.config["monitor"]
        if isinstance(sync_table, dict):
            if mode is not None:
                sync_table["mode"] = mode
            if drive_id is not None:
                sync_table["drive_id"] = drive_id
        if isinstance(download_table, dict) and rate is not None:
            download_table["maximum_rate_bytes_per_second"] = rate
        if isinstance(monitor_table, dict) and websocket is not None:
            monitor_table["websocket_enabled"] = websocket
        private_write(self.config_path, toml_document(self.config).encode())

    def command(self, *arguments: str) -> list[str]:
        return [
            str(self.executable),
            *arguments,
            "--config",
            str(self.config_path),
            "--output",
            "json",
        ]

    def run(
        self,
        *arguments: str,
        expected: tuple[int, ...] = (0,),
    ) -> subprocess.CompletedProcess[str]:
        try:
            completed = subprocess.run(
                self.command(*arguments),
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                timeout=self.timeout,
            )
        except subprocess.TimeoutExpired as error:
            raise E2EError(
                f"{' '.join(arguments[:2])} exceeded the command timeout"
            ) from error
        if completed.returncode not in expected:
            codes = {
                event.get("reason_code", event.get("event", ""))
                for event in parse_events(completed)
                if event.get("level") in ("error", "critical", "warning")
            }
            safe_codes = sorted(
                code
                for code in codes
                if isinstance(code, str)
                and re.fullmatch(r"[a-z_]{1,64}", code)
            )
            raise E2EError(
                f"{' '.join(arguments[:2])} exited {completed.returncode}; "
                f"diagnostic codes: {', '.join(safe_codes) or 'unavailable'}"
            )
        return completed

    def start(self, operation: str) -> subprocess.Popen[bytes]:
        output = tempfile.TemporaryFile(dir=self.home)
        try:
            process = subprocess.Popen(
                self.command("transfer", operation),
                stdin=subprocess.DEVNULL,
                stdout=output,
                stderr=subprocess.STDOUT,
            )
        except OSError:
            output.close()
            raise
        self.processes.append((process, output))
        return process

    def stop_all(self) -> None:
        forced = False
        failures: list[str] = []
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
            (process, output)
            for process, output in self.processes
            if process.poll() is None
        ]
        require(not failures, "Client shutdown failed: " + "; ".join(failures))
        require(not forced, "Client required a forced kill during shutdown")

    def only(self, pattern: str) -> Path:
        paths = list(self.home.glob(pattern))
        require(len(paths) == 1, f"Expected exactly one isolated {pattern}")
        require(
            paths[0].resolve().is_relative_to(self.home.resolve()),
            "Path escaped workspace",
        )
        return paths[0]

    @property
    def root(self) -> Path:
        return self.only("data/accounts/*/drives/*")

    @property
    def database(self) -> Path:
        return self.only("state/accounts/*/drives/*/items.sqlite3")

    def path(self, relative: str = "") -> Path:
        path = self.root / self.scope / relative
        require(
            path.resolve().is_relative_to((self.root / self.scope).resolve()),
            "Path escaped test subtree",
        )
        return path

    def rows(
        self,
        query: str,
        parameters: tuple[object, ...] = (),
    ) -> list[tuple[object, ...]]:
        with closing(
            sqlite3.connect(self.database.as_uri() + "?mode=ro", uri=True)
        ) as database:
            return database.execute(query, parameters).fetchall()

    def assert_scope(self) -> None:
        for remote_path, local_path in self.rows(
            "SELECT remote_path,local_path FROM item"
        ):
            require(
                isinstance(remote_path, str)
                and (
                    remote_path == self.scope
                    or remote_path.startswith(self.scope + "/")
                ),
                "Snapshot includes an item outside the test subtree",
            )
            require(
                Path(str(local_path))
                .resolve()
                .is_relative_to(self.root.resolve()),
                "Snapshot includes a path outside the isolated Drive",
            )

    def cursor(self) -> tuple[list[tuple[object, ...]], ...]:
        return tuple(
            self.rows(f"SELECT * FROM {table} ORDER BY 1,2")
            for table in (
                "drive_state",
                "pending_delta",
                "pending_delta_change",
            )
        )

    def remote(self, relative: str) -> tuple[object, ...]:
        rows = self.rows(
            "SELECT remote_id,etag,ctag FROM item WHERE remote_path=?",
            (self.scope + "/" + relative,),
        )
        require(len(rows) == 1, "Expected one tracked remote item")
        return rows[0]

    def sync(
        self,
        mode: str = "bidirectional",
        expected: tuple[int, ...] = (0,),
    ) -> subprocess.CompletedProcess[str]:
        self.configure(mode=mode)
        result = self.run("transfer", "sync", expected=expected)
        self.assert_scope()
        return result

    def pull_until(self, predicate: Callable[[], object]) -> object:
        def pull() -> object:
            self.sync("download_only")
            return predicate()

        return wait_for_condition(
            pull,
            self.timeout,
            "cloud changes on the independent client",
        )


class DualClientDriveE2E:
    SCOPE_PREFIX = "__onedrive_cpp_drive_e2e_"
    EXPECTED_DRIVE_TYPE = "business"
    DISALLOWED_DRIVE_TYPES = "personal/documentLibrary"
    UNICODE_TEXT = "Drive \u6d4b\u8bd5\n"
    IDENTITY_SCENARIO = "drive-identity-and-isolation"
    SUITE_LABEL = "Drive"

    def _module(self) -> object:
        return sys.modules.get(self.__class__.__module__, sys.modules[__name__])

    def _client_class(self) -> type[IsolatedDriveClient]:
        return getattr(self._module(), "Client", IsolatedDriveClient)

    def _snapshot_credentials(
        self,
        state: Path,
    ) -> tuple[str, bytes, bytes]:
        snapshot_fn = getattr(
            self._module(),
            "credentials_snapshot",
            credentials_snapshot,
        )
        return snapshot_fn(state)

    def __init__(
        self,
        workspace: Path,
        source: dict[str, object],
        credentials: tuple[str, bytes, bytes],
        drives: str | list[str] | tuple[str, ...],
        executable: Path,
        timeout: int,
    ) -> None:
        self.scope = self.SCOPE_PREFIX + uuid.uuid4().hex
        self.owner = (uuid.uuid4().hex + "\n").encode()
        self.workspace = workspace
        self.source = source
        self.candidates = (
            list(drives)
            if isinstance(drives, (list, tuple))
            else [drives]
        )
        require(
            bool(self.candidates)
            and all(
                isinstance(candidate, str) and bool(candidate)
                for candidate in self.candidates
            ),
            "At least one candidate Drive is required",
        )
        self.drive = self.candidates[0]
        self.executable = executable
        self.timeout = timeout
        client_cls = self._client_class()
        self.a = client_cls(
            workspace,
            "a",
            source,
            credentials,
            self.drive,
            self.scope,
            executable,
            timeout,
        )
        self.b = client_cls(
            workspace,
            "b",
            source,
            credentials,
            self.drive,
            self.scope,
            executable,
            timeout,
        )
        self.upload_attempted = False
        self.fixture = {
            "owner.txt": self.owner,
            "nested/unicode-\u6d4b\u8bd5.txt": self.UNICODE_TEXT.encode(),
            "empty.bin": b"",
            "preserve.txt": b"cloud-original\n",
            "conflict.txt": b"original\n",
            "local-delete.txt": b"local deletion\n",
            "remote-delete.txt": b"remote deletion\n",
        }

    def resolve_candidate(self) -> str:
        if len(self.candidates) == 1:
            return self.candidates[0]
        matched: list[str] = []
        for candidate in self.candidates:
            self.a.configure(drive_id=candidate)
            status = section(self.a.run("inspect", "status"), "status")
            if status.get("drive_type") == self.EXPECTED_DRIVE_TYPE:
                matched.append(candidate)
        require(
            len(matched) != 0,
            f"None of the configured Drives reported driveType={self.EXPECTED_DRIVE_TYPE}",
        )
        require(
            len(matched) == 1,
            f"Multiple configured Drives reported driveType={self.EXPECTED_DRIVE_TYPE}; select one with --drive",
        )
        return matched[0]

    def identity(self) -> None:
        if len(self.candidates) > 1:
            selected = self.resolve_candidate()
            for client in (self.a, self.b):
                client.configure(drive_id=selected)
        identities = [
            section(client.run("inspect", "status"), "status")
            for client in (self.a, self.b)
        ]
        require(
            all(
                identity.get("drive_type") == self.EXPECTED_DRIVE_TYPE
                for identity in identities
            ),
            f"Selected live Drive must report driveType={self.EXPECTED_DRIVE_TYPE} "
            f"(not {self.DISALLOWED_DRIVE_TYPES})",
        )
        require(
            isinstance(identities[0].get("drive_id"), str)
            and bool(identities[0]["drive_id"])
            and identities[0]["drive_id"] == identities[1].get("drive_id"),
            "The two clients resolved different Drives",
        )
        self.drive = str(identities[0]["drive_id"])
        for client in (self.a, self.b):
            client.configure(drive_id=self.drive)
        self.a.sync("download_only")
        self.b.sync("download_only")
        require(
            not self.a.path().exists() and not self.b.path().exists(),
            "Random test subtree already exists; refusing to adopt it",
        )

    def verify_discovery(self) -> None:
        pass

    def roundtrip(self) -> None:
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
            require(
                self.b.path(name).read_bytes() == contents,
                "Uploaded/downloaded bytes differ",
            )
        require(
            self.b.path("empty-dir").is_dir(),
            "Empty directory did not round-trip",
        )

    def inspection_and_diagnostics(self) -> None:
        config_info = section(self.a.run("inspect", "config"), "effective_config")
        require(
            config_info.get("data_directory") == str(self.a.data)
            and config_info.get("state_directory") == str(self.a.state),
            "inspect config did not report isolated workspace directories",
        )
        quota_info = section(self.a.run("inspect", "quota"), "quota")
        require(
            quota_info.get("drive_id") == self.drive
            and bool(quota_info.get("state")),
            "inspect quota did not report the selected Drive quota",
        )
        drives_completed = self.a.run("inspect", "drives")
        require(
            any(
                event.get("event") == "drive"
                and isinstance(event.get("values"), dict)
                and event["values"].get("drive_id") == self.drive
                and event["values"].get("type") == self.EXPECTED_DRIVE_TYPE
                for event in parse_events(drives_completed)
            ),
            "inspect drives did not list the selected Drive with its expected type",
        )
        self.verify_discovery()
        storage_info = section(self.a.run("inspect", "storage"), "storage")
        require(
            storage_info.get("root_exists") == "true"
            and int(str(storage_info.get("tracked_files", "0")))
            >= len(self.fixture),
            "inspect storage did not report tracked files in the isolated Drive",
        )
        files_summary = section(
            self.a.run("inspect", "files", self.scope),
            "downloaded_file_summary",
        )
        require(
            files_summary.get("ok") == str(len(self.fixture))
            and files_summary.get("missing") == "0"
            and files_summary.get("modified") == "0",
            "inspect files did not report all fixture files as ok",
        )
        verify_summary = section(
            self.a.run("inspect", "verify", self.scope, "--mode", "metadata"),
            "file_verification_summary",
        )
        require(
            verify_summary.get("verified") == str(len(self.fixture))
            and verify_summary.get("failed") == "0",
            "inspect verify did not validate all fixture files",
        )
        health_completed = self.a.run("inspect", "health")
        require(
            section(health_completed, "database_integrity").get("status")
            == "healthy"
            and any(
                event.get("event") == "database_integrity_passed"
                for event in parse_events(health_completed)
            ),
            "inspect health did not report a healthy synchronization database",
        )

    def idempotence(self) -> None:
        paths = [self.a.path(name) for name in self.fixture]
        local = [(path.stat().st_ino, path.stat().st_mtime_ns) for path in paths]
        remote = {name: self.b.remote(name) for name in self.fixture}
        self.a.sync()
        self.b.sync("download_only")
        require(
            local
            == [(path.stat().st_ino, path.stat().st_mtime_ns) for path in paths],
            "Unchanged synchronization replaced or touched a local file",
        )
        require(
            remote == {name: self.b.remote(name) for name in self.fixture},
            "Unchanged synchronization changed remote identity/content tags",
        )
        reset_completed = self.a.run("state", "reset-cursor")
        require(
            any(
                event.get("event") == "state_cursor_reset"
                for event in parse_events(reset_completed)
            ),
            "state reset-cursor did not report cursor reset",
        )
        self.a.sync()
        require(
            local
            == [(path.stat().st_ino, path.stat().st_mtime_ns) for path in paths],
            "Post-cursor-reset synchronization replaced or touched a local file",
        )

    def modes_and_single_download(self) -> None:
        self.a.path("upload-only.txt").write_bytes(b"upload-only-payload\n")
        self.a.configure(mode="bidirectional")
        self.a.run("transfer", "sync", "--dry-run")
        self.b.sync("download_only")
        require(
            not self.b.path("upload-only.txt").exists(),
            "Dry-run synchronization uploaded a local file",
        )
        self.b.path("single-download.txt").write_bytes(
            b"single-download-payload\n"
        )
        self.b.sync()
        self.a.sync("upload_only")
        require(
            not self.a.path("single-download.txt").exists(),
            "upload_only synchronization downloaded a remote file",
        )
        self.b.pull_until(lambda: self.b.path("upload-only.txt").is_file())
        require(
            self.b.path("upload-only.txt").read_bytes()
            == b"upload-only-payload\n",
            "upload_only synchronization did not upload local file bytes",
        )
        remote_target = self.scope + "/single-download.txt"
        plan = section(
            self.a.run("transfer", "download", remote_target, "--dry-run"),
            "single_download_plan",
        )
        require(
            plan.get("remote_path") == remote_target,
            "Single-file download plan reported the wrong remote path",
        )
        require(
            not self.a.path("single-download.txt").exists(),
            "Single-file download dry-run created the local file",
        )
        self.a.run("transfer", "download", remote_target)
        require(
            self.a.path("single-download.txt").read_bytes()
            == b"single-download-payload\n",
            "Single-file download bytes differ",
        )
        reused = self.a.run("transfer", "download", remote_target)
        require(
            any(
                event.get("event") == "single_download_reused"
                for event in parse_events(reused)
            ),
            "Repeated single-file download did not reuse the current local file",
        )
        self.a.path("upload-only.txt").unlink()
        self.a.path("single-download.txt").unlink()
        self.a.sync()
        self.b.pull_until(
            lambda: not self.b.path("upload-only.txt").exists()
            and not self.b.path("single-download.txt").exists()
        )

    def moves(self) -> None:
        previous = "nested/unicode-\u6d4b\u8bd5.txt"
        identity = self.a.remote(previous)[0]
        self.a.path(previous).rename(self.a.path("nested/renamed.txt"))
        self.a.sync()
        self.b.pull_until(
            lambda: self.b.path("nested/renamed.txt").is_file()
            and not self.b.path(previous).exists()
        )
        require(
            not self.b.path(previous).exists(),
            "Local rename left the old remote path",
        )
        require(
            self.b.remote("nested/renamed.txt")[0] == identity,
            "File rename replaced its ID",
        )
        self.b.path("nested").rename(self.b.path("moved"))
        self.b.sync()
        self.a.pull_until(
            lambda: self.a.path("moved/renamed.txt").is_file()
            and not self.a.path("nested").exists()
        )
        require(
            not self.a.path("nested").exists(),
            "Remote directory move left the old path",
        )
        require(
            self.a.remote("moved/renamed.txt")[0] == identity,
            "Directory move replaced child ID",
        )

    def conflict(self) -> None:
        self.a.path("conflict.txt").write_bytes(b"keep-local\n")
        self.b.path("conflict.txt").write_bytes(b"keep-cloud\n")
        self.b.sync()

        def blocked() -> subprocess.CompletedProcess[str] | None:
            completed = self.a.sync("download_only", expected=(0, 3))
            return completed if completed.returncode == 3 else None

        completed = wait_for_condition(
            blocked,
            self.timeout,
            "a local-modification conflict",
        )
        assert isinstance(completed, subprocess.CompletedProcess)
        require(
            any(
                event.get("event") == "item_blocked"
                and event.get("path") == self.scope + "/conflict.txt"
                for event in parse_events(completed)
            ),
            "Conflict did not identify the expected file",
        )
        require(
            self.a.path("conflict.txt").read_bytes() == b"keep-local\n",
            "Conflict overwrote local bytes",
        )
        blocked_report = self.a.run("inspect", "blocked")
        require(
            section(blocked_report, "blocked_item").get("remote_path")
            == self.scope + "/conflict.txt"
            and section(blocked_report, "blocked_item_summary").get("recorded")
            == "1",
            "inspect blocked did not report the active conflict",
        )
        self.b.sync("download_only")
        require(
            self.b.path("conflict.txt").read_bytes() == b"keep-cloud\n",
            "Conflict overwrote cloud bytes",
        )
        self.a.path("conflict.txt").unlink()
        self.a.run("transfer", "restore", self.scope + "/conflict.txt")
        require(
            self.a.path("conflict.txt").read_bytes() == b"keep-cloud\n",
            "Conflict recovery failed",
        )
        self.a.sync("download_only")
        cleared_report = self.a.run("inspect", "blocked")
        require(
            section(cleared_report, "blocked_item_summary").get("recorded")
            == "0",
            "Recovered conflict remained in inspect blocked",
        )

    def restore(self) -> None:
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
        require(
            not self.a.path("moved/renamed.txt").exists(),
            "Preview restored a file",
        )
        require(
            not self.a.path("empty-dir").exists(),
            "Preview restored a directory",
        )
        require(
            cursor == self.a.cursor(),
            "Restore preview advanced or consumed delta state",
        )
        self.a.run("transfer", "restore", self.scope)
        require(
            self.a.path("moved/renamed.txt").read_bytes()
            == self.UNICODE_TEXT.encode(),
            "Restore bytes differ",
        )
        require(
            self.a.path("empty-dir").is_dir(),
            "Restore omitted the empty directory",
        )
        require(
            self.a.path("preserve.txt").read_bytes()
            == b"leave-existing-local-alone\n",
            "Restore overwrote an existing file",
        )
        require(
            cursor == self.a.cursor(),
            "Restore execution advanced or consumed delta state",
        )
        self.b.sync("download_only")
        require(
            self.b.remote("preserve.txt") == remote,
            "Restore modified the cloud file",
        )
        require(
            self.b.path("preserve.txt").read_bytes() == b"cloud-original\n",
            "Restore uploaded local bytes",
        )
        self.a.path("preserve.txt").write_bytes(b"cloud-original\n")

    def busy_restore(self, process: subprocess.Popen[bytes]) -> None:
        for flags in ((), ("--dry-run",)):
            completed = self.a.run(
                "transfer",
                "restore",
                self.scope,
                *flags,
                expected=(1,),
            )
            require(
                any(
                    event.get("event") == "restore_busy"
                    and str(process.pid) in str(event.get("message", ""))
                    for event in parse_events(completed)
                ),
                "Busy restore omitted its structured refusal/owner PID",
            )
            require(
                process.poll() is None,
                "Restore stopped the owning sync/watch process",
            )

    def interrupted_sync(self) -> None:
        payload = bytes(range(256)) * 32768
        self.b.path("resume.bin").write_bytes(payload)
        self.b.sync()
        self.a.configure(mode="download_only", rate=65536)
        process = self.a.start("sync")

        def partial() -> tuple[object, ...] | None:
            require(
                process.poll() is None,
                "Slow sync exited before its durable checkpoint",
            )
            records = self.a.rows(
                "SELECT temporary_path,completed_bytes FROM partial_download WHERE remote_path=?",
                (self.scope + "/resume.bin",),
            )
            return (
                records[0]
                if records and 0 < int(records[0][1]) < len(payload)
                else None
            )

        wait_for_condition(partial, self.timeout, "a durable partial download")
        self.busy_restore(process)
        process.send_signal(signal.SIGTERM)
        process.wait(timeout=10)
        records = self.a.rows(
            "SELECT temporary_path,completed_bytes FROM partial_download WHERE remote_path=?",
            (self.scope + "/resume.bin",),
        )
        require(
            len(records) == 1 and int(records[0][1]) > 0,
            "Interrupted sync lost its checkpoint",
        )
        partials_report = self.a.run("inspect", "partials")
        require(
            section(partials_report, "partial_download").get("remote_path")
            == self.scope + "/resume.bin"
            and section(partials_report, "partial_download").get("status")
            == "resumable"
            and section(
                partials_report, "partial_download_summary"
            ).get("resumable")
            == "1",
            "inspect partials did not report the resumable checkpoint",
        )
        temporary = Path(str(records[0][0]))
        require(
            temporary.resolve().is_relative_to(self.a.root.resolve()),
            "Partial escaped the isolated Drive",
        )
        with temporary.open("rb") as checkpoint:
            original = os.fstat(checkpoint.fileno())
            self.a.configure(rate=0)
            self.a.sync("download_only")
            installed = self.a.path("resume.bin").stat()
            require(
                (original.st_dev, original.st_ino)
                == (installed.st_dev, installed.st_ino),
                "Download restarted in a new file instead of reusing its checkpoint",
            )
        require(
            self.a.path("resume.bin").read_bytes() == payload,
            "Resumed content differs",
        )
        require(
            not self.a.rows("SELECT remote_id FROM partial_download"),
            "Completed resume left a partial-download record",
        )
        cleared_partials = self.a.run("inspect", "partials")
        require(
            section(cleared_partials, "partial_download_summary").get(
                "recorded"
            )
            == "0",
            "Completed resume remained in inspect partials",
        )

    def watch(self, *, websocket: bool) -> None:
        self.a.configure(mode="bidirectional", websocket=websocket)
        process = self.a.start("watch")

        def ready() -> bool:
            require(
                process.poll() is None,
                "Watch exited before its control socket was ready",
            )
            completed = self.a.run("monitor", "status", expected=(0, 1))
            if completed.returncode != 0:
                return False
            reply = section(completed, "monitor_control")
            return (
                reply.get("pid") == str(process.pid)
                and int(str(reply.get("completed_rounds", "0"))) > 0
                and reply.get("last_result") == "0"
            )

        wait_for_condition(ready, self.timeout, "a successful watch round")
        self.busy_restore(process)
        triggered = self.a.run("monitor", "trigger")
        require(
            bool(section(triggered, "monitor_control").get("acknowledgement")),
            "monitor trigger did not acknowledge the request",
        )
        name = (
            "watch-notifications.txt" if websocket else "watch-polling.txt"
        )
        self.b.path(name).write_bytes(b"remote-watch\n")
        self.b.sync()

        def received() -> bool:
            require(
                process.poll() is None,
                "Watch exited while awaiting a remote change",
            )
            return (
                self.a.path(name).is_file()
                and self.a.path(name).read_bytes() == b"remote-watch\n"
            )

        wait_for_condition(received, self.timeout, "automatic watch download")
        staged = self.workspace / f".{name}.staged"
        staged.write_bytes(b"local-watch\n")
        os.replace(staged, self.a.path(name))
        self.b.pull_until(
            lambda: self.b.path(name).read_bytes() == b"local-watch\n"
        )
        self.a.run("monitor", "stop")
        result = process.wait(timeout=self.timeout)
        require(result in (0, 130), "Watch did not stop cleanly")
        self.a.assert_scope()

    def deletions(self) -> None:
        self.a.path("local-delete.txt").unlink()
        self.a.sync()
        self.b.pull_until(
            lambda: not self.b.path("local-delete.txt").exists()
        )
        require(
            not self.b.rows(
                "SELECT remote_id FROM item WHERE remote_path=?",
                (self.scope + "/local-delete.txt",),
            ),
            "Local deletion left a remote snapshot",
        )
        self.b.path("remote-delete.txt").unlink()
        self.b.sync()
        self.a.pull_until(
            lambda: not self.a.path("remote-delete.txt").exists()
        )
        require(
            not self.a.rows(
                "SELECT remote_id FROM item WHERE remote_path=?",
                (self.scope + "/remote-delete.txt",),
            ),
            "Remote deletion left a local snapshot",
        )

    def cleanup(self) -> None:
        if not self.upload_attempted:
            return
        client_cls = self._client_class()
        credentials = self._snapshot_credentials(self.b.state)
        cleaner = client_cls(
            self.workspace,
            "cleanup",
            self.source,
            credentials,
            self.drive,
            self.scope,
            self.executable,
            self.timeout,
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
        verifier = client_cls(
            self.workspace,
            "cleanup-verifier",
            self.source,
            self._snapshot_credentials(cleaner.state),
            self.drive,
            self.scope,
            self.executable,
            self.timeout,
        )
        verifier.sync("download_only")
        require(
            not verifier.path().exists(),
            "Remote test subtree still exists after cleanup",
        )

    def run(self) -> None:
        scenarios = [
            (self.IDENTITY_SCENARIO, self.identity),
            (
                "upload-download-unicode-empty-files-and-directories",
                self.roundtrip,
            ),
            (
                "cli-discovery-quota-inspection-and-state-diagnostics",
                self.inspection_and_diagnostics,
            ),
            ("unchanged-sync-is-idempotent", self.idempotence),
            (
                "sync-dry-run-upload-only-and-single-file-download",
                self.modes_and_single_download,
            ),
            ("local-file-and-remote-directory-moves", self.moves),
            ("local-conflict-and-explicit-recovery", self.conflict),
            ("restore-preview-execution-and-no-overwrite", self.restore),
            (
                "sync-busy-refusal-interruption-and-resume",
                self.interrupted_sync,
            ),
            (
                "watch-notification-enabled-compatibility-and-busy-refusal",
                lambda: self.watch(websocket=True),
            ),
            (
                "watch-with-explicit-polling-fallback",
                lambda: self.watch(websocket=False),
            ),
            ("local-and-remote-deletion-propagation", self.deletions),
        ]
        print(f"Remote test subtree: {self.scope}", flush=True)
        errors: list[BaseException] = []
        interrupted = False
        try:
            for index, (name, scenario) in enumerate(scenarios, 1):
                print(f"[{index}/{len(scenarios)}] {name}", flush=True)
                scenario()
        except (
            E2EError,
            OSError,
            sqlite3.Error,
            subprocess.SubprocessError,
        ) as error:
            errors.append(error)
        except KeyboardInterrupt:
            interrupted = True
        finally:
            for client in (self.a, self.b):
                try:
                    client.stop_all()
                except (
                    E2EError,
                    OSError,
                    subprocess.SubprocessError,
                ) as error:
                    errors.append(error)
            if any(
                process.poll() is None
                for client in (self.a, self.b)
                for process, _ in client.processes
            ):
                errors.append(
                    E2EError(
                        f"Remote cleanup refused while a test process is running: {self.scope}"
                    )
                )
            else:
                try:
                    self.cleanup()
                except (
                    E2EError,
                    OSError,
                    sqlite3.Error,
                    subprocess.SubprocessError,
                ) as error:
                    errors.append(
                        E2EError(
                            f"Remote cleanup failed for {self.scope}: {error}"
                        )
                    )
        if errors:
            raise E2EError("\n".join(str(error) for error in errors))
        if interrupted:
            raise KeyboardInterrupt
        print(
            f"PASS: {len(scenarios)} {self.SUITE_LABEL} scenarios; remote subtree removed and verified.",
            flush=True,
        )


def run_drive_e2e_cli(
    argv: list[str] | None,
    *,
    description: str | None,
    drive_help: str,
    temp_prefix: str,
    cancelled_label: str,
    runner_cls: type[DualClientDriveE2E],
    select_drives_fn: Callable[
        [dict[str, object], str | None, Path, str],
        str | list[str],
    ],
) -> int:
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument(
        "--client",
        type=Path,
        default=DEFAULT_CLIENT_PATH,
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=DEFAULT_CONFIG_PATH,
    )
    parser.add_argument("--drive", help=drive_help)
    parser.add_argument(
        "--timeout",
        type=int,
        default=600,
        help="Command/eventual-consistency timeout in seconds",
    )
    parser.add_argument(
        "--allow-remote-writes",
        action="store_true",
        help="Permit creating, changing and deleting only the random test subtree",
    )
    arguments = parser.parse_args(argv)
    if not arguments.allow_remote_writes:
        parser.error(
            "Live E2E requires --allow-remote-writes; no files or credentials were read"
        )
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        executable = arguments.client.resolve(strict=True)
        require(
            executable.is_file() and os.access(executable, os.X_OK),
            "CLI is not executable",
        )
        config_path = arguments.config.resolve(strict=True)
        with config_path.open("rb") as source_file:
            source = tomllib.load(source_file)
        require(
            source.get("config_version") == 2,
            "Source config must have config_version = 2",
        )
        for table in ("sync", "state", "proxy"):
            require(
                isinstance(source.get(table, {}), dict),
                f"{table} must be a table",
            )
        proxy = source.get("proxy", {})
        if isinstance(proxy, dict):
            for key in ("password_file", "ca_file"):
                if key in proxy:
                    require(
                        isinstance(proxy[key], str) and bool(proxy[key]),
                        f"proxy.{key} must be a path",
                    )
                    proxy[key] = str(
                        (config_path.parent / str(proxy[key])).resolve(
                            strict=True
                        )
                    )
        state_table = source.get("state", {})
        sync_table = source.get("sync", {})
        state_value = (
            state_table.get(
                "directory",
                str(Path.home() / ".local/state/onedrive-cpp"),
            )
            if isinstance(state_table, dict)
            else None
        )
        data_value = (
            sync_table.get("data_directory", str(Path.home() / "OneDrive"))
            if isinstance(sync_table, dict)
            else None
        )
        require(
            isinstance(state_value, str)
            and bool(state_value)
            and isinstance(data_value, str)
            and bool(data_value),
            "Source state and synchronization directories must be paths",
        )
        state = Path(str(state_value)).expanduser().absolute()
        data = Path(str(data_value)).expanduser().resolve()
        module = sys.modules.get(runner_cls.__module__, sys.modules[__name__])
        snapshot_fn = getattr(
            module,
            "credentials_snapshot",
            credentials_snapshot,
        )
        credentials = snapshot_fn(state)
        selected = select_drives_fn(
            source,
            arguments.drive,
            state,
            credentials[0],
        )
        with tempfile.TemporaryDirectory(
            prefix=temp_prefix,
            dir="/tmp",
        ) as temporary:
            workspace = Path(temporary)
            require(
                not workspace.is_relative_to(data)
                and not data.is_relative_to(workspace)
                and not workspace.is_relative_to(state.resolve())
                and not state.resolve().is_relative_to(workspace),
                "Test workspace overlaps the source state or synchronization directory",
            )
            runner_cls(
                workspace,
                source,
                credentials,
                selected,
                executable,
                arguments.timeout,
            ).run()
    except KeyboardInterrupt:
        print(f"CANCELLED: {cancelled_label} interrupted.", file=sys.stderr)
        return 130
    except (
        E2EError,
        OSError,
        ValueError,
        sqlite3.Error,
        subprocess.SubprocessError,
    ) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0
