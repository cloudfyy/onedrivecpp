#!/usr/bin/env python3
"""Independent, opt-in SharePoint documentLibrary E2E tests using the real CLI."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import time

from e2e.base import (
    E2EError,
    configured_drives,
    require,
    saved_drive_types,
    select_sharepoint_drives as select_drive,
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
    SCOPE_PATTERN = r"__onedrive_cpp_sharepoint_e2e_[0-9a-f]{32}"


class SharePointE2E(DualClientDriveE2E):
    SCOPE_PREFIX = "__onedrive_cpp_sharepoint_e2e_"
    EXPECTED_DRIVE_TYPE = "documentLibrary"
    DISALLOWED_DRIVE_TYPES = "personal/business"
    UNICODE_TEXT = "SharePoint \u6d4b\u8bd5\n"
    IDENTITY_SCENARIO = "sharepoint-document-library-identity-and-isolation"
    SUITE_LABEL = "SharePoint documentLibrary"


def main(argv: list[str] | None = None) -> int:
    return run_drive_e2e_cli(
        argv,
        description=__doc__,
        drive_help="Configured SharePoint documentLibrary Drive ID; auto-selected when unique",
        temp_prefix="onedrivecpp-sharepoint-e2e-",
        cancelled_label="SharePoint E2E",
        runner_cls=SharePointE2E,
        select_drives_fn=lambda source, requested, state, account: select_drive(
            source,
            requested,
            known_types=saved_drive_types(state, account),
        ),
    )


__all__ = [
    "Client",
    "E2EError",
    "Path",
    "SharePointE2E",
    "configured_drives",
    "credentials_snapshot",
    "main",
    "parse_events",
    "private_read",
    "private_write",
    "require",
    "saved_drive_types",
    "section",
    "select_drive",
    "subprocess",
    "time",
    "toml_document",
    "wait_until",
]


if __name__ == "__main__":
    sys.exit(main())
