#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import tomllib


class E2EError(RuntimeError):
    pass


def rewrite_config(source: str, sync_directory: Path, state_directory: Path) -> str:
    replacements = {
        ("sync", "directory"): json.dumps(str(sync_directory)),
        ("sync", "dry_run"): "false",
        ("state", "directory"): json.dumps(str(state_directory)),
    }
    seen: set[tuple[str, str]] = set()
    table = ""
    output: list[str] = []
    table_pattern = re.compile(r"^\s*\[([A-Za-z0-9_.-]+)\]\s*(?:#.*)?$")
    key_pattern = re.compile(r"^(\s*)([A-Za-z0-9_-]+)\s*=.*$")
    for line in source.splitlines():
        table_match = table_pattern.match(line)
        if table_match:
            table = table_match.group(1)
            output.append(line)
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


def fixture_path(root: Path, relative: Path) -> Path:
    if relative.is_absolute() or ".." in relative.parts or not relative.parts:
        raise E2EError("ONEDRIVE_E2E_EXPECTED_PATH must be a safe relative path")
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
        log_file = workspace / "onedrive-cpp.log"
        home.mkdir()
        shutil.copytree(source_state, state_directory)
        config.write_text(
            rewrite_config(source_text, sync_directory, state_directory),
            encoding="utf-8",
        )
        try:
            reset = reset_copied_state(client, config, home)
            completed.append(reset)
            if reset.returncode != 0:
                raise E2EError(
                    f"isolated state reset failed with {reset.returncode}"
                )
            first = run_sync(client, config, home, log_file)
            completed.append(first)
            if first.returncode != 0:
                raise E2EError(
                    f"initial live Graph synchronization failed with {first.returncode}"
                )
            downloaded = fixture_path(sync_directory, expected_path)
            actual_sha256 = sha256(downloaded)
            if actual_sha256 != expected_sha256:
                raise E2EError(
                    f"downloaded fixture SHA-256 mismatch: {actual_sha256}"
                )
            initial_stat = downloaded.stat()

            second = run_sync(client, config, home, log_file)
            completed.append(second)
            if second.returncode != 0:
                raise E2EError(
                    f"repeat live Graph synchronization failed with {second.returncode}"
                )
            repeated = fixture_path(sync_directory, expected_path)
            repeated_stat = repeated.stat()
            if sha256(repeated) != expected_sha256:
                raise E2EError("fixture changed after repeat synchronization")
            if (
                repeated_stat.st_ino != initial_stat.st_ino
                or repeated_stat.st_mtime_ns != initial_stat.st_mtime_ns
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
    rewritten = rewrite_config(source, Path("/new/sync"), Path("/new/state"))
    parsed = tomllib.loads(rewritten)
    if (
        parsed["sync"]["directory"] != "/new/sync"
        or parsed["sync"]["dry_run"] is not False
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
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        expected = root / "account" / "fixture" / "small.txt"
        expected.parent.mkdir(parents=True)
        expected.write_text("fixture", encoding="utf-8")
        if fixture_path(root, Path("fixture/small.txt")) != expected:
            raise E2EError("fixture lookup self-test failed")
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
