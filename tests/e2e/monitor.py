from __future__ import annotations

from collections.abc import Callable
import os
from pathlib import Path
import signal
import subprocess
import time

from .base import E2EError, large_upload_size, sha256, write_pattern_file
from .graph import GraphMoveFixture
from .run import run_monitor_boundary
from .util import json_text_event_count, log_text_since, wait_until


def run_monitor_scenarios(
    client: Path,
    config: Path,
    home: Path,
    log_file: Path,
    workspace: Path,
    local_root: Path,
    remote_root: Path,
    fixture: GraphMoveFixture,
) -> subprocess.CompletedProcess[str]:
    large_source = workspace / "monitor-large-source.bin"
    large_size = large_upload_size()
    large_digest = write_pattern_file(large_source, large_size)

    def exercise(
        process: subprocess.Popen[str],
        stdout_path: Path,
    ) -> None:
        def require(
            predicate: Callable[[], bool],
            message: str,
            timeout: float = 180,
        ) -> None:
            def ready() -> bool:
                if process.poll() is not None:
                    return False
                return bool(predicate())

            if not wait_until(ready, timeout, 0.5):
                raise E2EError(f"{message} ({process.poll()})")

        def remote_item(path: Path) -> dict[str, object] | None:
            try:
                return fixture.item_by_path(path)
            except E2EError as error:
                if "unexpected HTTP status 404" in str(error):
                    return None
                raise

        def remote_size(path: Path, size: int) -> bool:
            item = remote_item(path)
            return item is not None and item.get("size") == size

        def remote_missing(path: Path) -> bool:
            return remote_item(path) is None

        def item_id(path: Path) -> str | None:
            item = remote_item(path)
            if item is None:
                return None
            value = item.get("id")
            return value if isinstance(value, str) and value else None

        def require_item_id(path: Path, message: str) -> str:
            value = item_id(path)
            if value is None:
                raise E2EError(message)
            return value

        def sync_count() -> int:
            try:
                return json_text_event_count(
                    stdout_path.read_text(encoding="utf-8"),
                    "sync_completed",
                )
            except FileNotFoundError:
                return 0

        source = local_root / "monitor.txt"
        created = b"monitor create\n"
        source.write_bytes(created)
        require(
            lambda: remote_size(remote_root / source.name, len(created)),
            "Monitor did not upload a created file",
        )

        modified = b"monitor modified with a different size\n"
        source.write_bytes(modified)
        require(
            lambda: remote_size(remote_root / source.name, len(modified)),
            "Monitor did not upload a modified file",
        )

        atomic_contents = b"monitor atomic-save replacement contents\n"
        atomic_source = workspace / "atomic-save.tmp"
        atomic_source.write_bytes(atomic_contents)
        os.replace(atomic_source, source)
        require(
            lambda: remote_size(
                remote_root / source.name,
                len(atomic_contents),
            ),
            "Monitor did not upload an atomic-save replacement",
        )

        before_burst = sync_count()
        burst_contents = b""
        for index in range(8):
            burst_contents = (
                f"monitor burst final generation {index}".encode("ascii")
            )
            source.write_bytes(burst_contents)
        require(
            lambda: remote_size(
                remote_root / source.name,
                len(burst_contents),
            ),
            "Monitor did not upload the final burst generation",
        )
        time.sleep(0.5)
        if sync_count() != before_burst + 1:
            raise E2EError("Monitor did not coalesce a local write burst")

        source_remote = remote_root / source.name
        source_id = require_item_id(
            source_remote,
            "Monitor file rename fixture has no Graph item ID",
        )
        renamed = local_root / "renamed.txt"
        source.rename(renamed)
        renamed_remote = remote_root / renamed.name
        require(
            lambda: (
                remote_missing(source_remote)
                and item_id(renamed_remote) == source_id
            ),
            "Monitor did not preserve a file rename",
        )

        nested = local_root / "new" / "nested"
        nested.mkdir(parents=True)
        nested_file = nested / "recursive.txt"
        nested_contents = b"monitor recursive watch\n"
        nested_file.write_bytes(nested_contents)
        nested_remote = remote_root / "new" / "nested" / nested_file.name
        require(
            lambda: remote_size(nested_remote, len(nested_contents)),
            "Monitor did not add a recursive watch",
        )
        nested_id = require_item_id(
            nested_remote,
            "Monitor directory rename fixture has no Graph item ID",
        )

        renamed_directory = local_root / "renamed-dir"
        (local_root / "new").rename(renamed_directory)
        renamed_nested_remote = (
            remote_root
            / renamed_directory.name
            / "nested"
            / nested_file.name
        )
        require(
            lambda: (
                remote_missing(nested_remote)
                and item_id(renamed_nested_remote) == nested_id
            ),
            "Monitor did not preserve a directory rename",
        )

        source_directory = local_root / "source-dir"
        target_directory = local_root / "target-dir"
        source_directory.mkdir()
        target_directory.mkdir()
        moved = source_directory / "cross.txt"
        moved_contents = b"monitor cross-directory move\n"
        moved.write_bytes(moved_contents)
        moved_remote = remote_root / source_directory.name / moved.name
        require(
            lambda: remote_size(moved_remote, len(moved_contents)),
            "Monitor did not upload the cross-directory fixture",
        )
        moved_id = require_item_id(
            moved_remote,
            "Monitor cross-directory fixture has no Graph item ID",
        )
        moved_target = target_directory / moved.name
        moved.rename(moved_target)
        moved_target_remote = (
            remote_root / target_directory.name / moved_target.name
        )
        require(
            lambda: (
                remote_missing(moved_remote)
                and item_id(moved_target_remote) == moved_id
            ),
            "Monitor did not preserve a cross-directory move",
        )

        before_transient = sync_count()
        transient = local_root / "transient.txt"
        transient.write_bytes(b"transient")
        transient.unlink()
        require(
            lambda: sync_count() > before_transient,
            "Monitor ignored a create-then-delete burst",
        )
        if not remote_missing(remote_root / transient.name):
            raise E2EError("Monitor uploaded a create-then-delete file")

        renamed.unlink()
        require(
            lambda: remote_missing(renamed_remote),
            "Monitor did not propagate a local file deletion",
        )

        large_target = local_root / "large.bin"
        os.replace(large_source, large_target)
        require(
            lambda: remote_size(
                remote_root / large_target.name,
                large_size,
            ),
            "Monitor did not complete a large upload session",
            timeout=900,
        )
        if sha256(large_target) != large_digest:
            raise E2EError("Monitor large upload source changed")

        maximum_events_path = Path("/proc/sys/fs/inotify/max_queued_events")
        try:
            maximum_events = int(
                maximum_events_path.read_text(encoding="ascii").strip()
            )
        except (OSError, ValueError) as error:
            raise E2EError(
                "cannot read the inotify queue capacity"
            ) from error
        if maximum_events <= 0 or maximum_events > 65_536:
            raise E2EError(
                f"unsupported inotify queue capacity: {maximum_events}"
            )
        overflow_log_offset = (
            log_file.stat().st_size if log_file.exists() else 0
        )
        process.send_signal(signal.SIGSTOP)
        try:
            for index in range(maximum_events + 1024):
                event = local_root / f".overflow-{index}"
                event.write_bytes(b"x")
                event.unlink()
            overflow_marker = local_root / "overflow-recovered.txt"
            overflow_contents = b"monitor overflow recovery\n"
            overflow_marker.write_bytes(overflow_contents)
        finally:
            process.send_signal(signal.SIGCONT)
        require(
            lambda: (
                "inotify queue overflowed"
                in log_text_since(log_file, overflow_log_offset)
                and remote_size(
                    remote_root / overflow_marker.name,
                    len(overflow_contents),
                )
            ),
            "Monitor did not recover from a real inotify queue overflow",
            timeout=600,
        )

    return run_monitor_boundary(
        client,
        config,
        home,
        log_file,
        workspace,
        exercise,
    )
