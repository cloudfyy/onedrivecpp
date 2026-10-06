from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import tomllib


class E2EError(RuntimeError):
    pass


@dataclass(frozen=True)
class LiveSettings:
    config_path: Path
    source_text: str
    source_config: dict[str, object]
    source_state: Path
    expected_path: Path
    expected_sha256: str


def load_live_settings(client: Path) -> LiveSettings:
    config_path = Path(required_environment("ONEDRIVE_E2E_CONFIG")).expanduser()
    expected_path = Path(required_environment("ONEDRIVE_E2E_EXPECTED_PATH"))
    validate_expected_path(expected_path)
    expected_sha256 = required_environment(
        "ONEDRIVE_E2E_EXPECTED_SHA256"
    ).lower()
    if not client.is_file():
        raise E2EError(f"onedrive-cpp executable does not exist: {client}")
    if not config_path.is_file():
        raise E2EError(f"E2E configuration does not exist: {config_path}")
    if not re.fullmatch(r"[0-9a-f]{64}", expected_sha256):
        raise E2EError(
            "ONEDRIVE_E2E_EXPECTED_SHA256 must be 64 lowercase hex digits"
        )
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
    return LiveSettings(
        config_path=config_path,
        source_text=source_text,
        source_config=source_config,
        source_state=source_state,
        expected_path=expected_path,
        expected_sha256=expected_sha256,
    )


def rewrite_config(
    source: str,
    sync_data_directory: Path,
    state_directory: Path,
    sync_list: Path | None = None,
    local_conflict: str | None = None,
    sync_root_files: bool | None = None,
    proxy_url: str | None = None,
    monitor_poll_interval_seconds: int | None = None,
    monitor_settle_delay_milliseconds: int | None = None,
    upload_maximum_rate_bytes_per_second: int | None = None,
) -> str:
    replacements = {
        ("sync", "data_directory"): json.dumps(str(sync_data_directory)),
        ("sync", "dry_run"): "false",
        ("state", "directory"): json.dumps(str(state_directory)),
    }
    insert_missing: set[tuple[str, str]] = set()
    if sync_list is not None:
        replacements[("sync", "sync_list")] = json.dumps(str(sync_list))
        insert_missing.add(("sync", "sync_list"))
    if local_conflict is not None:
        replacements[("sync", "local_conflict")] = json.dumps(local_conflict)
        insert_missing.add(("sync", "local_conflict"))
    if sync_root_files is not None:
        replacements[("sync", "sync_root_files")] = json.dumps(sync_root_files)
        insert_missing.add(("sync", "sync_root_files"))
    if proxy_url is not None:
        replacements[("proxy", "url")] = json.dumps(proxy_url)
        replacements[("proxy", "no_proxy")] = "[]"
        insert_missing.update({("proxy", "url"), ("proxy", "no_proxy")})
    if monitor_poll_interval_seconds is not None:
        replacements[("monitor", "poll_interval_seconds")] = str(
            monitor_poll_interval_seconds
        )
        insert_missing.add(("monitor", "poll_interval_seconds"))
    if monitor_settle_delay_milliseconds is not None:
        replacements[("monitor", "settle_delay_milliseconds")] = str(
            monitor_settle_delay_milliseconds
        )
        insert_missing.add(("monitor", "settle_delay_milliseconds"))
    if upload_maximum_rate_bytes_per_second is not None:
        replacements[("upload", "maximum_rate_bytes_per_second")] = str(
            upload_maximum_rate_bytes_per_second
        )
        insert_missing.add(("upload", "maximum_rate_bytes_per_second"))
    seen: set[tuple[str, str]] = set()
    seen_tables: set[str] = set()
    table = ""
    output: list[str] = []
    table_pattern = re.compile(r"^\s*\[([A-Za-z0-9_.-]+)\]\s*(?:#.*)?$")
    key_pattern = re.compile(r"^(\s*)([A-Za-z0-9_-]+)\s*=.*$")

    def append_missing(table_name: str) -> None:
        for key in replacements:
            if (
                key[0] == table_name
                and key in insert_missing
                and key not in seen
            ):
                output.append(f"{key[1]} = {replacements[key]}")
                seen.add(key)

    for line in source.splitlines():
        table_match = table_pattern.match(line)
        if table_match:
            append_missing(table)
            table = table_match.group(1)
            seen_tables.add(table)
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
    append_missing(table)
    missing_tables = {
        key[0]
        for key in insert_missing - seen
        if key[0] not in seen_tables
    }
    for table_name in sorted(missing_tables):
        if output and output[-1]:
            output.append("")
        output.append(f"[{table_name}]")
        seen_tables.add(table_name)
        append_missing(table_name)
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


def sync_list_subtree_rule(relative: Path) -> str:
    validate_expected_path(relative)
    return f"/{relative.as_posix()}/**\n"


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


def sync_root_for_fixture(path: Path, relative: Path) -> Path:
    validate_expected_path(relative)
    if (
        len(path.parts) < len(relative.parts)
        or path.parts[-len(relative.parts):] != relative.parts
    ):
        raise E2EError(f"fixture path does not end with '{relative}'")
    return path.parents[len(relative.parts) - 1]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_pattern_file(path: Path, size: int) -> str:
    if size <= 250_000_000:
        raise E2EError(
            "upload-session E2E file must exceed the 250 MB threshold"
        )
    pattern = bytes(range(256)) * 4096
    digest = hashlib.sha256()
    remaining = size
    with path.open("wb") as stream:
        while remaining:
            chunk = pattern[:min(len(pattern), remaining)]
            stream.write(chunk)
            digest.update(chunk)
            remaining -= len(chunk)
        stream.flush()
        os.fsync(stream.fileno())
    return digest.hexdigest()


def large_upload_size() -> int:
    try:
        size = int(
            os.environ.get(
                "ONEDRIVE_E2E_LARGE_UPLOAD_BYTES",
                "250000001",
            )
        )
    except ValueError as error:
        raise E2EError(
            "ONEDRIVE_E2E_LARGE_UPLOAD_BYTES must be an integer"
        ) from error
    if size > 1_000_000_000:
        raise E2EError("ONEDRIVE_E2E_LARGE_UPLOAD_BYTES exceeds 1 GB")
    return size
