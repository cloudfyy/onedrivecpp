from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess

from .base import E2EError
from .boundary import run_boundary
from .live import run_live
from .selftest import self_test


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", type=Path)
    parser.add_argument("--work-root", type=Path)
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
            if arguments.client is None or arguments.work_root is None:
                raise E2EError("--client and --work-root are required")
            client = arguments.client.resolve()
            work_root = arguments.work_root.resolve()
            if arguments.boundary is None:
                run_live(client, work_root)
            else:
                run_boundary(client, work_root, arguments.boundary)
    except (E2EError, OSError, subprocess.SubprocessError) as error:
        print(f"Graph synchronization E2E failed: {error}", file=os.sys.stderr)
        return 1
    return 0
