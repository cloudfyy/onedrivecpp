from __future__ import annotations

import io
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time
import tomllib
from urllib import error as url_error, request

from . import graph as graph_module
from .base import (
    E2EError,
    fixture_path,
    large_upload_size,
    rewrite_config,
    sha256,
    sync_list_rule,
    sync_list_subtree_rule,
    sync_root_for_fixture,
    write_pattern_file,
)
from .graph import GraphMoveFixture, inject_disappeared_item, json_request, refresh_graph_access_token
from .proxy import ConnectionDropProxy
from .run import run_sync_after_stop
from .state import (
    active_refresh_token_path,
    inject_completed_upload,
    pending_upload_row,
    replace_private_file,
    tracked_drive_id,
    tracked_item_count,
)
from .util import (
    has_json_event,
    json_event_count,
    json_text_event_count,
    materialized_files,
    safe_backup_files,
    wait_until,
)


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
    boundary_rewrite = tomllib.loads(
        rewrite_config(
            source,
            Path("/new/sync"),
            Path("/new/state"),
            proxy_url="http://127.0.0.1:12345",
            monitor_poll_interval_seconds=60,
            monitor_settle_delay_milliseconds=100,
            upload_maximum_rate_bytes_per_second=5_000_000,
        )
    )
    if (
        boundary_rewrite["proxy"]["url"] != "http://127.0.0.1:12345"
        or boundary_rewrite["proxy"]["no_proxy"] != []
        or boundary_rewrite["monitor"]["poll_interval_seconds"] != 60
        or boundary_rewrite["monitor"]["settle_delay_milliseconds"] != 100
        or boundary_rewrite["upload"][
            "maximum_rate_bytes_per_second"
        ]
        != 5_000_000
    ):
        raise E2EError("boundary configuration rewrite self-test failed")
    with ConnectionDropProxy() as proxy:
        with socket.create_connection(
            ("127.0.0.1", int(proxy.url.rsplit(":", 1)[1])),
            timeout=2,
        ):
            pass
        if not wait_until(lambda: proxy.connection_count == 1, 2):
            raise E2EError("connection-dropping proxy self-test failed")
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
        gate_home = root / "gate-home"
        gate_home.mkdir()
        gated_process_ids: list[int] = []
        gated = run_sync_after_stop(
            Path("/bin/true"),
            root / "unused.toml",
            gate_home,
            root / "unused.log",
            gated_process_ids.append,
        )
        if (
            gated.returncode != 0
            or len(gated_process_ids) != 1
            or gated_process_ids[0] <= 0
        ):
            raise E2EError("gated client execution self-test failed")
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
        original_large_size = os.environ.get(
            "ONEDRIVE_E2E_LARGE_UPLOAD_BYTES"
        )
        try:
            os.environ["ONEDRIVE_E2E_LARGE_UPLOAD_BYTES"] = "250000001"
            if large_upload_size() != 250_000_001:
                raise E2EError("large upload size self-test failed")
            os.environ["ONEDRIVE_E2E_LARGE_UPLOAD_BYTES"] = "invalid"
            try:
                large_upload_size()
                raise E2EError("invalid large upload size was accepted")
            except E2EError as error:
                if "must be an integer" not in str(error):
                    raise
            os.environ["ONEDRIVE_E2E_LARGE_UPLOAD_BYTES"] = "1000000001"
            try:
                large_upload_size()
                raise E2EError("oversized large upload fixture was accepted")
            except E2EError as error:
                if "exceeds 1 GB" not in str(error):
                    raise
        finally:
            if original_large_size is None:
                os.environ.pop("ONEDRIVE_E2E_LARGE_UPLOAD_BYTES", None)
            else:
                os.environ["ONEDRIVE_E2E_LARGE_UPLOAD_BYTES"] = (
                    original_large_size
                )
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
        pending_upload = pending_upload_row(
            state,
            Path("fixture/recovery.txt"),
        )
        if (
            pending_upload is None
            or pending_upload[0] != ""
            or pending_upload[1] != 0
            or pending_upload[2] != 0
            or Path(pending_upload[3]) != injected_snapshot
        ):
            raise E2EError("pending upload lookup self-test failed")
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

        original_json_request = graph_module.json_request
        graph_module.json_request = fake_json_request
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
            graph_module.json_request = original_json_request

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
        if json_text_event_count(
            event_result.stdout,
            "local_conflict_backed_up",
        ) != 1:
            raise E2EError("JSON text event count self-test failed")
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
