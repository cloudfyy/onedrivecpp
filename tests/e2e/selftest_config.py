from __future__ import annotations

from pathlib import Path
import socket
import tomllib

from .base import E2EError, rewrite_config
from .proxy import ConnectionDropProxy
from .util import wait_until


def self_test_config() -> None:
        source = """\
    config_version = 2
    [sync]
    data_directory = "/old/sync"
    dry_run = true
    [state]
    directory = "/old/state"
    """
        rewritten = rewrite_config(
            source,
            Path("/new/sync"),
            Path("/new/state"),
            Path("/new/sync_list"),
            "backup",
            True,
            True,
            "download_only",
        )
        parsed = tomllib.loads(rewritten)
        if (
            parsed["sync"]["data_directory"] != "/new/sync"
            or parsed["sync"]["data_mount_point"] != "/"
            or parsed["sync"]["dry_run"] is not True
            or parsed["sync"]["sync_list"] != "/new/sync_list"
            or parsed["sync"]["local_conflict"] != "backup"
            or parsed["sync"]["sync_root_files"] is not True
            or parsed["sync"]["mode"] != "download_only"
            or parsed["state"]["directory"] != "/new/state"
        ):
            raise E2EError("configuration rewrite self-test failed")
        boundary_rewrite = tomllib.loads(
            rewrite_config(
                source,
                Path("/new/sync"),
                Path("/new/state"),
                proxy_url="http://127.0.0.1:12345",
                monitor_poll_interval_seconds=60,
                monitor_settle_delay_milliseconds=100,
                upload_maximum_rate_bytes_per_second=5_000_000,
            )
        )
        if (
            boundary_rewrite["proxy"]["url"] != "http://127.0.0.1:12345"
            or boundary_rewrite["proxy"]["no_proxy"] != []
            or boundary_rewrite["monitor"]["poll_interval_seconds"] != 60
            or boundary_rewrite["monitor"]["settle_delay_milliseconds"] != 100
            or boundary_rewrite["upload"][
                "maximum_rate_bytes_per_second"
            ]
            != 5_000_000
        ):
            raise E2EError("boundary configuration rewrite self-test failed")
        with ConnectionDropProxy() as proxy:
            with socket.create_connection(
                ("127.0.0.1", int(proxy.url.rsplit(":", 1)[1])),
                timeout=2,
            ):
                pass
            if not wait_until(lambda: proxy.connection_count == 1, 2):
                raise E2EError("connection-dropping proxy self-test failed")
        try:
            rewrite_config(
                "[sync]\ndata_directory = \"/tmp\"\n",
                Path("/a"),
                Path("/b"),
            )
            raise E2EError("incomplete configuration was accepted")
        except E2EError as error:
            if "missing required keys" not in str(error):
                raise
        duplicate = source.replace(
            'data_directory = "/old/sync"',
            'data_directory = "/old/sync"\ndata_directory = "/duplicate"',
        )
        try:
            rewrite_config(duplicate, Path("/a"), Path("/b"))
            raise E2EError("duplicate configuration key was accepted")
        except E2EError as error:
            if "duplicate configuration key" not in str(error):
                raise
        existing_sync_list = source.replace(
            'data_directory = "/old/sync"',
            'data_directory = "/old/sync"\nsync_list = "/old/sync_list"',
        )
        replaced_sync_list = tomllib.loads(
            rewrite_config(
                existing_sync_list,
                Path("/new/sync"),
                Path("/new/state"),
                Path("/new/sync_list"),
            )
        )
        if replaced_sync_list["sync"]["sync_list"] != "/new/sync_list":
            raise E2EError("existing selective-sync path was not replaced")
        existing_local_conflict = source.replace(
            'data_directory = "/old/sync"',
            'data_directory = "/old/sync"\nlocal_conflict = "block"',
        )
        replaced_local_conflict = tomllib.loads(
            rewrite_config(
                existing_local_conflict,
                Path("/new/sync"),
                Path("/new/state"),
                local_conflict="backup",
            )
        )
        if replaced_local_conflict["sync"]["local_conflict"] != "backup":
            raise E2EError("existing local-conflict policy was not replaced")
        existing_root_files = source.replace(
            'data_directory = "/old/sync"',
            'data_directory = "/old/sync"\nsync_root_files = false',
        )
        replaced_root_files = tomllib.loads(
            rewrite_config(
                existing_root_files,
                Path("/new/sync"),
                Path("/new/state"),
                sync_root_files=True,
            )
        )
        if replaced_root_files["sync"]["sync_root_files"] is not True:
            raise E2EError("existing root-file policy was not replaced")
