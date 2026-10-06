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
from .graph import GraphMoveFixture, refresh_graph_access_token
from .proxy import ConnectionDropProxy
from .run import (
    reset_copied_state,
    run_monitor_boundary,
    run_sync,
    run_sync_after_stop,
    run_sync_until_upload_progress_then_kill,
)
from .state import pending_upload_row, tracked_drive_id, tracked_item_count
from .util import has_json_event, save_artifacts


def run_boundary(
    client: Path,
    work_root: Path,
    scenario: str,
) -> None:
    scenarios = {"network", "storage", "permission", "crash", "monitor"}
    if scenario not in scenarios:
        raise E2EError(f"unknown system boundary scenario: {scenario}")
    settings = load_live_settings(client)
    work_root.mkdir(parents=True, exist_ok=True)
    completed: list[subprocess.CompletedProcess[str]] = []
    with tempfile.TemporaryDirectory(
        prefix=f"graph-boundary-{scenario}-",
        dir=work_root,
    ) as raw:
        workspace = Path(raw)
        sync_directory = workspace / "sync"
        state_directory = workspace / "state"
        home = workspace / "home"
        config = workspace / "config.toml"
        sync_list = workspace / "sync_list"
        log_file = workspace / "onedrive-cpp.log"
        home.mkdir()
        shutil.copytree(settings.source_state, state_directory)
        sync_list.write_text(
            sync_list_rule(settings.expected_path),
            encoding="utf-8",
        )
        config.write_text(
            rewrite_config(
                settings.source_text,
                sync_directory,
                state_directory,
                sync_list,
                "backup",
                False,
            ),
            encoding="utf-8",
        )
        fixture: GraphMoveFixture | None = None
        boundary_root_path = Path(
            "__onedrive_cpp_boundary_"
            + scenario
            + "_"
            + secrets.token_hex(8)
        )
        boundary_local_root: Path | None = None
        try:
            reset = reset_copied_state(client, config, home)
            completed.append(reset)
            if reset.returncode != 0:
                raise E2EError(
                    f"{scenario} boundary state reset failed with "
                    f"{reset.returncode}"
                )
            baseline = run_sync(client, config, home, log_file)
            completed.append(baseline)
            if baseline.returncode != 0:
                raise E2EError(
                    f"{scenario} boundary baseline failed with "
                    f"{baseline.returncode}"
                )
            baseline_file = fixture_path(
                sync_directory,
                settings.expected_path,
            )
            if sha256(baseline_file) != settings.expected_sha256:
                raise E2EError(
                    f"{scenario} boundary baseline fixture changed"
                )
            graph_config = settings.source_config.get("graph", {})
            if not isinstance(graph_config, dict):
                raise E2EError("E2E configuration graph table is invalid")
            graph_endpoint = graph_config.get(
                "endpoint",
                "https://graph.microsoft.com/v1.0",
            )
            if not isinstance(graph_endpoint, str) or not graph_endpoint:
                raise E2EError("E2E Graph endpoint is invalid")
            fixture = GraphMoveFixture(
                graph_endpoint,
                tracked_drive_id(
                    state_directory,
                    settings.expected_path,
                ),
                refresh_graph_access_token(
                    settings.source_config,
                    state_directory,
                ),
            )
            boundary_local_root = (
                sync_root_for_fixture(
                    baseline_file,
                    settings.expected_path,
                )
                / boundary_root_path
            )
            sync_list.write_text(
                sync_list_rule(settings.expected_path)
                + sync_list_subtree_rule(boundary_root_path),
                encoding="utf-8",
            )
            config.write_text(
                rewrite_config(
                    settings.source_text,
                    sync_directory,
                    state_directory,
                    sync_list,
                    "backup",
                    False,
                ),
                encoding="utf-8",
            )
            refreshed = run_sync(client, config, home, log_file)
            completed.append(refreshed)
            if refreshed.returncode != 0:
                raise E2EError(
                    f"{scenario} boundary filter refresh failed with "
                    f"{refreshed.returncode}"
                )

            def capture_root() -> None:
                if fixture is None:
                    raise E2EError("boundary Graph fixture is unavailable")
                item = fixture.item_by_path(boundary_root_path)
                item_id = item.get("id")
                if not isinstance(item_id, str) or not item_id:
                    raise E2EError(
                        f"{scenario} boundary root has no Graph item ID"
                    )
                fixture.root_id = item_id

            if scenario == "network":
                with ConnectionDropProxy() as proxy:
                    network_config = workspace / "network-failure.toml"
                    network_config.write_text(
                        rewrite_config(
                            settings.source_text,
                            sync_directory,
                            state_directory,
                            sync_list,
                            "backup",
                            False,
                            proxy_url=proxy.url,
                        ),
                        encoding="utf-8",
                    )
                    failed = run_sync(
                        client,
                        network_config,
                        home,
                        log_file,
                    )
                    completed.append(failed)
                    if failed.returncode == 0 or proxy.connection_count == 0:
                        raise E2EError(
                            "network boundary did not fail through the "
                            "connection-dropping proxy"
                        )
                recovered = run_sync(client, config, home, log_file)
                completed.append(recovered)
                if recovered.returncode != 0:
                    raise E2EError("network boundary did not recover")

            elif scenario == "storage":
                boundary_local_root.mkdir(parents=True)
                source = boundary_local_root / "storage.txt"
                contents = b"onedrive-cpp local storage boundary E2E\n"
                source.write_bytes(contents)
                remote_path = boundary_root_path / source.name
                collisions: list[Path] = []

                def block_snapshot(process_id: int) -> None:
                    for attempt in range(1, 101):
                        collision = source.with_name(
                            f".{source.name}.onedrive-upload-"
                            f"{process_id}-{attempt}"
                        )
                        collision.write_bytes(b"occupied")
                        collisions.append(collision)

                try:
                    failed = run_sync_after_stop(
                        client,
                        config,
                        home,
                        log_file,
                        block_snapshot,
                    )
                    completed.append(failed)
                    pending = pending_upload_row(
                        state_directory,
                        remote_path,
                    )
                    if (
                        failed.returncode != 2
                        or pending is None
                        or pending[0] != "local_storage"
                        or fixture.item_exists(remote_path)
                    ):
                        raise E2EError(
                            "storage boundary failure was not persisted"
                        )
                finally:
                    for collision in collisions:
                        collision.unlink(missing_ok=True)
                recovered = run_sync(client, config, home, log_file)
                completed.append(recovered)
                capture_root()
                if (
                    recovered.returncode != 0
                    or pending_upload_row(state_directory, remote_path)
                    is not None
                    or fixture.item_by_path(remote_path).get("size")
                    != len(contents)
                ):
                    raise E2EError("storage boundary did not recover")

            elif scenario == "permission":
                if os.geteuid() == 0:
                    raise E2EError(
                        "permission boundary E2E must run as a non-root user"
                    )
                boundary_local_root.mkdir(parents=True)
                source = boundary_local_root / "permission.txt"
                contents = b"onedrive-cpp local permission boundary E2E\n"
                source.write_bytes(contents)
                remote_path = boundary_root_path / source.name
                source.chmod(0)
                try:
                    failed = run_sync(client, config, home, log_file)
                    completed.append(failed)
                    pending = pending_upload_row(
                        state_directory,
                        remote_path,
                    )
                    if (
                        failed.returncode != 2
                        or pending is None
                        or pending[0] != "local_permission"
                        or fixture.item_exists(remote_path)
                    ):
                        raise E2EError(
                            "permission boundary failure was not persisted"
                        )
                finally:
                    source.chmod(0o600)
                recovered = run_sync(client, config, home, log_file)
                completed.append(recovered)
                capture_root()
                if (
                    recovered.returncode != 0
                    or pending_upload_row(state_directory, remote_path)
                    is not None
                    or fixture.item_by_path(remote_path).get("size")
                    != len(contents)
                ):
                    raise E2EError("permission boundary did not recover")

            elif scenario == "crash":
                boundary_local_root.mkdir(parents=True)
                source = boundary_local_root / "session.bin"
                try:
                    size = int(
                        os.environ.get(
                            "ONEDRIVE_E2E_LARGE_UPLOAD_BYTES",
                            "250000001",
                        )
                    )
                except ValueError as error:
                    raise E2EError(
                        "ONEDRIVE_E2E_LARGE_UPLOAD_BYTES must be an integer"
                    ) from error
                if size > 1_000_000_000:
                    raise E2EError(
                        "ONEDRIVE_E2E_LARGE_UPLOAD_BYTES exceeds 1 GB"
                    )
                digest = write_pattern_file(source, size)
                remote_path = boundary_root_path / source.name
                config.write_text(
                    rewrite_config(
                        settings.source_text,
                        sync_directory,
                        state_directory,
                        sync_list,
                        "backup",
                        False,
                        upload_maximum_rate_bytes_per_second=5_000_000,
                    ),
                    encoding="utf-8",
                )
                crashed, progress = (
                    run_sync_until_upload_progress_then_kill(
                        client,
                        config,
                        home,
                        log_file,
                        state_directory,
                        remote_path,
                    )
                )
                completed.append(crashed)
                if progress[2] <= 0 or not Path(progress[3]).is_file():
                    raise E2EError(
                        "crash boundary did not preserve a resumable journal"
                    )
                config.write_text(
                    rewrite_config(
                        settings.source_text,
                        sync_directory,
                        state_directory,
                        sync_list,
                        "backup",
                        False,
                        upload_maximum_rate_bytes_per_second=0,
                    ),
                    encoding="utf-8",
                )
                recovered = run_sync(client, config, home, log_file)
                completed.append(recovered)
                capture_root()
                if (
                    recovered.returncode != 0
                    or pending_upload_row(state_directory, remote_path)
                    is not None
                    or fixture.item_by_path(remote_path).get("size") != size
                    or sha256(source) != digest
                ):
                    raise E2EError("crash boundary did not resume safely")

            elif scenario == "monitor":
                boundary_local_root.mkdir(parents=True)
                source = boundary_local_root / "monitor.txt"
                contents = b"onedrive-cpp monitor boundary E2E\n"
                remote_path = boundary_root_path / source.name
                config.write_text(
                    rewrite_config(
                        settings.source_text,
                        sync_directory,
                        state_directory,
                        sync_list,
                        "backup",
                        False,
                        monitor_poll_interval_seconds=60,
                        monitor_settle_delay_milliseconds=100,
                    ),
                    encoding="utf-8",
                )

                def uploaded() -> bool:
                    try:
                        return (
                            fixture.item_by_path(remote_path).get("size")
                            == len(contents)
                        )
                    except E2EError:
                        return False

                result = run_monitor_boundary(
                    client,
                    config,
                    home,
                    log_file,
                    workspace,
                    lambda: source.write_bytes(contents),
                    uploaded,
                )
                completed.append(result)
                capture_root()
                if (
                    not has_json_event(result, "local_item_uploaded")
                    or tracked_item_count(state_directory, remote_path) != 1
                ):
                    raise E2EError(
                        "monitor boundary did not persist its upload"
                    )
                settled = run_sync(client, config, home, log_file)
                completed.append(settled)
                if settled.returncode != 0:
                    raise E2EError(
                        "monitor boundary did not settle its Graph delta"
                    )
        except Exception:
            save_artifacts(completed, log_file)
            raise
        finally:
            if fixture is not None:
                if fixture.root_id is None:
                    try:
                        root_item = fixture.item_by_path(boundary_root_path)
                        root_id = root_item.get("id")
                        if isinstance(root_id, str) and root_id:
                            fixture.root_id = root_id
                    except E2EError:
                        pass
                fixture.delete_root()
