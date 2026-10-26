from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from .base import (
    DEFAULT_CLIENT_PATH,
    DEFAULT_CONFIG_PATH,
    E2EError,
    load_live_settings,
)
from .boundary import run_boundary
from .live import run_live
from .selftest import self_test


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--client",
        type=Path,
        default=DEFAULT_CLIENT_PATH,
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=DEFAULT_CONFIG_PATH,
    )
    parser.add_argument(
        "--drive",
        help="Personal Drive reference when sync.drive_ids has multiple Drives",
    )
    parser.add_argument("--work-root", type=Path)
    parser.add_argument(
        "--large-upload-bytes",
        type=int,
        default=250_000_001,
    )
    parser.add_argument("--artifact-dir", type=Path)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument(
        "--boundary",
        choices=("network", "storage", "permission", "crash", "monitor"),
    )
    arguments = parser.parse_args()
    try:
        if arguments.self_test:
            if arguments.boundary is not None:
                raise E2EError("--self-test and --boundary cannot be combined")
            self_test()
        else:
            client = arguments.client.expanduser().resolve()
            settings = load_live_settings(
                client,
                arguments.config,
                drive=arguments.drive,
                large_upload_bytes=arguments.large_upload_bytes,
                artifact_dir=arguments.artifact_dir,
            )
            if arguments.work_root is not None:
                work_root = arguments.work_root.expanduser().resolve()
                if arguments.boundary is None:
                    run_live(client, work_root, settings)
                else:
                    run_boundary(
                        client,
                        work_root,
                        arguments.boundary,
                        settings,
                    )
            else:
                with tempfile.TemporaryDirectory(
                    prefix="onedrive-cpp-personal-e2e-"
                ) as temp_root:
                    work_root = Path(temp_root)
                    if arguments.boundary is None:
                        run_live(client, work_root, settings)
                    else:
                        run_boundary(
                            client,
                            work_root,
                            arguments.boundary,
                            settings,
                        )
    except (E2EError, OSError, subprocess.SubprocessError) as error:
        print(f"Graph synchronization E2E failed: {error}", file=os.sys.stderr)
        return 1
    return 0
