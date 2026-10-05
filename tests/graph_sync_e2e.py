#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import sqlite3
import subprocess
import tempfile
import time
import tomllib
from urllib import error as url_error, parse, request


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


def tracked_drive_id(state_directory: Path, remote_path: Path) -> str:
    drive_ids: set[str] = set()
    for database in state_directory.rglob("items.sqlite3"):
        try:
            with sqlite3.connect(database) as connection:
                rows = connection.execute(
                    "SELECT DISTINCT drive_id FROM item WHERE remote_path = ?",
                    (remote_path.as_posix(),),
                ).fetchall()
        except sqlite3.Error as error:
            raise E2EError(f"cannot query item database: {database}") from error
        drive_ids.update(str(row[0]) for row in rows)
    if len(drive_ids) != 1:
        raise E2EError(
            "move fixture requires exactly one tracked Graph Drive"
        )
    return next(iter(drive_ids))


def inject_completed_upload(
    state_directory: Path,
    local_path: Path,
    remote_path: Path,
) -> Path:
    databases = list(state_directory.rglob("items.sqlite3"))
    matches: list[tuple[Path, tuple[object, ...]]] = []
    for database in databases:
        try:
            with sqlite3.connect(database) as connection:
                row = connection.execute(
                    "SELECT drive_id, remote_id, local_size, "
                    "local_modified_ticks FROM item WHERE remote_path = ?",
                    (remote_path.as_posix(),),
                ).fetchone()
        except sqlite3.Error as error:
            raise E2EError(f"cannot query item database: {database}") from error
        if row is not None:
            matches.append((database, row))
    if len(matches) != 1:
        raise E2EError(
            "upload recovery injection requires exactly one tracked item"
        )
    database, row = matches[0]
    drive_id, remote_id, local_size, local_modified_ticks = row
    snapshot = local_path.with_name(
        f".{local_path.name}.onedrive-upload-e2e-{secrets.token_hex(8)}"
    )
    shutil.copyfile(local_path, snapshot)
    try:
        with sqlite3.connect(database) as connection:
            connection.execute("BEGIN IMMEDIATE")
            connection.execute(
                "DELETE FROM item WHERE drive_id = ? AND remote_id = ?",
                (drive_id, remote_id),
            )
            connection.execute(
                "INSERT INTO pending_upload ("
                "drive_id, remote_path, local_path, snapshot_path, "
                "content_fingerprint, local_size, local_modified_ticks, "
                "remote_id, expected_etag, upload_url, upload_expiration, "
                "completed_bytes, failure_code, failure_message, "
                "failure_attempt_count, directory"
                ") VALUES (?, ?, ?, ?, ?, ?, ?, '', '', '', '', 0, '', '', 0, 0)",
                (
                    drive_id,
                    remote_path.as_posix(),
                    str(local_path),
                    str(snapshot),
                    sha256(snapshot),
                    local_size,
                    local_modified_ticks,
                ),
            )
            connection.commit()
    except sqlite3.Error as error:
        try:
            snapshot.unlink()
        except FileNotFoundError:
            pass
        raise E2EError("cannot inject completed upload recovery") from error
    return snapshot


def active_refresh_token_path(state_directory: Path) -> Path:
    marker = state_directory / "active_account"
    try:
        account = marker.read_text(encoding="utf-8").strip()
    except OSError as error:
        raise E2EError("cannot read isolated active-account marker") from error
    if not account or Path(account).name != account or account in {".", ".."}:
        raise E2EError("isolated active-account marker is invalid")
    token = state_directory / "accounts" / account / "refresh_token"
    if token.is_symlink() or not token.is_file():
        raise E2EError("isolated refresh token is not a regular file")
    return token


def replace_private_file(path: Path, contents: str) -> None:
    temporary = path.with_name(
        f".{path.name}.graph-e2e-{secrets.token_hex(8)}"
    )
    descriptor = -1
    try:
        descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL,
            0o600,
        )
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            descriptor = -1
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if descriptor != -1:
            os.close(descriptor)
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def json_request(
    url: str,
    method: str,
    *,
    access_token: str | None = None,
    body: bytes | None = None,
    content_type: str | None = None,
    expected_statuses: set[int],
) -> object | None:
    headers = {"Accept": "application/json"}
    if access_token is not None:
        headers["Authorization"] = f"Bearer {access_token}"
    if content_type is not None:
        headers["Content-Type"] = content_type
    for attempt in range(5):
        graph_request = request.Request(
            url,
            data=body,
            headers=headers,
            method=method,
        )
        try:
            with request.urlopen(graph_request, timeout=120) as response:
                status = response.status
                response_body = response.read()
                retry_after = response.headers.get("Retry-After")
        except url_error.HTTPError as http_error:
            status = http_error.code
            response_body = http_error.read()
            retry_after = http_error.headers.get("Retry-After")
        except url_error.URLError as request_error:
            raise E2EError(
                f"{method} request failed before receiving an HTTP response"
            ) from request_error
        if status in expected_statuses:
            if not response_body:
                return None
            try:
                return json.loads(response_body)
            except json.JSONDecodeError as decode_error:
                raise E2EError(
                    f"{method} request returned invalid JSON"
                ) from decode_error
        if status not in {429, 500, 502, 503, 504} or attempt == 4:
            raise E2EError(
                f"{method} request returned unexpected HTTP status {status}"
            )
        try:
            delay = max(1, min(30, int(retry_after or "1")))
        except ValueError:
            delay = 1
        time.sleep(delay)
    raise E2EError(f"{method} request exhausted its retry budget")


