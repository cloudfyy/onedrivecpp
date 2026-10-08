from __future__ import annotations

from pathlib import Path
import secrets

from .base import (
    E2EError,
    fixture_path,
    rewrite_config,
    sync_list_rule,
    sync_list_subtree_rule,
    sync_root_for_fixture,
)
from .graph import GraphMoveFixture
from .live_context import LiveScenarioContext
from .run import run_sync
from .state import tracked_drive_id, tracked_item_count
from .util import has_json_event


def run_additional_coverage_scenarios(
    context: LiveScenarioContext,
) -> None:
    context.progress.step(
        "Checking dry-run, sync modes, special paths, updates, and deletion"
    )
    client = context.client
    workspace = context.workspace
    sync_data_directory = context.sync_data_directory
    state_directory = context.state_directory
    home = context.home
    config = context.config
    sync_list = context.sync_list
    log_file = context.log_file
    source_text = context.source_text
    expected_path = context.expected_path
    stable = context.stable
    graph_endpoint = context.graph_endpoint
    access_token = context.access_token
    completed = context.completed
    coverage_fixture_name = (
        "__onedrive_cpp_coverage_e2e_" + secrets.token_hex(8)
    )
    coverage_fixture = GraphMoveFixture(
        graph_endpoint,
        tracked_drive_id(state_directory, expected_path),
        access_token,
    )
    coverage_root_path = Path(coverage_fixture_name)
    coverage_local_root = (
        sync_root_for_fixture(stable, expected_path)
        / coverage_fixture_name
    )
    try:
        coverage_root_id = coverage_fixture.create_folder(
            coverage_fixture_name
        )
        sync_list.write_text(
            sync_list_rule(expected_path)
            + sync_list_subtree_rule(coverage_root_path),
            encoding="utf-8",
        )
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
        coverage_baseline = run_sync(
            client,
            config,
            home,
            log_file,
        )
        completed.append(coverage_baseline)
        if (
            coverage_baseline.returncode != 0
            or not coverage_local_root.is_dir()
        ):
            raise E2EError(
                "additional coverage fixture baseline failed"
            )

        special_directory_name = "sp ace-\u6d4b\u8bd5"
        special_directory_id = coverage_fixture.create_folder(
            special_directory_name,
            coverage_root_id,
        )
        empty_name = "empty file.txt"
        empty_id = coverage_fixture.upload_file(
            special_directory_id,
            empty_name,
            b"",
        )
        unicode_name = "remote-\u00df.txt"
        unicode_contents = (
            b"onedrive-cpp Unicode path live Graph E2E\n"
        )
        unicode_id = coverage_fixture.upload_file(
            special_directory_id,
            unicode_name,
            unicode_contents,
        )
        local_dry_name = "local dry-run.txt"
        local_dry = coverage_local_root / local_dry_name
        local_dry_contents = (
            b"onedrive-cpp bidirectional dry-run E2E\n"
        )
        local_dry.write_bytes(local_dry_contents)
        empty_remote_path = (
            coverage_root_path
            / special_directory_name
            / empty_name
        )
        unicode_remote_path = (
            coverage_root_path
            / special_directory_name
            / unicode_name
        )
        local_dry_remote_path = (
            coverage_root_path / local_dry_name
        )

        dry_config = workspace / "dry-run.toml"
        dry_config.write_text(
            rewrite_config(
                source_text,
                sync_data_directory,
                state_directory,
                sync_list,
                "backup",
                True,
                True,
            ),
            encoding="utf-8",
        )
        dry_run = run_sync(
            client,
            dry_config,
            home,
            log_file,
        )
        completed.append(dry_run)
        if (
            dry_run.returncode != 0
            or (coverage_local_root / special_directory_name).exists()
            or coverage_fixture.item_exists(local_dry_remote_path)
            or tracked_item_count(
                state_directory,
                empty_remote_path,
            )
            != 0
            or tracked_item_count(
                state_directory,
                local_dry_remote_path,
            )
            != 0
        ):
            raise E2EError(
                "bidirectional dry-run changed Graph, local data, "
                "or snapshots"
            )

        applied_coverage = run_sync(
            client,
            config,
            home,
            log_file,
        )
        completed.append(applied_coverage)
        empty_local = fixture_path(
            sync_data_directory,
            empty_remote_path,
        )
        unicode_local = fixture_path(
            sync_data_directory,
            unicode_remote_path,
        )
        if (
            applied_coverage.returncode != 0
            or empty_local.stat().st_size != 0
            or unicode_local.read_bytes() != unicode_contents
            or coverage_fixture.item_by_path(
                local_dry_remote_path
            ).get("size")
            != len(local_dry_contents)
        ):
            raise E2EError(
                "dry-run follow-up did not apply zero-byte, special "
                "path, or local upload changes"
            )

        updated_unicode_contents = (
            b"onedrive-cpp clean remote update live Graph E2E\n"
        )
        coverage_fixture.replace_file(
            unicode_id,
            updated_unicode_contents,
        )
        updated_remote = run_sync(
            client,
            config,
            home,
            log_file,
        )
        completed.append(updated_remote)
        if (
            updated_remote.returncode != 0
            or unicode_local.read_bytes()
            != updated_unicode_contents
            or tracked_item_count(
                state_directory,
                unicode_remote_path,
            )
            != 1
        ):
            raise E2EError(
                "clean remote content update was not downloaded"
            )

        coverage_fixture.delete_item(empty_id)
        deleted_remote_file = run_sync(
            client,
            config,
            home,
            log_file,
        )
        completed.append(deleted_remote_file)
        if (
            deleted_remote_file.returncode != 0
            or empty_local.exists()
            or tracked_item_count(
                state_directory,
                empty_remote_path,
            )
            != 0
            or not has_json_event(
                deleted_remote_file,
                "local_item_removed",
            )
        ):
            raise E2EError(
                "individual remote file deletion was not applied"
            )

        remote_upload_only_path = (
            coverage_root_path / "remote upload-only.txt"
        )
        remote_upload_only_contents = (
            b"onedrive-cpp remote upload-only E2E\n"
        )
        coverage_fixture.upload_file(
            coverage_root_id,
            remote_upload_only_path.name,
            remote_upload_only_contents,
        )
        local_upload_only_name = "local upload-only \u7a7a.txt"
        local_upload_only = (
            coverage_local_root / local_upload_only_name
        )
        local_upload_only_contents = (
            b"onedrive-cpp local upload-only E2E\n"
        )
        local_upload_only.write_bytes(local_upload_only_contents)
        local_upload_only_remote_path = (
            coverage_root_path / local_upload_only_name
        )
        upload_only_config = workspace / "upload-only.toml"
        upload_only_config.write_text(
            rewrite_config(
                source_text,
                sync_data_directory,
                state_directory,
                sync_list,
                "backup",
                True,
                False,
                "upload_only",
            ),
            encoding="utf-8",
        )
        upload_only = run_sync(
            client,
            upload_only_config,
            home,
            log_file,
        )
        completed.append(upload_only)
        if (
            upload_only.returncode != 0
            or (
                coverage_local_root
                / remote_upload_only_path.name
            ).exists()
            or coverage_fixture.item_by_path(
                local_upload_only_remote_path
            ).get("size")
            != len(local_upload_only_contents)
            or not has_json_event(
                upload_only,
                "local_item_uploaded",
            )
        ):
            raise E2EError(
                "upload-only mode did not upload local data while "
                "ignoring remote-only content"
            )

        remote_download_only_path = (
            coverage_root_path / "remote download-only.txt"
        )
        remote_download_only_contents = (
            b"onedrive-cpp remote download-only E2E\n"
        )
        coverage_fixture.upload_file(
            coverage_root_id,
            remote_download_only_path.name,
            remote_download_only_contents,
        )
        local_download_only_name = "local download-only.txt"
        local_download_only = (
            coverage_local_root / local_download_only_name
        )
        local_download_only_contents = (
            b"onedrive-cpp local download-only E2E\n"
        )
        local_download_only.write_bytes(
            local_download_only_contents
        )
        local_download_only_remote_path = (
            coverage_root_path / local_download_only_name
        )
        download_only_config = workspace / "download-only.toml"
        download_only_config.write_text(
            rewrite_config(
                source_text,
                sync_data_directory,
                state_directory,
                sync_list,
                "backup",
                True,
                False,
                "download_only",
            ),
            encoding="utf-8",
        )
        download_only = run_sync(
            client,
            download_only_config,
            home,
            log_file,
        )
        completed.append(download_only)
        downloaded_only = fixture_path(
            sync_data_directory,
            remote_download_only_path,
        )
        if (
            download_only.returncode != 0
            or downloaded_only.read_bytes()
            != remote_download_only_contents
            or local_download_only.read_bytes()
            != local_download_only_contents
            or coverage_fixture.item_exists(
                local_download_only_remote_path
            )
        ):
            raise E2EError(
                "download-only mode did not download remote data "
                "while preserving local-only content"
            )

        local_download_only.unlink()
        coverage_fixture.delete_root()
        cleaned_coverage = run_sync(
            client,
            config,
            home,
            log_file,
        )
        completed.append(cleaned_coverage)
        if (
            cleaned_coverage.returncode != 0
            or coverage_local_root.exists()
        ):
            raise E2EError(
                "additional coverage fixture cleanup failed"
            )
    finally:
        coverage_fixture.delete_root()
