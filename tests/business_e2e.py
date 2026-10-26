#!/usr/bin/env python3
"""Independent, opt-in OneDrive for Business E2E tests using the real CLI."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import time

from e2e.base import (
    E2EError,
    configured_drives,
    require,
    select_business_drive as select_drive,
    toml_document,
)
from e2e.drive_runner import (
    DualClientDriveE2E,
    IsolatedDriveClient,
    run_drive_e2e_cli,
    wait_for_condition as wait_until,
)
from e2e.state import (
    credentials_snapshot,
    private_read,
    private_write,
)
from e2e.util import parse_events, section


class Client(IsolatedDriveClient):
    SCOPE_PATTERN = r"__onedrive_cpp_business_e2e_[0-9a-f]{32}"


class BusinessE2E(DualClientDriveE2E):
    SCOPE_PREFIX = "__onedrive_cpp_business_e2e_"
    EXPECTED_DRIVE_TYPE = "business"
    DISALLOWED_DRIVE_TYPES = "personal/documentLibrary"
    UNICODE_TEXT = "Business \u6d4b\u8bd5\n"
    IDENTITY_SCENARIO = "business-identity-and-isolation"
    SUITE_LABEL = "Business"


def main(argv: list[str] | None = None) -> int:
    return run_drive_e2e_cli(
        argv,
        description=__doc__,
        drive_help="One configured Drive reference; mandatory for multiple Drives",
        temp_prefix="onedrivecpp-business-e2e-",
        cancelled_label="Business E2E",
        runner_cls=BusinessE2E,
        select_drives_fn=lambda source, requested, _state, _account: select_drive(
            source, requested
        ),
    )


__all__ = [
    "BusinessE2E",
    "Client",
    "E2EError",
    "Path",
    "configured_drives",
    "credentials_snapshot",
    "main",
    "parse_events",
    "private_read",
    "private_write",
    "require",
    "section",
    "select_drive",
    "subprocess",
    "time",
    "toml_document",
    "wait_until",
]


if __name__ == "__main__":
    sys.exit(main())