def refresh_graph_access_token(
    source_config: dict[str, object],
    state_directory: Path,
) -> str:
    auth = source_config.get("auth")
    if not isinstance(auth, dict):
        raise E2EError("E2E configuration requires an auth table")
    application_id = auth.get("application_id")
    if not isinstance(application_id, str) or not application_id:
        raise E2EError("E2E configuration requires auth.application_id")
    tenant = auth.get("tenant_id", "common")
    endpoint = auth.get(
        "endpoint",
        "https://login.microsoftonline.com",
    )
    scopes = auth.get(
        "scopes",
        ["User.Read", "Files.ReadWrite", "offline_access"],
    )
    if (
        not isinstance(tenant, str)
        or not tenant
        or not isinstance(endpoint, str)
        or not endpoint
        or not isinstance(scopes, list)
        or not scopes
        or not all(isinstance(scope, str) and scope for scope in scopes)
    ):
        raise E2EError("E2E authentication configuration is invalid")

    token_path = active_refresh_token_path(state_directory)
    refresh_token = token_path.read_text(encoding="utf-8").strip()
    if not refresh_token:
        raise E2EError("isolated refresh token is empty")
    token_url = (
        endpoint.rstrip("/")
        + "/"
        + parse.quote(tenant, safe="")
        + "/oauth2/v2.0/token"
    )
    response = json_request(
        token_url,
        "POST",
        body=parse.urlencode(
            {
                "client_id": application_id,
                "grant_type": "refresh_token",
                "refresh_token": refresh_token,
                "scope": " ".join(scopes),
            }
        ).encode("ascii"),
        content_type="application/x-www-form-urlencoded",
        expected_statuses={200},
    )
    if not isinstance(response, dict):
        raise E2EError("token refresh response is not a JSON object")
    access_token = response.get("access_token")
    rotated_refresh_token = response.get("refresh_token", refresh_token)
    if (
        not isinstance(access_token, str)
        or not access_token
        or not isinstance(rotated_refresh_token, str)
        or not rotated_refresh_token
    ):
        raise E2EError("token refresh response is missing required tokens")
    if rotated_refresh_token != refresh_token:
        replace_private_file(token_path, rotated_refresh_token)
    return access_token


