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
) -> str:
    replacements = {
        ("sync", "directory"): json.dumps(str(sync_directory)),
        ("sync", "dry_run"): "false",
        ("state", "directory"): json.dumps(str(state_directory)),
    }
    if sync_list is not None:
        replacements[("sync", "sync_list")] = json.dumps(str(sync_list))
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
    sync_list_key = ("sync", "sync_list")
    if sync_list is not None and sync_list_key not in seen:
        if sync_insert_index is None:
            raise E2EError("configuration is missing required table: sync")
        output.insert(
            sync_insert_index,
            f"sync_list = {replacements[sync_list_key]}",
        )
        seen.add(sync_list_key)
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


def materialized_files(sync_directory: Path) -> list[Path]:
    return sorted(
        path
        for path in sync_directory.rglob("*")
        if path.is_file() and not path.is_symlink()
    )


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
    )
    parsed = tomllib.loads(rewritten)
    if (
        parsed["sync"]["directory"] != "/new/sync"
        or parsed["sync"]["dry_run"] is not False
        or parsed["sync"]["sync_list"] != "/new/sync_list"
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
                "CREATE TABLE item (remote_path TEXT NOT NULL)"
            )
            connection.execute(
                "INSERT INTO item (remote_path) VALUES (?)",
                ("fixture/small.txt",),
            )
        if tracked_item_count(state, Path("fixture/small.txt")) != 1:
            raise E2EError("item database lookup self-test failed")
        if materialized_files(root) != sorted([expected, database]):
            raise E2EError("materialized file lookup self-test failed")


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
