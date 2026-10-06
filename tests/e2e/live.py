from __future__ import annotations

import os
from pathlib import Path
import secrets
import shutil
import tempfile

from .base import (
    E2EError,
    fixture_path,
    load_live_settings,
    rewrite_config,
    sha256,
    sync_list_rule,
    sync_list_subtree_rule,
    sync_root_for_fixture,
    write_pattern_file,
)
from .graph import GraphMoveFixture, inject_disappeared_item, refresh_graph_access_token
from .run import reset_copied_state, reset_delta_cursor, run_single_download, run_sync
from .state import inject_completed_upload, tracked_drive_id, tracked_item_count
from .util import (
    has_json_event,
    json_event_count,
    log_text_since,
    materialized_files,
    safe_backup_files,
    save_artifacts,
    wait_until,
)


def run_live(client: Path, work_root: Path) -> None:
    settings = load_live_settings(client)
    expected_path = settings.expected_path
    expected_sha256 = settings.expected_sha256
    source_text = settings.source_text
    source_config = settings.source_config
    source_state = settings.source_state

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