class GraphMoveFixture:
    def __init__(
        self,
        endpoint: str,
        drive_id: str,
        access_token: str,
    ) -> None:
        self._base_url = (
            endpoint.rstrip("/")
            + "/drives/"
            + parse.quote(drive_id, safe="")
        )
        self._access_token = access_token
        self.root_id: str | None = None

    def _request(
        self,
        method: str,
        path: str,
        *,
        payload: object | None = None,
        body: bytes | None = None,
        content_type: str | None = None,
        expected_statuses: set[int],
    ) -> object | None:
        if payload is not None:
            body = json.dumps(payload).encode("utf-8")
            content_type = "application/json"
        return json_request(
            self._base_url + path,
            method,
            access_token=self._access_token,
            body=body,
            content_type=content_type,
            expected_statuses=expected_statuses,
        )

    @staticmethod
    def _item_id(response: object | None, operation: str) -> str:
        if not isinstance(response, dict):
            raise E2EError(f"Graph {operation} response is not a JSON object")
        item_id = response.get("id")
        if not isinstance(item_id, str) or not item_id:
            raise E2EError(f"Graph {operation} response has no item ID")
        return item_id

    def create_folder(self, name: str, parent_id: str | None = None) -> str:
        parent_path = (
            "/root/children"
            if parent_id is None
            else "/items/"
            + parse.quote(parent_id, safe="")
            + "/children"
        )
        response = self._request(
            "POST",
            parent_path,
            payload={
                "name": name,
                "folder": {},
                "@microsoft.graph.conflictBehavior": "fail",
            },
            expected_statuses={201},
        )
        item_id = self._item_id(response, "folder creation")
        if parent_id is None:
            self.root_id = item_id
        return item_id

    def upload_file(
        self,
        parent_id: str,
        name: str,
        contents: bytes,
    ) -> str:
        response = self._request(
            "PUT",
            "/items/"
            + parse.quote(parent_id, safe="")
            + ":/"
            + parse.quote(name, safe="")
            + ":/content",
            body=contents,
            content_type="application/octet-stream",
            expected_statuses={200, 201},
        )
        return self._item_id(response, "file upload")

    def move_item(
        self,
        item_id: str,
        parent_id: str,
        name: str,
    ) -> None:
        self._request(
            "PATCH",
            "/items/" + parse.quote(item_id, safe=""),
            payload={
                "name": name,
                "parentReference": {"id": parent_id},
            },
            expected_statuses={200},
        )

    def item_by_path(self, remote_path: Path) -> dict[str, object]:
        response = self._request(
            "GET",
            "/root:/" + parse.quote(remote_path.as_posix(), safe="/"),
            expected_statuses={200},
        )
        if not isinstance(response, dict):
            raise E2EError("Graph path lookup response is not a JSON object")
        return response

    def item_exists(self, remote_path: Path) -> bool:
        try:
            self.item_by_path(remote_path)
            return True
        except E2EError as error:
            if "unexpected HTTP status 404" in str(error):
                return False
            raise

    def replace_file(self, item_id: str, contents: bytes) -> None:
        self._request(
            "PUT",
            "/items/" + parse.quote(item_id, safe="") + "/content",
            body=contents,
            content_type="application/octet-stream",
            expected_statuses={200},
        )

    def delete_root(self) -> None:
        if self.root_id is None:
            return
        self._request(
            "DELETE",
            "/items/" + parse.quote(self.root_id, safe=""),
            expected_statuses={204, 404},
        )
        self.root_id = None


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
    with tempfile.TemporaryDirectory(prefix="graph-sync-", dir=work_root) as raw:
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

            original_contents = stable.read_bytes()
            upload_contents = (
                b"onedrive-cpp live Graph local upload E2E\n"
            )
            stable.write_bytes(upload_contents)
            uploaded = run_sync(client, config, home, log_file)
            completed.append(uploaded)
            if uploaded.returncode != 0:
                raise E2EError(
                    f"live Graph upload failed with {uploaded.returncode}"
                )
            if not has_json_event(uploaded, "local_item_uploaded"):
                raise E2EError(
                    "local upload did not emit its JSON event"
                )
            stable.unlink()
            downloaded_upload = run_sync(client, config, home, log_file)
            completed.append(downloaded_upload)
            if downloaded_upload.returncode != 0:
                raise E2EError(
                    "uploaded-content verification sync failed with "
                    f"{downloaded_upload.returncode}"
                )
            if stable.read_bytes() != upload_contents:
                raise E2EError(
                    "Graph did not retain the uploaded local content"
                )
            stable.write_bytes(original_contents)
            restored_upload = run_sync(client, config, home, log_file)
            completed.append(restored_upload)
            if restored_upload.returncode != 0:
                raise E2EError(
                    "remote fixture restoration upload failed with "
                    f"{restored_upload.returncode}"
                )
            stable.unlink()
            verified_restore = run_sync(client, config, home, log_file)
            completed.append(verified_restore)
            if verified_restore.returncode != 0:
                raise E2EError(
                    "remote fixture restoration verification failed with "
                    f"{verified_restore.returncode}"
                )
            if sha256(stable) != expected_sha256:
                raise E2EError(
                    "live Graph upload E2E did not restore the fixture"
                )

            graph_config = source_config.get("graph", {})
            if not isinstance(graph_config, dict):
                raise E2EError("E2E configuration graph table is invalid")
            graph_endpoint = graph_config.get(
                "endpoint",
                "https://graph.microsoft.com/v1.0",
            )
            if not isinstance(graph_endpoint, str) or not graph_endpoint:
                raise E2EError("E2E Graph endpoint is invalid")
            access_token = refresh_graph_access_token(
                source_config,
                state_directory,
            )
            upload_fixture_name = (
                "__onedrive_cpp_upload_e2e_" + secrets.token_hex(8)
            )
            upload_fixture = GraphMoveFixture(
                graph_endpoint,
                tracked_drive_id(state_directory, expected_path),
                access_token,
            )
            upload_root_path = Path(upload_fixture_name)
            upload_local_root = (
                sync_root_for_fixture(stable, expected_path)
                / upload_fixture_name
            )
            try:
                sync_list.write_text(
                    sync_list_rule(expected_path)
                    + sync_list_subtree_rule(upload_root_path),
                    encoding="utf-8",
                )
                upload_nested = upload_local_root / "nested"
                upload_source = upload_local_root / "source"
                upload_nested.mkdir(parents=True)
                upload_source.mkdir()
                recovery_file = upload_nested / "recovery.txt"
                recovery_contents = (
                    b"onedrive-cpp completed upload recovery E2E\n"
                )
                recovery_file.write_bytes(recovery_contents)
                move_file = upload_source / "move.txt"
                move_contents = b"onedrive-cpp local move upload E2E\n"
                move_file.write_bytes(move_contents)
                large_file = upload_local_root / "session.bin"
                try:
                    large_size = int(
                        os.environ.get(
                            "ONEDRIVE_E2E_LARGE_UPLOAD_BYTES",
                            "250000001",
                        )
                    )
                except ValueError as error:
                    raise E2EError(
                        "ONEDRIVE_E2E_LARGE_UPLOAD_BYTES must be an integer"
                    ) from error
                if large_size > 1_000_000_000:
                    raise E2EError(
                        "ONEDRIVE_E2E_LARGE_UPLOAD_BYTES exceeds 1 GB"
                    )
                large_sha256 = write_pattern_file(
                    large_file,
                    large_size,
                )

                created_upload_fixture = run_sync(
                    client,
                    config,
                    home,
                    log_file,
                )
                completed.append(created_upload_fixture)
                if created_upload_fixture.returncode != 0:
                    raise E2EError(
                        "local Graph upload fixture creation failed with "
                        f"{created_upload_fixture.returncode}"
                    )
                if json_event_count(
                    created_upload_fixture,
                    "local_directory_created",
                ) < 3:
                    raise E2EError(
                        "local Graph upload did not create its directories"
                    )
                if json_event_count(
                    created_upload_fixture,
                    "local_item_uploaded",
                ) < 3:
                    raise E2EError(
                        "local Graph upload did not upload all fixture files"
                    )

                upload_root_item = upload_fixture.item_by_path(
                    upload_root_path
                )
                upload_root_id = upload_root_item.get("id")
                if not isinstance(upload_root_id, str) or not upload_root_id:
                    raise E2EError(
                        "uploaded fixture root has no Graph item ID"
                    )
                upload_fixture.root_id = upload_root_id
                large_remote_path = upload_root_path / "session.bin"
                large_item = upload_fixture.item_by_path(large_remote_path)
                if large_item.get("size") != large_size:
                    raise E2EError(
                        "upload-session E2E file has the wrong remote size"
                    )
                if sha256(large_file) != large_sha256:
                    raise E2EError(
                        "upload-session E2E changed the local source"
                    )
                for remote_path in (
                    upload_root_path,
                    upload_root_path / "nested",
                    upload_root_path / "source",
                    upload_root_path / "nested" / "recovery.txt",
                    upload_root_path / "source" / "move.txt",
                    large_remote_path,
                ):
                    if tracked_item_count(
                        state_directory,
                        remote_path,
                    ) != 1:
                        raise E2EError(
                            "local Graph upload did not persist its snapshot"
                        )

                recovery_remote_path = (
                    upload_root_path / "nested" / "recovery.txt"
                )
                recovery_snapshot = inject_completed_upload(
                    state_directory,
                    recovery_file,
                    recovery_remote_path,
                )
                recovered_upload = run_sync(
                    client,
                    config,
                    home,
                    log_file,
                )
                completed.append(recovered_upload)
                if recovered_upload.returncode != 0:
                    raise E2EError(
                        "completed Graph upload recovery failed with "
                        f"{recovered_upload.returncode}"
                    )
                if not has_json_event(
                    recovered_upload,
                    "pending_upload_recovered",
                ):
                    raise E2EError(
                        "completed Graph upload recovery emitted no event"
                    )
                if recovery_snapshot.exists():
                    raise E2EError(
                        "completed Graph upload recovery retained its snapshot"
                    )
                if (
                    recovery_file.read_bytes() != recovery_contents
                    or tracked_item_count(
                        state_directory,
                        recovery_remote_path,
                    ) != 1
                ):
                    raise E2EError(
                        "completed Graph upload recovery changed its file"
                    )

                local_conflict_contents = (
                    b"onedrive-cpp local upload conflict bytes\n"
                )
                remote_conflict_contents = (
                    b"onedrive-cpp remote upload conflict bytes\n"
                )
                recovery_file.write_bytes(local_conflict_contents)
                recovery_item = upload_fixture.item_by_path(
                    recovery_remote_path
                )
                recovery_item_id = recovery_item.get("id")
                if (
                    not isinstance(recovery_item_id, str)
                    or not recovery_item_id
                ):
                    raise E2EError(
                        "upload-conflict fixture has no Graph item ID"
                    )
                upload_fixture.replace_file(
                    recovery_item_id,
                    remote_conflict_contents,
                )
                reconciled_upload_conflict = run_sync(
                    client,
                    config,
                    home,
                    log_file,
                )
                completed.append(reconciled_upload_conflict)
                if reconciled_upload_conflict.returncode != 0:
                    raise E2EError(
                        "upload conflict reconciliation failed with "
                        f"{reconciled_upload_conflict.returncode}"
                    )
                conflict_backups = safe_backup_files(recovery_file)
                if (
                    recovery_file.read_bytes() != remote_conflict_contents
                    or len(conflict_backups) != 1
                    or conflict_backups[0].read_bytes()
                    != local_conflict_contents
                    or not has_json_event(
                        reconciled_upload_conflict,
                        "local_conflict_backed_up",
                    )
                ):
                    raise E2EError(
                        "upload conflict did not preserve both versions"
                    )

                move_remote_path = upload_root_path / "source" / "move.txt"
                move_item = upload_fixture.item_by_path(move_remote_path)
                move_item_id = move_item.get("id")
                if not isinstance(move_item_id, str) or not move_item_id:
                    raise E2EError(
                        "local-move upload fixture has no Graph item ID"
                    )
                move_target = (
                    upload_local_root / "new-parent" / "nested" / "moved.txt"
                )
                move_target.parent.mkdir(parents=True)
                move_file.rename(move_target)
                uploaded_move = run_sync(
                    client,
                    config,
                    home,
                    log_file,
                )
                completed.append(uploaded_move)
                moved_remote_path = (
                    upload_root_path /
                    "new-parent" /
                    "nested" /
                    "moved.txt"
                )
                moved_item = upload_fixture.item_by_path(moved_remote_path)
                if (
                    uploaded_move.returncode != 0
                    or moved_item.get("id") != move_item_id
                    or upload_fixture.item_exists(move_remote_path)
                    or move_target.read_bytes() != move_contents
                    or not has_json_event(
                        uploaded_move,
                        "local_move_uploaded",
                    )
                ):
                    raise E2EError(
                        "local move upload did not preserve the Graph item"
                    )

                settled_move = run_sync(
                    client,
                    config,
                    home,
                    log_file,
                )
                completed.append(settled_move)
                settled_item = upload_fixture.item_by_path(moved_remote_path)
                if (
                    settled_move.returncode != 0
                    or settled_item.get("id") != move_item_id
                    or upload_fixture.item_exists(move_remote_path)
                    or move_target.read_bytes() != move_contents
                ):
                    raise E2EError(
                        "local move upload did not settle its Graph delta"
                    )

                shutil.rmtree(upload_local_root)
                deleted_upload_fixture = run_sync(
                    client,
                    config,
                    home,
                    log_file,
                )
                completed.append(deleted_upload_fixture)
                if (
                    deleted_upload_fixture.returncode != 0
                    or upload_fixture.item_exists(upload_root_path)
                    or not has_json_event(
                        deleted_upload_fixture,
                        "local_item_deleted",
                    )
                ):
                    raise E2EError(
                        "local deletion did not remove the Graph upload fixture"
                    )
                upload_fixture.root_id = None
            finally:
                upload_fixture.delete_root()

            move_fixture_name = (
                "__onedrive_cpp_move_e2e_" + secrets.token_hex(8)
            )
            move_fixture = GraphMoveFixture(
                graph_endpoint,
                tracked_drive_id(state_directory, expected_path),
                access_token,
            )
            try:
                fixture_root_id = move_fixture.create_folder(
                    move_fixture_name
                )
                source_folder_id = move_fixture.create_folder(
                    "source",
                    fixture_root_id,
                )
                target_folder_id = move_fixture.create_folder(
                    "target",
                    fixture_root_id,
                )
                directory_id = move_fixture.create_folder(
                    "directory-old",
                    source_folder_id,
                )
                move_file_contents = (
                    b"onedrive-cpp live Graph remote file move E2E\n"
                )
                nested_file_contents = (
                    b"onedrive-cpp live Graph remote directory move E2E\n"
                )
                move_file_id = move_fixture.upload_file(
                    source_folder_id,
                    "file-old.bin",
                    move_file_contents,
                )
                move_fixture.upload_file(
                    directory_id,
                    "nested.bin",
                    nested_file_contents,
                )

                sync_list.write_text(
                    sync_list_rule(expected_path)
                    + sync_list_subtree_rule(Path(move_fixture_name)),
                    encoding="utf-8",
                )
                downloaded_move_fixture = run_sync(
                    client,
                    config,
                    home,
                    log_file,
                )
                completed.append(downloaded_move_fixture)
                if downloaded_move_fixture.returncode != 0:
                    raise E2EError(
                        "remote-move fixture download failed with "
                        f"{downloaded_move_fixture.returncode}"
                    )

                old_file_path = (
                    Path(move_fixture_name) / "source" / "file-old.bin"
                )
                old_directory_path = (
                    Path(move_fixture_name) / "source" / "directory-old"
                )
                old_nested_path = old_directory_path / "nested.bin"
                old_file = fixture_path(sync_directory, old_file_path)
                old_directory = old_file.parent / "directory-old"
                old_nested = fixture_path(sync_directory, old_nested_path)
                if old_file.read_bytes() != move_file_contents:
                    raise E2EError("remote file-move fixture content changed")
                if old_nested.read_bytes() != nested_file_contents:
                    raise E2EError(
                        "remote directory-move fixture content changed"
                    )
                old_file_stat = old_file.stat()
                old_directory_stat = old_directory.stat()
                old_nested_stat = old_nested.stat()
                move_local_root = old_file.parents[1]

                move_fixture.move_item(
                    move_file_id,
                    target_folder_id,
                    "file-new.bin",
                )
                move_fixture.move_item(
                    directory_id,
                    target_folder_id,
                    "directory-new",
                )
                applied_moves = run_sync(
                    client,
                    config,
                    home,
                    log_file,
                )
                completed.append(applied_moves)
                if applied_moves.returncode != 0:
                    raise E2EError(
                        "remote move synchronization failed with "
                        f"{applied_moves.returncode}"
                    )

                new_file_path = (
                    Path(move_fixture_name) / "target" / "file-new.bin"
                )
                new_directory_path = (
                    Path(move_fixture_name) / "target" / "directory-new"
                )
                new_nested_path = new_directory_path / "nested.bin"
                new_file = fixture_path(sync_directory, new_file_path)
                new_directory = new_file.parent / "directory-new"
                new_nested = fixture_path(sync_directory, new_nested_path)
                if old_file.exists() or old_directory.exists():
                    raise E2EError(
                        "remote move left its old local path materialized"
                    )
                if (
                    new_file.stat().st_ino != old_file_stat.st_ino
                    or new_directory.stat().st_ino
                    != old_directory_stat.st_ino
                    or new_nested.stat().st_ino != old_nested_stat.st_ino
                ):
                    raise E2EError(
                        "remote move did not reuse the local filesystem objects"
                    )
                if (
                    new_file.read_bytes() != move_file_contents
                    or new_nested.read_bytes() != nested_file_contents
                ):
                    raise E2EError("remote move changed fixture content")
                for old_path in (
                    old_file_path,
                    old_directory_path,
                    old_nested_path,
                ):
                    if tracked_item_count(state_directory, old_path) != 0:
                        raise E2EError(
                            "remote move retained an old-path snapshot"
                        )
                for new_path in (
                    new_file_path,
                    new_directory_path,
                    new_nested_path,
                ):
                    if tracked_item_count(state_directory, new_path) != 1:
                        raise E2EError(
                            "remote move did not persist its new-path snapshot"
                        )
                if json_event_count(applied_moves, "local_item_moved") < 2:
                    raise E2EError(
                        "remote moves did not emit structured move events"
                    )

                move_fixture.delete_root()
                cleaned_moves = run_sync(
                    client,
                    config,
                    home,
                    log_file,
                )
                completed.append(cleaned_moves)
                if cleaned_moves.returncode != 0:
                    raise E2EError(
                        "remote-move fixture cleanup sync failed with "
                        f"{cleaned_moves.returncode}"
                    )
                if move_local_root.exists():
                    raise E2EError(
                        "remote-move fixture cleanup retained local data"
                    )
                for moved_path in (
                    new_file_path,
                    new_directory_path,
                    new_nested_path,
                ):
                    if tracked_item_count(state_directory, moved_path) != 0:
                        raise E2EError(
                            "remote-move fixture cleanup retained snapshots"
                        )
            finally:
                move_fixture.delete_root()

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
        if (
            sync_root_for_fixture(expected, Path("fixture/small.txt"))
            != root / "account"
        ):
            raise E2EError("fixture sync-root lookup self-test failed")
        try:
            sync_root_for_fixture(expected, Path("other/small.txt"))
            raise E2EError("mismatched fixture suffix was accepted")
        except E2EError as error:
            if "does not end with" not in str(error):
                raise
        if sync_list_rule(Path("fixture/small.txt")) != "/fixture/small.txt\n":
            raise E2EError("selective-sync rule generation self-test failed")
        if (
            sync_list_subtree_rule(Path("move-fixture"))
            != "/move-fixture/**\n"
        ):
            raise E2EError(
                "selective-sync subtree rule generation self-test failed"
            )
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
        if tracked_drive_id(state, Path("fixture/small.txt")) != "drive":
            raise E2EError("tracked Drive lookup self-test failed")
        if materialized_files(root) != sorted([expected, database]):
            raise E2EError("materialized file lookup self-test failed")
        try:
            write_pattern_file(root / "too-small.bin", 250_000_000)
            raise E2EError("small upload-session fixture was accepted")
        except E2EError as error:
            if "must exceed" not in str(error):
                raise
        recovery_source = expected.with_name("recovery.txt")
        recovery_source.write_text("recovery", encoding="utf-8")
        with sqlite3.connect(database) as connection:
            connection.execute(
                "CREATE TABLE pending_upload ("
                "drive_id TEXT NOT NULL, remote_path TEXT NOT NULL, "
                "local_path TEXT NOT NULL, snapshot_path TEXT NOT NULL, "
                "content_fingerprint TEXT NOT NULL, local_size INTEGER NOT NULL, "
                "local_modified_ticks INTEGER NOT NULL, remote_id TEXT NOT NULL, "
                "expected_etag TEXT NOT NULL, upload_url TEXT NOT NULL, "
                "upload_expiration TEXT NOT NULL, completed_bytes INTEGER NOT NULL, "
                "failure_code TEXT NOT NULL, failure_message TEXT NOT NULL, "
                "failure_attempt_count INTEGER NOT NULL, directory INTEGER NOT NULL)"
            )
            connection.execute(
                "INSERT INTO item VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    "drive",
                    "recovery",
                    "parent",
                    "recovery.txt",
                    "etag",
                    "fixture/recovery.txt",
                    str(recovery_source),
                    "2026-10-04T00:00:00Z",
                    8,
                    8,
                    456,
                    0,
                ),
            )
        injected_snapshot = inject_completed_upload(
            state,
            recovery_source,
            Path("fixture/recovery.txt"),
        )
        with sqlite3.connect(database) as connection:
            pending = connection.execute(
                "SELECT content_fingerprint, local_size, "
                "local_modified_ticks FROM pending_upload "
                "WHERE remote_path = 'fixture/recovery.txt'"
            ).fetchone()
            remaining = connection.execute(
                "SELECT count(*) FROM item "
                "WHERE remote_path = 'fixture/recovery.txt'"
            ).fetchone()
            connection.execute(
                "DELETE FROM pending_upload "
                "WHERE remote_path = 'fixture/recovery.txt'"
            )
        if (
            pending != (sha256(recovery_source), 8, 456)
            or remaining != (0,)
            or not injected_snapshot.is_file()
        ):
            raise E2EError("completed upload injection self-test failed")
        injected_snapshot.unlink()
        recovery_source.unlink()
        account = state / "accounts" / "account--12345678"
        account.mkdir(parents=True)
        token = account / "refresh_token"
        token.write_text("old-token", encoding="utf-8")
        token.chmod(0o600)
        (state / "active_account").write_text(
            account.name + "\n",
            encoding="utf-8",
        )
        if active_refresh_token_path(state) != token:
            raise E2EError("active refresh-token lookup self-test failed")
        replace_private_file(token, "new-token")
        if (
            token.read_text(encoding="utf-8") != "new-token"
            or token.stat().st_mode & 0o777 != 0o600
        ):
            raise E2EError("private token replacement self-test failed")

        class FakeResponse:
            def __init__(
                self,
                status: int,
                body: bytes,
                headers: dict[str, str] | None = None,
            ) -> None:
                self.status = status
                self._body = body
                self.headers = headers or {}

            def __enter__(self) -> FakeResponse:
                return self

            def __exit__(self, *arguments: object) -> None:
                return None

            def read(self) -> bytes:
                return self._body

        urlopen_results: list[object] = [
            FakeResponse(200, b'{"ok":true}'),
            url_error.HTTPError(
                "https://graph.example.test/retry",
                429,
                "throttled",
                {"Retry-After": "0"},
                io.BytesIO(b"{}"),
            ),
            FakeResponse(204, b""),
            url_error.HTTPError(
                "https://graph.example.test/fail",
                400,
                "bad request",
                {},
                io.BytesIO(b"{}"),
            ),
        ]

        def fake_urlopen(
            graph_request: request.Request,
            timeout: int,
        ) -> object:
            del graph_request, timeout
            result = urlopen_results.pop(0)
            if isinstance(result, BaseException):
                raise result
            return result

        original_urlopen = request.urlopen
        original_sleep = time.sleep
        request.urlopen = fake_urlopen
        time.sleep = lambda delay: None
        try:
            response = json_request(
                "https://graph.example.test/success",
                "POST",
                access_token="access-token",
                body=b"{}",
                content_type="application/json",
                expected_statuses={200},
            )
            if response != {"ok": True}:
                raise E2EError("JSON request success self-test failed")
            if (
                json_request(
                    "https://graph.example.test/retry",
                    "DELETE",
                    expected_statuses={204},
                )
                is not None
            ):
                raise E2EError("JSON request retry self-test failed")
            try:
                json_request(
                    "https://graph.example.test/fail",
                    "GET",
                    expected_statuses={200},
                )
                raise E2EError("unexpected Graph HTTP status was accepted")
            except E2EError as graph_error:
                if "unexpected HTTP status 400" not in str(graph_error):
                    raise
        finally:
            request.urlopen = original_urlopen
            time.sleep = original_sleep

        requests: list[tuple[str, str, dict[str, object]]] = []

        def fake_json_request(
            url: str,
            method: str,
            **kwargs: object,
        ) -> object | None:
            requests.append((url, method, kwargs))
            if url.endswith("/oauth2/v2.0/token"):
                return {
                    "access_token": "access-token",
                    "refresh_token": "rotated-token",
                }
            if method == "GET" and url.endswith("/root:/root/missing.bin"):
                raise E2EError(
                    "GET request returned unexpected HTTP status 404"
                )
            if method == "DELETE":
                return None
            return {"id": f"item-{len(requests)}"}

        original_json_request = globals()["json_request"]
        globals()["json_request"] = fake_json_request
        try:
            access_token = refresh_graph_access_token(
                {
                    "auth": {
                        "application_id": "application",
                        "tenant_id": "tenant/id",
                        "endpoint": "https://login.example.test/",
                        "scopes": [
                            "User.Read",
                            "Files.ReadWrite",
                            "offline_access",
                        ],
                    }
                },
                state,
            )
            if (
                access_token != "access-token"
                or token.read_text(encoding="utf-8") != "rotated-token"
            ):
                raise E2EError("token refresh self-test failed")

            graph_fixture = GraphMoveFixture(
                "https://graph.example.test/v1.0/",
                "drive/id",
                access_token,
            )
            graph_root_id = graph_fixture.create_folder("root")
            graph_child_id = graph_fixture.create_folder(
                "child",
                graph_root_id,
            )
            graph_file_id = graph_fixture.upload_file(
                graph_child_id,
                "file name.bin",
                b"contents",
            )
            graph_fixture.move_item(
                graph_file_id,
                graph_root_id,
                "renamed.bin",
            )
            looked_up = graph_fixture.item_by_path(
                Path("root/renamed.bin")
            )
            if looked_up.get("id") is None:
                raise E2EError("Graph fixture path lookup self-test failed")
            if not graph_fixture.item_exists(Path("root/renamed.bin")):
                raise E2EError("Graph fixture existence self-test failed")
            if graph_fixture.item_exists(Path("root/missing.bin")):
                raise E2EError("Graph fixture accepted a missing item")
            graph_fixture.replace_file(graph_file_id, b"replacement")
            graph_fixture.delete_root()
            if graph_fixture.root_id is not None:
                raise E2EError("Graph fixture cleanup self-test failed")
        finally:
            globals()["json_request"] = original_json_request

        graph_calls = requests[1:]
        if [call[1] for call in graph_calls] != [
            "POST",
            "POST",
            "PUT",
            "PATCH",
            "GET",
            "GET",
            "GET",
            "PUT",
            "DELETE",
        ]:
            raise E2EError("Graph fixture request sequence self-test failed")
        if (
            "/drives/drive%2Fid/root/children" not in graph_calls[0][0]
            or "file%20name.bin:/content" not in graph_calls[2][0]
            or graph_calls[2][2].get("access_token") != "access-token"
        ):
            raise E2EError("Graph fixture URL self-test failed")
        move_body = graph_calls[3][2].get("body")
        if not isinstance(move_body, bytes):
            raise E2EError("Graph fixture move body self-test failed")
        move_payload = json.loads(move_body)
        if move_payload != {
            "name": "renamed.bin",
            "parentReference": {"id": graph_root_id},
        }:
            raise E2EError("Graph fixture move payload self-test failed")
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
        if json_event_count(event_result, "local_conflict_backed_up") != 1:
            raise E2EError("JSON event count self-test failed")
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
        print(f"Graph synchronization E2E failed: {error}", file=os.sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
