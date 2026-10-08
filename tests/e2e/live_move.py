from __future__ import annotations

from pathlib import Path
import secrets

from .base import E2EError, fixture_path, sync_list_rule, sync_list_subtree_rule
from .graph import GraphMoveFixture
from .live_context import LiveScenarioContext
from .run import run_sync
from .state import tracked_drive_id, tracked_item_count
from .util import json_event_count


def run_remote_move_scenarios(context: LiveScenarioContext) -> None:
    context.progress.step("Checking remote file and directory moves")
    client = context.client
    sync_data_directory = context.sync_data_directory
    state_directory = context.state_directory
    home = context.home
    config = context.config
    sync_list = context.sync_list
    log_file = context.log_file
    expected_path = context.expected_path
    graph_endpoint = context.graph_endpoint
    access_token = context.access_token
    completed = context.completed
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
        old_file = fixture_path(sync_data_directory, old_file_path)
        old_directory = old_file.parent / "directory-old"
        old_nested = fixture_path(sync_data_directory, old_nested_path)
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
        new_file = fixture_path(sync_data_directory, new_file_path)
        new_directory = new_file.parent / "directory-new"
        new_nested = fixture_path(sync_data_directory, new_nested_path)
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
