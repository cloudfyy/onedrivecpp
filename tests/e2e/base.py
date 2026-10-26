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


def require(condition: object, message: str) -> None:
    if not condition:
        raise E2EError(message)


DEFAULT_CLIENT_PATH = Path("build/release/onedrive-cpp")
DEFAULT_CONFIG_PATH = Path.home() / ".config" / "onedrive-cpp" / "config.toml"
BASELINE_FIXTURE_BYTES = b"onedrive-cpp live Graph E2E baseline fixture\n"


@dataclass(frozen=True)
class LiveSettings:
    config_path: Path
    source_text: str
    source_config: dict[str, object]
    source_state: Path
    drive_id: str
    large_upload_bytes: int = 250_000_001
    artifact_dir: Path | None = None


def configured_drives(source_config: dict[str, object]) -> list[str]:
    sync = source_config.get("sync", {})
    require(isinstance(sync, dict), "E2E configuration sync table is invalid")
    require(
        not ("drive_id" in sync and "drive_ids" in sync),
        "Conflicting Drive settings",
    )
    drives = sync.get("drive_ids", [sync.get("drive_id", "me")])
    require(
        isinstance(drives, list)
        and bool(drives)
        and all(isinstance(drive, str) and bool(drive) for drive in drives),
        "Invalid configured Drive list",
    )
    return list(dict.fromkeys(drives))


def saved_drive_types(
    source_state: Path,
    marker: str | None = None,
) -> dict[str, str]:
    if marker is None:
        marker_path = source_state / "active_account"
        try:
            if marker_path.is_symlink() or not marker_path.is_file():
                return {}
            marker = marker_path.read_text(encoding="utf-8").strip()
        except OSError:
            return {}
    if not marker or Path(marker).name != marker or marker in {".", ".."}:
        return {}
    account_dir = source_state / "accounts" / marker
    types: dict[str, str] = {}
    account_json = account_dir / "account.json"
    if account_json.is_file() and not account_json.is_symlink():
        try:
            payload = json.loads(account_json.read_text(encoding="utf-8"))
            drive_type = payload.get("drive_type")
            if isinstance(drive_type, str) and drive_type:
                for key in ("configured_drive_id", "drive_id"):
                    value = payload.get(key)
                    if isinstance(value, str) and value:
                        types[value] = drive_type
        except (OSError, UnicodeDecodeError, json.JSONDecodeError):
            pass
    drives_dir = account_dir / "drives"
    if not drives_dir.is_dir() or drives_dir.is_symlink():
        return types
    for entry in drives_dir.iterdir():
        if entry.is_symlink() or not entry.is_dir():
            continue
        metadata_path = entry / "drive.json"
        if not metadata_path.is_file() or metadata_path.is_symlink():
            continue
        try:
            payload = json.loads(metadata_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError):
            continue
        drive_type = payload.get("drive_type")
        if not isinstance(drive_type, str) or not drive_type:
            continue
        for key in ("configured_drive_id", "drive_id"):
            value = payload.get(key)
            if isinstance(value, str) and value:
                types[value] = drive_type
    return types


def select_personal_drive(
    source_config: dict[str, object],
    requested: str | None = None,
    *,
    known_types: dict[str, str] | None = None,
) -> str:
    drives = configured_drives(source_config)
    if requested is not None:
        require(
            requested in drives,
            "--drive must select a configured Drive reference",
        )
        return requested
    if len(drives) == 1:
        return drives[0]
    types = dict(known_types or {})
    personal_matches = [
        drive for drive in drives if types.get(drive) == "personal"
    ]
    if len(personal_matches) == 1:
        return personal_matches[0]
    if len(personal_matches) > 1:
        raise E2EError(
            "Multiple personal Drives configured in drive_ids; "
            "select one with --drive"
        )
    candidates = [
        drive
        for drive in drives
        if drive == "me"
        and types.get(drive) not in ("business", "documentLibrary")
    ]
    if len(candidates) == 1:
        return candidates[0]
    raise E2EError(
        "Multiple Drives configured in drive_ids; "
        "select the personal Drive with --drive"
    )


