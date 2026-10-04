#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import tempfile
import tomllib


class E2EError(RuntimeError):
    pass


def rewrite_config(
    source: str,
    sync_directory: Path,
    state_directory: Path,
    sync_list: Path | None = None,
    local_conflict: str | None = None,
    sync_root_files: bool | None = None,
) -> str:
    replacements = {
        ("sync", "directory"): json.dumps(str(sync_directory)),
        ("sync", "dry_run"): "false",
        ("state", "directory"): json.dumps(str(state_directory)),
    }
    if sync_list is not None:
        replacements[("sync", "sync_list")] = json.dumps(str(sync_list))
    if local_conflict is not None:
        replacements[("sync", "local_conflict")] = json.dumps(local_conflict)
    if sync_root_files is not None:
        replacements[("sync", "sync_root_files")] = json.dumps(sync_root_files)
    seen: set[tuple[str, str]] = set()
    table = ""
    output: list[str] = []
    sync_insert_index: int | None = None
    table_pattern = re.compile(r"^\s*\[([A-Za-z0-9_.-]+)\]\s*(?:#.*)?$")
    key_pattern = re.compile(r"^(\s*)([A-Za-z0-9_-]+)\s*=.*$")
    for line in source.splitlines():
        table_match = table_pattern.match(line)
        if table_match:
            table = table_match.group(1)
            output.append(line)
            if table == "sync":
                sync_insert_index = len(output)
            continue
        key_match = key_pattern.match(line)
        key = (table, key_match.group(2)) if key_match else None
        if key in replacements:
            if key in seen:
                raise E2EError(f"duplicate configuration key: {key[0]}.{key[1]}")
            seen.add(key)
            output.append(
                f"{key_match.group(1)}{key_match.group(2)} = {replacements[key]}"
            )
        else:
            output.append(line)
    optional_sync_keys = [
        key
        for key in (
            ("sync", "sync_list"),
            ("sync", "local_conflict"),
            ("sync", "sync_root_files"),
        )
        if key in replacements and key not in seen
    ]
    if optional_sync_keys and sync_insert_index is None:
        raise E2EError("configuration is missing required table: sync")
    for key in reversed(optional_sync_keys):
        output.insert(
            sync_insert_index,
            f"{key[1]} = {replacements[key]}",
        )
        seen.add(key)
    missing = set(replacements) - seen
    if missing:
        names = ", ".join(f"{table}.{key}" for table, key in sorted(missing))
        raise E2EError(f"configuration is missing required keys: {names}")
    return "\n".join(output) + "\n"


