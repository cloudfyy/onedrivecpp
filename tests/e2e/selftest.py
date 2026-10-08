from __future__ import annotations

import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

from .base import (
    E2EError,
    fixture_path,
    large_upload_size,
    sha256,
    sync_list_rule,
    sync_list_subtree_rule,
    sync_root_for_fixture,
    write_pattern_file,
)
from .graph import inject_disappeared_item
from .selftest_config import self_test_config
from .selftest_graph import self_test_graph
from .selftest_progress import self_test_progress
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
)


def self_test() -> None:
    self_test_config()
    self_test_progress()
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

        self_test_graph(state, token)
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
