from __future__ import annotations

import json
from pathlib import Path
import shutil
import sqlite3
import time
from urllib import error as url_error, parse, request

from .base import E2EError
from .state import active_refresh_token_path, replace_private_file


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

    def delete_item(self, item_id: str) -> None:
        self._request(
            "DELETE",
            "/items/" + parse.quote(item_id, safe=""),
            expected_statuses={204, 404},
        )

    def delete_root(self) -> None:
        if self.root_id is None:
            return
        self.delete_item(self.root_id)
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