def select_business_drive(
    source_config: dict[str, object],
    requested: str | None = None,
) -> str:
    drives = configured_drives(source_config)
    if requested is not None:
        require(
            requested in drives,
            "--drive must select a configured Drive reference",
        )
        return requested
    require(
        len(drives) == 1,
        "Multiple Drives configured; select one with --drive",
    )
    return drives[0]


def select_sharepoint_drives(
    source_config: dict[str, object],
    requested: str | None = None,
    *,
    known_types: dict[str, str] | None = None,
) -> list[str]:
    drives = configured_drives(source_config)
    if requested is not None:
        require(
            requested in drives,
            "--drive must select a configured Drive reference",
        )
        return [requested]
    if len(drives) == 1:
        return drives
    types = dict(known_types or {})
    library_matches = [
        drive for drive in drives if types.get(drive) == "documentLibrary"
    ]
    if len(library_matches) == 1:
        return library_matches
    if len(library_matches) > 1:
        raise E2EError(
            "Multiple documentLibrary Drives configured in drive_ids; "
            "select one with --drive"
        )
    candidates = [
        drive
        for drive in drives
        if drive != "me" and types.get(drive) not in ("personal", "business")
    ]
    if candidates:
        return candidates
    raise E2EError(
        "No SharePoint documentLibrary candidate found in configured Drives; "
        "select one with --drive"
    )


def toml_document(document: dict[str, object]) -> str:
    def value(item: object) -> str:
        if isinstance(item, bool):
            return "true" if item else "false"
        if isinstance(item, str):
            return json.dumps(item, ensure_ascii=False)
        if isinstance(item, int):
            return str(item)
        if isinstance(item, list):
            return "[" + ", ".join(value(entry) for entry in item) + "]"
        raise E2EError("Unsupported value in source configuration")

    lines: list[str] = []

    def table(entries: dict[str, object], prefix: tuple[str, ...] = ()) -> None:
        if prefix:
            lines.append(
                "[" + ".".join(json.dumps(key) for key in prefix) + "]"
            )
        for key, entry in entries.items():
            if not isinstance(entry, dict):
                lines.append(f"{json.dumps(key)} = {value(entry)}")
        for key, entry in entries.items():
            if isinstance(entry, dict):
                table(entry, (*prefix, key))

    table(document)
    return "\n".join(lines) + "\n"


def load_live_settings(
    client: Path,
    config_path: Path = DEFAULT_CONFIG_PATH,
    *,
    drive: str | None = None,
    large_upload_bytes: int | str = 250_000_001,
    artifact_dir: Path | None = None,
) -> LiveSettings:
    config_path = config_path.expanduser()
    validated_large_bytes = large_upload_size(large_upload_bytes)
    if not client.is_file():
        raise E2EError(f"onedrive-cpp executable does not exist: {client}")
    if not config_path.is_file():
        raise E2EError(f"E2E configuration does not exist: {config_path}")
    source_text = config_path.read_text(encoding="utf-8")
    source_config = tomllib.loads(source_text)
    if source_config.get("config_version") != 2:
        raise E2EError("E2E configuration must use config_version = 2")
    try:
        state_entry = source_config["state"]["directory"]
        if not isinstance(state_entry, str) or not state_entry:
            raise TypeError("state.directory must be a non-empty string")
        source_state = Path(state_entry).expanduser()
        if not source_state.is_absolute():
            source_state = (config_path.resolve().parent / source_state).resolve()
    except (KeyError, TypeError) as error:
        raise E2EError("E2E configuration requires state.directory") from error
    if not source_state.is_dir():
        raise E2EError(f"E2E state directory does not exist: {source_state}")
    selected_drive = select_personal_drive(
        source_config,
        drive,
        known_types=saved_drive_types(source_state),
    )
    return LiveSettings(
        config_path=config_path.resolve(),
        source_text=source_text,
        source_config=source_config,
        source_state=source_state,
        drive_id=selected_drive,
        large_upload_bytes=validated_large_bytes,
        artifact_dir=(
            artifact_dir.expanduser().resolve()
            if artifact_dir is not None
            else None
        ),
    )


