from __future__ import annotations

from pathlib import Path
import shutil
import tempfile

from .base import (
    E2EError,
    fixture_path,
    load_live_settings,
    rewrite_config,
    sha256,
    sync_list_rule,
)
from .graph import inject_disappeared_item, refresh_graph_access_token
from .live_context import LiveScenarioContext
from .live_coverage import run_additional_coverage_scenarios
from .live_move import run_remote_move_scenarios
from .live_upload import run_upload_scenarios
from .progress import E2EProgress
from .run import reset_copied_state, reset_delta_cursor, run_single_download, run_sync
from .state import tracked_item_count
from .util import (
    has_json_event,
    log_text_since,
    materialized_files,
    safe_backup_files,
    save_artifacts,
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
        sync_data_directory = workspace / "sync"
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
                sync_data_directory,
                state_directory,
                sync_list,
                "backup",
                False,
            ),
            encoding="utf-8",
        )
        try:
            progress = E2EProgress(10)
            progress.step("Preparing isolated state")
            reset = reset_copied_state(client, config, home)
            completed.append(reset)
            if reset.returncode != 0:
                raise E2EError(
                    f"isolated state reset failed with {reset.returncode}"
                )
            progress.step("Checking single-file and selective downloads")
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
            downloaded = fixture_path(sync_data_directory, expected_path)
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
            excluded_fixture = fixture_path(sync_data_directory, expected_path)
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
            synchronized = fixture_path(sync_data_directory, expected_path)
            synchronized_stat = synchronized.stat()
            if sha256(synchronized) != expected_sha256:
                raise E2EError("included fixture SHA-256 mismatch")
            if tracked_item_count(state_directory, expected_path) != 1:
                raise E2EError(
                    "included fixture snapshot was not persisted exactly once"
                )
            if materialized_files(sync_data_directory) != [synchronized]:
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
            repeated = fixture_path(sync_data_directory, expected_path)
            repeated_stat = repeated.stat()
            if sha256(repeated) != expected_sha256:
                raise E2EError("fixture changed after repeat synchronization")
            if (
                repeated_stat.st_ino != synchronized_stat.st_ino
                or repeated_stat.st_mtime_ns != synchronized_stat.st_mtime_ns
            ):
                raise E2EError("repeat synchronization rewrote the unchanged fixture")

            progress.step("Checking root-file selection changes")
            config.write_text(
                rewrite_config(
                    source_text,
                    sync_data_directory,
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
                sync_data_directory,
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

            progress.step("Checking safeBackup conflict handling")
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
            restored = fixture_path(sync_data_directory, expected_path)
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
            stable = fixture_path(sync_data_directory, expected_path)
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

            progress.step("Checking fixture upload round trip")
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
            scenario_context = LiveScenarioContext(
                client=client,
                workspace=workspace,
                sync_data_directory=sync_data_directory,
                state_directory=state_directory,
                home=home,
                config=config,
                sync_list=sync_list,
                log_file=log_file,
                source_text=source_text,
                expected_path=expected_path,
                stable=stable,
                graph_endpoint=graph_endpoint,
                access_token=access_token,
                completed=completed,
                progress=progress,
            )
            run_upload_scenarios(scenario_context)
            run_remote_move_scenarios(scenario_context)
            run_additional_coverage_scenarios(scenario_context)

            progress.step("Checking full remote-deletion reconciliation")
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
            progress.step("Live Graph E2E completed")
        except Exception:
            save_artifacts(completed, log_file)
            raise