def required_environment(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise E2EError(f"{name} must be set for the live Graph E2E test")
    return value


def validate_expected_path(relative: Path) -> None:
    if relative.is_absolute() or ".." in relative.parts or not relative.parts:
        raise E2EError("ONEDRIVE_E2E_EXPECTED_PATH must be a safe relative path")


def sync_list_rule(relative: Path) -> str:
    validate_expected_path(relative)
    return f"/{relative.as_posix()}\n"


def fixture_path(root: Path, relative: Path) -> Path:
    validate_expected_path(relative)
    candidates = [
        path
        for path in root.rglob(relative.name)
        if path.is_file() and not path.is_symlink() and
        path.relative_to(root).parts[-len(relative.parts):] == relative.parts
    ]
    if len(candidates) != 1:
        raise E2EError(
            f"expected exactly one downloaded '{relative}', found {len(candidates)}"
        )
    return candidates[0]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tracked_item_count(state_directory: Path, remote_path: Path) -> int:
    databases = list(state_directory.rglob("items.sqlite3"))
    if not databases:
        raise E2EError("isolated state contains no item database")
    count = 0
    for database in databases:
        try:
            with sqlite3.connect(database) as connection:
                row = connection.execute(
                    "SELECT count(*) FROM item WHERE remote_path = ?",
                    (remote_path.as_posix(),),
                ).fetchone()
        except sqlite3.Error as error:
            raise E2EError(f"cannot query item database: {database}") from error
        if row is None:
            raise E2EError(f"cannot query item database: {database}")
        count += int(row[0])
    return count


def inject_disappeared_item(
    state_directory: Path,
    source: Path,
    source_remote_path: Path,
) -> tuple[Path, Path]:
    fake_remote_path = (
        source_remote_path.parent /
        "__onedrive_cpp_remote_delete_e2e__.txt"
    )
    fake_local_path = (
        source.parent /
        "__onedrive_cpp_remote_delete_e2e__.txt"
    )
    matches: list[tuple[Path, tuple[object, ...]]] = []
    for database in state_directory.rglob("items.sqlite3"):
        try:
            with sqlite3.connect(database) as connection:
                rows = connection.execute(
                    "SELECT drive_id, parent_id, etag, last_modified, size, "
                    "local_size, local_modified_ticks "
                    "FROM item WHERE remote_path = ?",
                    (source_remote_path.as_posix(),),
                ).fetchall()
        except sqlite3.Error as error:
            raise E2EError(
                f"cannot prepare remote deletion fixture: {database}"
            ) from error
        matches.extend((database, row) for row in rows)
    if len(matches) != 1:
        raise E2EError(
            "remote deletion fixture requires exactly one source snapshot"
        )

    database, row = matches[0]
    shutil.copy2(source, fake_local_path)
    try:
        with sqlite3.connect(database) as connection:
            existing = connection.execute(
                "SELECT count(*) FROM item "
                "WHERE remote_id = ? OR remote_path = ?",
                (
                    "__onedrive_cpp_remote_delete_e2e__",
                    fake_remote_path.as_posix(),
                ),
            ).fetchone()
            if existing is None or int(existing[0]) != 0:
                raise E2EError(
                    "remote deletion fixture unexpectedly already exists"
                )
            connection.execute(
                "INSERT INTO item ("
                "drive_id, remote_id, parent_id, name, etag, remote_path, "
                "local_path, last_modified, size, local_size, "
                "local_modified_ticks, directory"
                ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0)",
                (
                    row[0],
                    "__onedrive_cpp_remote_delete_e2e__",
                    row[1],
                    fake_local_path.name,
                    row[2],
                    fake_remote_path.as_posix(),
                    str(fake_local_path),
                    row[3],
                    row[4],
                    row[5],
                    row[6],
                ),
            )
    except sqlite3.Error as error:
        raise E2EError(
            f"cannot inject remote deletion fixture: {database}"
        ) from error
    return fake_local_path, fake_remote_path


def materialized_files(sync_directory: Path) -> list[Path]:
    return sorted(
        path
        for path in sync_directory.rglob("*")
        if path.is_file() and not path.is_symlink()
    )


def safe_backup_files(destination: Path) -> list[Path]:
    prefix = f"{destination.stem}.safeBackup-"
    suffix = destination.suffix
    return sorted(
        path
        for path in destination.parent.iterdir()
        if path.is_file()
        and not path.is_symlink()
        and path.name.startswith(prefix)
        and path.name.endswith(suffix)
    )


def has_json_event(result: subprocess.CompletedProcess[str], event: str) -> bool:
    for line in result.stdout.splitlines():
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and value.get("event") == event:
            return True
    return False


def log_text_since(log_file: Path, offset: int) -> str:
    if not log_file.is_file():
        raise E2EError("client log file was not created")
    with log_file.open("r", encoding="utf-8") as stream:
        stream.seek(offset)
        return stream.read()


def save_artifacts(
    completed: list[subprocess.CompletedProcess[str]],
    log_file: Path,
) -> None:
    destination_value = os.environ.get("ONEDRIVE_E2E_ARTIFACT_DIR", "").strip()
    if not destination_value:
        return
    destination = Path(destination_value).expanduser().resolve()
    destination.mkdir(parents=True, exist_ok=True)
    for index, result in enumerate(completed, start=1):
        (destination / f"command-{index}.stdout.log").write_text(
            result.stdout,
            encoding="utf-8",
        )
        (destination / f"command-{index}.stderr.log").write_text(
            result.stderr,
            encoding="utf-8",
        )
    if log_file.is_file():
        shutil.copy2(log_file, destination / "onedrive-cpp.log")


