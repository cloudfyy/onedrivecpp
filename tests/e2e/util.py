from __future__ import annotations

from collections.abc import Callable
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

from .base import E2EError


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


def json_event_count(
    result: subprocess.CompletedProcess[str],
    event: str,
) -> int:
    count = 0
    for line in result.stdout.splitlines():
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and value.get("event") == event:
            count += 1
    return count


def has_json_event(result: subprocess.CompletedProcess[str], event: str) -> bool:
    return json_event_count(result, event) != 0


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


def wait_until(
    predicate: Callable[[], bool],
    timeout: float,
    interval: float = 0.05,
) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return bool(predicate())


def client_environment(
    home: Path,
    overrides: dict[str, str] | None = None,
) -> dict[str, str]:
    environment = os.environ.copy()
    environment["HOME"] = str(home)
    if overrides is not None:
        environment.update(overrides)
    return environment