def rewrite_config(
    source: str,
    sync_data_directory: Path,
    state_directory: Path,
    sync_list: Path | None = None,
    local_conflict: str | None = None,
    sync_root_files: bool | None = None,
    dry_run: bool = False,
    sync_mode: str | None = None,
    proxy_url: str | None = None,
    monitor_poll_interval_seconds: int | None = None,
    monitor_settle_delay_milliseconds: int | None = None,
    upload_maximum_rate_bytes_per_second: int | None = None,
    drive_id: str | None = None,
) -> str:
    mount_point = sync_data_directory.resolve()
    while not os.path.ismount(mount_point):
        parent = mount_point.parent
        if parent == mount_point:
            raise E2EError(
                f"cannot determine mount point for {sync_data_directory}"
            )
        mount_point = parent
    replacements = {
        ("sync", "data_directory"): json.dumps(str(sync_data_directory)),
        ("sync", "data_mount_point"): json.dumps(str(mount_point)),
        ("sync", "dry_run"): json.dumps(dry_run),
        ("state", "directory"): json.dumps(str(state_directory)),
    }
    insert_missing: set[tuple[str, str]] = {("sync", "data_mount_point")}
    if drive_id is not None:
        replacements[("sync", "drive_id")] = json.dumps(drive_id)
        insert_missing.add(("sync", "drive_id"))
    if sync_list is not None:
        replacements[("sync", "sync_list")] = json.dumps(str(sync_list))
        insert_missing.add(("sync", "sync_list"))
    if local_conflict is not None:
        replacements[("sync", "local_conflict")] = json.dumps(local_conflict)
        insert_missing.add(("sync", "local_conflict"))
    if sync_root_files is not None:
        replacements[("sync", "sync_root_files")] = json.dumps(sync_root_files)
        insert_missing.add(("sync", "sync_root_files"))
    if sync_mode is not None:
        replacements[("sync", "mode")] = json.dumps(sync_mode)
        insert_missing.add(("sync", "mode"))
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
    skipping_multiline_array = False
    table_pattern = re.compile(r"^\s*\[([A-Za-z0-9_.-]+)\]\s*(?:#.*)?$")
    key_pattern = re.compile(r"^(\s*)([A-Za-z0-9_-]+)\s*=(.*)$")

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
        if skipping_multiline_array:
            code_part = line.split("#", 1)[0]
            if "]" in code_part:
                skipping_multiline_array = False
            continue
        table_match = table_pattern.match(line)
        if table_match:
            append_missing(table)
            table = table_match.group(1)
            seen_tables.add(table)
            output.append(line)
            continue
        key_match = key_pattern.match(line)
        key = (table, key_match.group(2)) if key_match else None
        if drive_id is not None and key == ("sync", "drive_ids"):
            if key in seen:
                raise E2EError("duplicate configuration key: sync.drive_ids")
            seen.add(key)
            rhs = key_match.group(3).split("#", 1)[0]
            if "[" in rhs and "]" not in rhs:
                skipping_multiline_array = True
            continue
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


def validate_expected_path(relative: Path) -> None:
    if relative.is_absolute() or ".." in relative.parts or not relative.parts:
        raise E2EError("expected fixture path must be a safe relative path")


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


def large_upload_size(value: int | str = 250_000_001) -> int:
    try:
        if isinstance(value, bool):
            raise ValueError("boolean is not a valid upload size")
        size = int(value)
    except (TypeError, ValueError) as error:
        raise E2EError(
            "large upload size must be an integer"
        ) from error
    if size <= 250_000_000:
        raise E2EError(
            "upload-session E2E file must exceed the 250 MB threshold"
        )
    if size > 1_000_000_000:
        raise E2EError("large upload size exceeds 1 GB")
    return size