def run_client(
    client: Path,
    arguments: list[str],
    home: Path,
    timeout: int,
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["HOME"] = str(home)
    return subprocess.run(
        [str(client), *arguments],
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
        env=environment,
    )


def run_sync(
    client: Path,
    config: Path,
    home: Path,
    log_file: Path,
) -> subprocess.CompletedProcess[str]:
    return run_client(
        client,
        [
            "sync",
            "--config",
            str(config),
            "--color",
            "never",
            "--output",
            "json",
            "--log-level",
            "trace",
            "--log-file",
            str(log_file),
        ],
        home,
        timeout=600,
    )


def run_single_download(
    client: Path,
    config: Path,
    remote_path: Path,
    home: Path,
    log_file: Path,
) -> subprocess.CompletedProcess[str]:
    return run_client(
        client,
        [
            "download",
            remote_path.as_posix(),
            "--config",
            str(config),
            "--color",
            "never",
            "--output",
            "json",
            "--log-level",
            "trace",
            "--log-file",
            str(log_file),
        ],
        home,
        timeout=600,
    )


def reset_copied_state(
    client: Path,
    config: Path,
    home: Path,
) -> subprocess.CompletedProcess[str]:
    return run_client(
        client,
        [
            "reset-state",
            "--config",
            str(config),
            "--clear-all",
            "--yes",
            "--color",
            "never",
            "--output",
            "json",
        ],
        home,
        timeout=60,
    )


def reset_delta_cursor(
    client: Path,
    config: Path,
    home: Path,
) -> subprocess.CompletedProcess[str]:
    return run_client(
        client,
        [
            "reset-state",
            "--config",
            str(config),
            "--color",
            "never",
            "--output",
            "json",
        ],
        home,
        timeout=60,
    )


def run_live(client: Path, work_root: Path) -> None:
    config_path = Path(required_environment("ONEDRIVE_E2E_CONFIG")).expanduser()
    expected_path = Path(required_environment("ONEDRIVE_E2E_EXPECTED_PATH"))
    validate_expected_path(expected_path)
    expected_sha256 = required_environment("ONEDRIVE_E2E_EXPECTED_SHA256").lower()
    if not client.is_file():
        raise E2EError(f"onedrive-cpp executable does not exist: {client}")
    if not config_path.is_file():
        raise E2EError(f"E2E configuration does not exist: {config_path}")
    if not re.fullmatch(r"[0-9a-f]{64}", expected_sha256):
        raise E2EError("ONEDRIVE_E2E_EXPECTED_SHA256 must be 64 lowercase hex digits")

    source_text = config_path.read_text(encoding="utf-8")
    source_config = tomllib.loads(source_text)
    if source_config.get("config_version") != 2:
        raise E2EError("E2E configuration must use config_version = 2")
    try:
        source_state = Path(source_config["state"]["directory"]).expanduser()
    except (KeyError, TypeError) as error:
        raise E2EError("E2E configuration requires state.directory") from error
    if not source_state.is_dir():
        raise E2EError(f"E2E state directory does not exist: {source_state}")

    work_root.mkdir(parents=True, exist_ok=True)
    completed: list[subprocess.CompletedProcess[str]] = []
    with tempfile.TemporaryDirectory(prefix="graph-download-", dir=work_root) as raw:
        workspace = Path(raw)
        sync_directory = workspace / "sync"
        state_directory = workspace / "state"
        home = workspace / "home"
        config = workspace / "config.toml"
        sync_list = workspace / "sync_list"
        log_file = workspace / "onedrive-cpp.log"
        home.mkdir()
        shutil.copytree(source_state, state_directory)
        sync_list.write_text("", encoding="utf-8")
        config.write_text(
            rewrite_config(
                source_text,
                sync_directory,
                state_directory,
                sync_list,
                "backup",
                False,
            ),
            encoding="utf-8",
        )
        try:
            reset = reset_copied_state(client, config, home)
            completed.append(reset)
            if reset.returncode != 0:
                raise E2EError(
                    f"isolated state reset failed with {reset.returncode}"
                )
            single = run_single_download(
                client,
                config,
                expected_path,
                home,
                log_file,
            )
            completed.append(single)
            if single.returncode != 0:
                raise E2EError(
                    f"single-file Graph download failed with {single.returncode}"
                )
            downloaded = fixture_path(sync_directory, expected_path)
            actual_sha256 = sha256(downloaded)
            if actual_sha256 != expected_sha256:
                raise E2EError(
                    f"single-file fixture SHA-256 mismatch: {actual_sha256}"
                )
            initial_stat = downloaded.stat()

            excluded = run_sync(client, config, home, log_file)
            completed.append(excluded)
            if excluded.returncode != 0:
                raise E2EError(
                    "empty-list live Graph synchronization failed with "
                    f"{excluded.returncode}"
                )
            excluded_fixture = fixture_path(sync_directory, expected_path)
            excluded_stat = excluded_fixture.stat()
            if sha256(excluded_fixture) != expected_sha256:
                raise E2EError("fixture changed during empty-list synchronization")
            if (
                excluded_stat.st_ino != initial_stat.st_ino
                or excluded_stat.st_mtime_ns != initial_stat.st_mtime_ns
            ):
                raise E2EError(
                    "empty-list synchronization rewrote the explicit download"
                )
            if tracked_item_count(state_directory, expected_path) != 0:
                raise E2EError(
                    "empty sync list retained the excluded fixture snapshot"
                )

            excluded_fixture.unlink()
            sync_list.write_text(
                sync_list_rule(expected_path),
                encoding="utf-8",
            )
            log_offset = log_file.stat().st_size
            included = run_sync(client, config, home, log_file)
            completed.append(included)
            if included.returncode != 0:
                raise E2EError(
                    "included live Graph synchronization failed with "
                    f"{included.returncode}"
                )
            if (
                "Selective synchronization rules changed; fetching the full "
                "remote state"
                not in log_text_since(log_file, log_offset)
            ):
                raise E2EError(
                    "sync-list change did not report a full remote-state query"
                )
            synchronized = fixture_path(sync_directory, expected_path)
            synchronized_stat = synchronized.stat()
            if sha256(synchronized) != expected_sha256:
                raise E2EError("included fixture SHA-256 mismatch")
            if tracked_item_count(state_directory, expected_path) != 1:
                raise E2EError(
                    "included fixture snapshot was not persisted exactly once"
                )
            if materialized_files(sync_directory) != [synchronized]:
                raise E2EError(
                    "selective synchronization materialized unexpected files"
                )

            repeated_sync = run_sync(client, config, home, log_file)
            completed.append(repeated_sync)
            if repeated_sync.returncode != 0:
                raise E2EError(
                    "repeat live Graph synchronization failed with "
                    f"{repeated_sync.returncode}"
                )
            repeated = fixture_path(sync_directory, expected_path)
            repeated_stat = repeated.stat()
            if sha256(repeated) != expected_sha256:
                raise E2EError("fixture changed after repeat synchronization")
            if (
                repeated_stat.st_ino != synchronized_stat.st_ino
                or repeated_stat.st_mtime_ns != synchronized_stat.st_mtime_ns
            ):
                raise E2EError("repeat synchronization rewrote the unchanged fixture")

            config.write_text(
                rewrite_config(
                    source_text,
                    sync_directory,
                    state_directory,
                    sync_list,
                    "backup",
                    True,
                ),
                encoding="utf-8",
            )
            log_offset = log_file.stat().st_size
            root_files_sync = run_sync(
                client,
                config,
                home,
                log_file,
            )
            completed.append(root_files_sync)
            if root_files_sync.returncode != 0:
                raise E2EError(
                    "root-file live Graph synchronization failed with "
                    f"{root_files_sync.returncode}"
                )
            if (
                "Selective synchronization rules changed; fetching the full "
                "remote state"
                not in log_text_since(log_file, log_offset)
            ):
                raise E2EError(
                    "enabling root files did not report a full remote-state query"
                )
            root_files_fixture = fixture_path(
                sync_directory,
                expected_path,
            )
            root_files_stat = root_files_fixture.stat()
            if sha256(root_files_fixture) != expected_sha256:
                raise E2EError(
                    "fixture changed after enabling root-file synchronization"
                )
            if (
                root_files_stat.st_ino != repeated_stat.st_ino
                or root_files_stat.st_mtime_ns != repeated_stat.st_mtime_ns
            ):
                raise E2EError(
                    "enabling root files rewrote the rule-selected fixture"
                )
            if tracked_item_count(state_directory, expected_path) != 1:
                raise E2EError(
                    "root-file synchronization changed the fixture snapshot"
                )

            conflict_contents = (
                b"onedrive-cpp safeBackup live Graph E2E local conflict\n"
            )
            root_files_fixture.write_bytes(conflict_contents)
            conflict_stat = root_files_fixture.stat()
            reset_for_conflict = reset_copied_state(
                client,
                config,
                home,
            )
            completed.append(reset_for_conflict)
            if reset_for_conflict.returncode != 0:
                raise E2EError(
                    "safeBackup state reset failed with "
                    f"{reset_for_conflict.returncode}"
                )

            backed_up_sync = run_sync(client, config, home, log_file)
            completed.append(backed_up_sync)
            if backed_up_sync.returncode != 0:
                raise E2EError(
                    "safeBackup live Graph synchronization failed with "
                    f"{backed_up_sync.returncode}"
                )
            if not has_json_event(
                backed_up_sync,
                "local_conflict_backed_up",
            ):
                raise E2EError(
                    "safeBackup synchronization did not emit its JSON event"
                )
            restored = fixture_path(sync_directory, expected_path)
            restored_stat = restored.stat()
            if sha256(restored) != expected_sha256:
                raise E2EError(
                    "safeBackup synchronization did not restore remote content"
                )
            if restored_stat.st_ino == conflict_stat.st_ino:
                raise E2EError(
                    "safeBackup synchronization did not install the remote inode"
                )
            backups = safe_backup_files(restored)
            if len(backups) != 1:
                raise E2EError(
                    f"expected one safeBackup file, found {len(backups)}"
                )
            backup = backups[0]
            backup_stat = backup.stat()
            if backup.read_bytes() != conflict_contents:
                raise E2EError(
                    "safeBackup file did not preserve the local conflict"
                )
            if tracked_item_count(state_directory, expected_path) != 1:
                raise E2EError(
                    "safeBackup synchronization did not persist one snapshot"
                )

            stable_after_backup = run_sync(
                client,
                config,
                home,
                log_file,
            )
            completed.append(stable_after_backup)
            if stable_after_backup.returncode != 0:
                raise E2EError(
                    "post-safeBackup incremental synchronization failed with "
                    f"{stable_after_backup.returncode}"
                )
            stable = fixture_path(sync_directory, expected_path)
            stable_stat = stable.stat()
            stable_backups = safe_backup_files(stable)
            if sha256(stable) != expected_sha256:
                raise E2EError(
                    "remote fixture changed after safeBackup synchronization"
                )
            if (
                stable_stat.st_ino != restored_stat.st_ino
                or stable_stat.st_mtime_ns != restored_stat.st_mtime_ns
            ):
                raise E2EError(
                    "post-safeBackup synchronization rewrote the remote fixture"
                )
            if stable_backups != [backup]:
                raise E2EError(
                    "post-safeBackup synchronization created another backup"
                )
            if (
                backup.stat().st_ino != backup_stat.st_ino
                or backup.stat().st_mtime_ns != backup_stat.st_mtime_ns
            ):
                raise E2EError(
                    "post-safeBackup synchronization modified the backup"
                )

            disappeared_local, disappeared_remote = inject_disappeared_item(
                state_directory,
                stable,
                expected_path,
            )
            cursor_reset = reset_delta_cursor(client, config, home)
            completed.append(cursor_reset)
            if cursor_reset.returncode != 0:
                raise E2EError(
                    "remote deletion E2E cursor reset failed with "
                    f"{cursor_reset.returncode}"
                )
            reconciled = run_sync(client, config, home, log_file)
            completed.append(reconciled)
            if reconciled.returncode != 0:
                raise E2EError(
                    "remote deletion full Graph reconciliation failed with "
                    f"{reconciled.returncode}"
                )
            if disappeared_local.exists():
                raise E2EError(
                    "full Graph reconciliation retained a remotely absent file"
                )
            if tracked_item_count(
                state_directory,
                disappeared_remote,
            ) != 0:
                raise E2EError(
                    "full Graph reconciliation retained a disappeared snapshot"
                )
            if not has_json_event(reconciled, "local_item_removed"):
                raise E2EError(
                    "remote deletion reconciliation did not emit its JSON event"
                )
            if sha256(stable) != expected_sha256:
                raise E2EError(
                    "remote deletion reconciliation changed the live fixture"
                )
        except Exception:
            save_artifacts(completed, log_file)
            raise


def self_test() -> None:
    source = """\
config_version = 2
[sync]
directory = "/old/sync"
dry_run = true
[state]
directory = "/old/state"
"""
    rewritten = rewrite_config(
        source,
        Path("/new/sync"),
        Path("/new/state"),
        Path("/new/sync_list"),
        "backup",
        True,
    )
    parsed = tomllib.loads(rewritten)
    if (
        parsed["sync"]["directory"] != "/new/sync"
        or parsed["sync"]["dry_run"] is not False
        or parsed["sync"]["sync_list"] != "/new/sync_list"
        or parsed["sync"]["local_conflict"] != "backup"
        or parsed["sync"]["sync_root_files"] is not True
        or parsed["state"]["directory"] != "/new/state"
    ):
        raise E2EError("configuration rewrite self-test failed")
    try:
        rewrite_config("[sync]\ndirectory = \"/tmp\"\n", Path("/a"), Path("/b"))
        raise E2EError("incomplete configuration was accepted")
    except E2EError as error:
        if "missing required keys" not in str(error):
            raise
    duplicate = source.replace(
        'directory = "/old/sync"',
        'directory = "/old/sync"\ndirectory = "/duplicate"',
    )
    try:
        rewrite_config(duplicate, Path("/a"), Path("/b"))
        raise E2EError("duplicate configuration key was accepted")
    except E2EError as error:
        if "duplicate configuration key" not in str(error):
            raise
    existing_sync_list = source.replace(
        'directory = "/old/sync"',
        'directory = "/old/sync"\nsync_list = "/old/sync_list"',
    )
    replaced_sync_list = tomllib.loads(
        rewrite_config(
            existing_sync_list,
            Path("/new/sync"),
            Path("/new/state"),
            Path("/new/sync_list"),
        )
    )
    if replaced_sync_list["sync"]["sync_list"] != "/new/sync_list":
        raise E2EError("existing selective-sync path was not replaced")
    existing_local_conflict = source.replace(
        'directory = "/old/sync"',
        'directory = "/old/sync"\nlocal_conflict = "block"',
    )
    replaced_local_conflict = tomllib.loads(
        rewrite_config(
            existing_local_conflict,
            Path("/new/sync"),
            Path("/new/state"),
            local_conflict="backup",
        )
    )
    if replaced_local_conflict["sync"]["local_conflict"] != "backup":
        raise E2EError("existing local-conflict policy was not replaced")
    existing_root_files = source.replace(
        'directory = "/old/sync"',
        'directory = "/old/sync"\nsync_root_files = false',
    )
    replaced_root_files = tomllib.loads(
        rewrite_config(
            existing_root_files,
            Path("/new/sync"),
            Path("/new/state"),
            sync_root_files=True,
        )
    )
    if replaced_root_files["sync"]["sync_root_files"] is not True:
        raise E2EError("existing root-file policy was not replaced")
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        expected = root / "account" / "fixture" / "small.txt"
        expected.parent.mkdir(parents=True)
        expected.write_text("fixture", encoding="utf-8")
        if fixture_path(root, Path("fixture/small.txt")) != expected:
            raise E2EError("fixture lookup self-test failed")
        if sync_list_rule(Path("fixture/small.txt")) != "/fixture/small.txt\n":
            raise E2EError("selective-sync rule generation self-test failed")
        try:
            fixture_path(root, Path("../small.txt"))
            raise E2EError("unsafe fixture path was accepted")
        except E2EError as error:
            if "safe relative path" not in str(error):
                raise
        duplicate_fixture = root / "other" / "fixture" / "small.txt"
        duplicate_fixture.parent.mkdir(parents=True)
        duplicate_fixture.write_text("fixture", encoding="utf-8")
        try:
            fixture_path(root, Path("fixture/small.txt"))
            raise E2EError("ambiguous fixture path was accepted")
        except E2EError as error:
            if "found 2" not in str(error):
                raise
        duplicate_fixture.unlink()
        symlink = root / "symlink.txt"
        symlink.symlink_to(expected)
        try:
            fixture_path(root, Path("symlink.txt"))
            raise E2EError("fixture symlink was accepted")
        except E2EError as error:
            if "found 0" not in str(error):
                raise
        state = root / "state"
        database = state / "items.sqlite3"
        state.mkdir()
        with sqlite3.connect(database) as connection:
            connection.execute(
                "CREATE TABLE item ("
                "drive_id TEXT NOT NULL, remote_id TEXT NOT NULL, "
                "parent_id TEXT NOT NULL, name TEXT NOT NULL, "
                "etag TEXT NOT NULL, remote_path TEXT NOT NULL, "
                "local_path TEXT NOT NULL, last_modified TEXT NOT NULL, "
                "size INTEGER NOT NULL, local_size INTEGER NOT NULL, "
                "local_modified_ticks INTEGER NOT NULL, "
                "directory INTEGER NOT NULL)"
            )
            connection.execute(
                "INSERT INTO item VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    "drive",
                    "fixture",
                    "parent",
                    "small.txt",
                    "etag",
                    "fixture/small.txt",
                    str(expected),
                    "2026-10-04T00:00:00Z",
                    7,
                    7,
                    123,
                    0,
                ),
            )
        if tracked_item_count(state, Path("fixture/small.txt")) != 1:
            raise E2EError("item database lookup self-test failed")
        if materialized_files(root) != sorted([expected, database]):
            raise E2EError("materialized file lookup self-test failed")
        backup = expected.with_name(
            "small.safeBackup-20261004T060000Z-0001.txt"
        )
        backup.write_text("local conflict", encoding="utf-8")
        ignored = expected.with_name("small.safeBackup-invalid.bin")
        ignored.write_text("wrong extension", encoding="utf-8")
        backup_symlink = expected.with_name(
            "small.safeBackup-20261004T060000Z-0002.txt"
        )
        backup_symlink.symlink_to(backup)
        if safe_backup_files(expected) != [backup]:
            raise E2EError("safeBackup lookup self-test failed")
        event_result = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout=(
                "not JSON\n"
                '{"event":"local_conflict_backed_up","level":"warning"}\n'
            ),
            stderr="",
        )
        if not has_json_event(event_result, "local_conflict_backed_up"):
            raise E2EError("safeBackup JSON event self-test failed")
        disappeared_local, disappeared_remote = inject_disappeared_item(
            state,
            expected,
            Path("fixture/small.txt"),
        )
        if (
            not disappeared_local.is_file()
            or tracked_item_count(state, disappeared_remote) != 1
        ):
            raise E2EError("remote deletion fixture injection self-test failed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", type=Path)
    parser.add_argument("--work-root", type=Path)
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    try:
        if arguments.self_test:
            self_test()
        else:
            if arguments.client is None or arguments.work_root is None:
                raise E2EError("--client and --work-root are required")
            run_live(arguments.client.resolve(), arguments.work_root.resolve())
    except (E2EError, OSError, subprocess.SubprocessError) as error:
        print(f"Graph download E2E failed: {error}", file=os.sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
