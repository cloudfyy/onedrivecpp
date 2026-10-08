from __future__ import annotations

from pathlib import Path
import secrets
import shutil

from .base import (
    E2EError,
    large_upload_size,
    sha256,
    sync_list_rule,
    sync_list_subtree_rule,
    sync_root_for_fixture,
    write_pattern_file,
)
from .graph import GraphMoveFixture
from .live_context import LiveScenarioContext
from .run import run_sync
from .state import inject_completed_upload, tracked_drive_id, tracked_item_count
from .util import (
    has_json_event,
    json_event_count,
    safe_backup_files,
    wait_until,
)


def run_upload_scenarios(context: LiveScenarioContext) -> None:
    context.progress.step("Checking uploads, recovery, conflicts, and local moves")
    client = context.client
    state_directory = context.state_directory
    home = context.home
    config = context.config
    sync_list = context.sync_list
    log_file = context.log_file
    expected_path = context.expected_path
    stable = context.stable
    graph_endpoint = context.graph_endpoint
    access_token = context.access_token
    completed = context.completed
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
        large_size = large_upload_size()
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
        moved_item: dict[str, object] | None = None

        def moved_item_visible() -> bool:
            nonlocal moved_item
            try:
                candidate = upload_fixture.item_by_path(
                    moved_remote_path
                )
            except E2EError:
                return False
            if (
                candidate.get("id") != move_item_id
                or upload_fixture.item_exists(move_remote_path)
            ):
                return False
            moved_item = candidate
            return True

        if (
            uploaded_move.returncode != 0
            or not wait_until(moved_item_visible, 30, 0.5)
            or moved_item is None
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
