#!/usr/bin/env python3
"""Offline tests for the independent Business E2E runner; never contact Graph."""

import argparse
import contextlib
import copy
import fcntl
import io
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys
import tempfile
import tomllib
import unittest
from unittest.mock import Mock, patch

import business_e2e as e2e


CLIENT = None
SCOPE = "__onedrive_cpp_business_e2e_" + "a" * 32


class BusinessRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="business-e2e-offline-", dir="/tmp")
        self.addCleanup(self.temporary.cleanup)
        self.home = Path(self.temporary.name)
        self.state = self.home / "original-state"
        self.account = self.state / "accounts" / "account--12345678"
        self.account.mkdir(parents=True, mode=0o700)
        self.source = {
            "config_version": 2,
            "auth": {"application_id": "00000000-0000-0000-0000-000000000000"},
            "sync": {
                "data_directory": str(self.home / "original-data"),
                "drive_ids": ["me", "second-drive"],
                "data_mount_point": "/not-a-real-mount",
                "sync_list": "/not-the-test-list",
                "dry_run": True,
                "upload": False,
                "mode": "upload_only",
                "delete_policy": "preserve",
            },
            "state": {"directory": str(self.state)},
            "logging": {"file": "/not-the-test-log", "level": "trace"},
            "proxy": {"url": "", "no_proxy": ["localhost", "*.example.test"]},
            "graph": {"endpoint": "https://graph.microsoft.com/v1.0"},
        }
        e2e.private_write(self.state / "onedrive-cpp.lock", b'{"pid":0}\n')
        e2e.private_write(self.state / "active_account", b"account--12345678\n")
        e2e.private_write(
            self.account / "account.json",
            b'{"user_id":"test","display_name":"Test"}\n',
        )
        e2e.private_write(self.account / "refresh_token", b"offline-test-token\n")
        (self.account / "drives").mkdir()
        (self.account / "drives" / "private-original.sqlite3").write_bytes(b"never copy this")

    def client(self, name="isolated", scope=SCOPE):
        return e2e.Client(
            self.home, name, self.source, e2e.credentials_snapshot(self.state),
            "me", scope, Path("/nonexistent/onedrive-cpp"), 10,
        )

    def prepare_database(self, client):
        drive = Path("accounts/account--12345678/drives/drive--12345678")
        (client.data / drive).mkdir(parents=True)
        directory = client.state / drive
        directory.mkdir(parents=True)
        with contextlib.closing(sqlite3.connect(directory / "items.sqlite3")) as database, database:
            database.executescript(
                "CREATE TABLE item(remote_path TEXT,local_path TEXT,"
                "remote_id TEXT,etag TEXT,ctag TEXT);"
                "CREATE TABLE drive_state(drive_id TEXT,delta_link TEXT);"
                "CREATE TABLE pending_delta(drive_id TEXT,delta_link TEXT);"
                "CREATE TABLE pending_delta_change(drive_id TEXT,remote_id TEXT);"
                "CREATE TABLE partial_download(remote_path TEXT,temporary_path TEXT,"
                "completed_bytes INTEGER);"
            )
        return client.root

    def runner(self):
        return e2e.BusinessE2E(
            self.home, self.source, e2e.credentials_snapshot(self.state),
            "me", Path("/nonexistent/onedrive-cpp"), 10,
        )

    def test_write_opt_in_is_checked_before_reading_any_files(self):
        with patch.object(e2e.Path, "resolve") as resolve:
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                e2e.main(["--config", "/missing/config.toml"])
        self.assertEqual(error.exception.code, 2)
        resolve.assert_not_called()

    def test_drive_selection_is_explicit_and_restricted_to_source(self):
        with self.assertRaisesRegex(e2e.E2EError, "Multiple Drives"):
            e2e.select_drive(self.source, None)
        self.assertEqual(e2e.select_drive(self.source, "second-drive"), "second-drive")
        self.assertEqual(e2e.select_drive({}, None), "me")
        for document, requested in (
            (self.source, "unconfigured"),
            ({"sync": {"drive_ids": []}}, None),
            ({"sync": {"drive_ids": "me"}}, None),
            ({"sync": {"drive_ids": [""]}}, None),
            ({"sync": {"drive_id": "me", "drive_ids": ["me"]}}, None),
        ):
            with self.subTest(document=document, requested=requested):
                with self.assertRaises(e2e.E2EError):
                    e2e.select_drive(document, requested)

    def test_configuration_and_only_minimum_credentials_are_isolated(self):
        before = {
            path.relative_to(self.state): path.read_bytes()
            for path in self.state.rglob("*") if path.is_file()
        }
        original = copy.deepcopy(self.source)
        client = self.client()
        document = tomllib.loads(client.config_path.read_text())
        self.assertEqual(self.source, original)
        self.assertEqual(document["sync"]["drive_id"], "me")
        self.assertNotIn("drive_ids", document["sync"])
        self.assertNotIn("data_mount_point", document["sync"])
        self.assertFalse(document["sync"]["dry_run"])
        self.assertEqual(document["sync"]["mode"], "download_only")
        self.assertEqual(document["sync"]["delete_policy"], "propagate")
        self.assertEqual(document["proxy"], self.source["proxy"])
        self.assertEqual(document["auth"], self.source["auth"])
        self.assertEqual(document["graph"], self.source["graph"])
        self.assertEqual(Path(document["sync"]["sync_list"]).read_text(), f"/{SCOPE}/\n")
        self.assertNotIn("file", document["logging"])
        self.assertEqual(
            {str(path.relative_to(client.state)) for path in client.state.rglob("*") if path.is_file()},
            {"active_account", "accounts/account--12345678/account.json", "accounts/account--12345678/refresh_token"},
        )
        for path in client.state.rglob("*"):
            self.assertEqual(path.stat().st_mode & 0o777, 0o700 if path.is_dir() else 0o600)
        self.assertEqual(before, {
            path.relative_to(self.state): path.read_bytes()
            for path in self.state.rglob("*") if path.is_file()
        })
        client.configure(mode="bidirectional", rate=65536, websocket=False)
        configured = tomllib.loads(client.config_path.read_text())
        self.assertEqual(configured["sync"]["delete_policy"], "propagate")
        self.assertEqual(configured["download"]["maximum_rate_bytes_per_second"], 65536)
        self.assertFalse(configured["monitor"]["websocket_enabled"])

    def test_nested_toml_strings_and_arrays_round_trip(self):
        document = {
            "config_version": 2,
            "proxy": {"url": "https://example.test", "no_proxy": ["one", "two"]},
            "graph": {"throttle": {"maximum_retries": 2, "initial_delay_seconds": 1}},
            "sync": {"dry_run": False, "data_directory": "space/quote\"\n\u6d4b\u8bd5"},
        }
        self.assertEqual(tomllib.loads(e2e.toml_document(document)), document)
        with self.assertRaisesRegex(e2e.E2EError, "Unsupported"):
            e2e.toml_document({"unsupported": 1.5})

    def test_busy_source_is_refused_and_its_lock_is_not_released(self):
        with (self.state / "onedrive-cpp.lock").open("rb") as owner:
            fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(e2e.E2EError, "Source account is busy"):
                e2e.credentials_snapshot(self.state)
            with (self.state / "onedrive-cpp.lock").open("rb") as competitor:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(competitor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.assertEqual(e2e.credentials_snapshot(self.state)[0], "account--12345678")

    def test_invalid_account_markers_and_token_permissions_are_refused(self):
        for marker in ("../outside", ".", "..", "a/b", "a\\b", "", "bad\0name"):
            e2e.private_write(self.state / "active_account", marker.encode())
            with self.subTest(marker=marker), self.assertRaises(e2e.E2EError):
                e2e.credentials_snapshot(self.state)
        e2e.private_write(self.state / "active_account", b"account--12345678\n")
        token = self.account / "refresh_token"
        token.chmod(0o644)
        with self.assertRaisesRegex(e2e.E2EError, "0600"):
            e2e.credentials_snapshot(self.state)

    def test_symlinked_credentials_and_parent_directories_are_refused(self):
        original = self.account / "refresh_token"
        target = self.home / "other-token"
        original.rename(target)
        original.symlink_to(target)
        with self.assertRaisesRegex(e2e.E2EError, "symlinks"):
            e2e.credentials_snapshot(self.state)
        original.unlink()
        target.rename(original)
        linked = self.home / "linked-state"
        linked.symlink_to(self.state, target_is_directory=True)
        with self.assertRaisesRegex(e2e.E2EError, "symlinks"):
            e2e.credentials_snapshot(linked)

    def test_empty_tokens_and_nonregular_credentials_are_refused(self):
        token = self.account / "refresh_token"
        token.write_bytes(b"")
        with self.assertRaisesRegex(e2e.E2EError, "empty"):
            e2e.credentials_snapshot(self.state)
        token.write_bytes(b" \t\r\n")
        with self.assertRaisesRegex(e2e.E2EError, "whitespace"):
            e2e.credentials_snapshot(self.state)
        token.unlink()
        os.mkfifo(token, 0o600)
        with self.assertRaisesRegex(e2e.E2EError, "regular"):
            e2e.credentials_snapshot(self.state)

    def test_scope_path_escape_and_snapshot_scope_are_rejected(self):
        with self.assertRaisesRegex(e2e.E2EError, "Invalid test subtree"):
            self.client(scope="ordinary-user-folder")
        client = self.client()
        self.prepare_database(client)
        with self.assertRaisesRegex(e2e.E2EError, "escaped"):
            client.path("../outside")
        with contextlib.closing(sqlite3.connect(client.database)) as database, database:
            database.execute(
                "INSERT INTO item VALUES(?,?,?,?,?)",
                ("unrelated/file", str(client.root / "unrelated/file"), "id", "e", "c"),
            )
        with self.assertRaisesRegex(e2e.E2EError, "outside the test subtree"):
            client.assert_scope()
        with contextlib.closing(sqlite3.connect(client.database)) as database, database:
            database.execute("UPDATE item SET remote_path=?,local_path=?", (SCOPE + "/file", "/outside"))
        with self.assertRaisesRegex(e2e.E2EError, "outside the isolated Drive"):
            client.assert_scope()

    def test_json_events_include_stderr_and_require_a_single_named_section(self):
        completed = subprocess.CompletedProcess(
            [], 1, 'not-json\n{"event":"status","values":{"drive_type":"business"}}\n',
            '{"event":"restore_busy","level":"error","message":"PID 42"}\n',
        )
        self.assertEqual(e2e.section(completed, "status"), {"drive_type": "business"})
        self.assertEqual(len(e2e.parse_events(completed)), 2)
        with self.assertRaises(e2e.E2EError):
            e2e.section(completed, "missing")
        completed.stdout += '{"event":"status"}\n'
        with self.assertRaises(e2e.E2EError):
            e2e.section(completed, "status")

    def test_failed_commands_report_codes_without_dumping_secrets(self):
        client = self.client()
        failure = subprocess.CompletedProcess(
            [], 1, "SECRET_TOKEN", '{"event":"auth_error","level":"error","message":"SECRET_TOKEN"}',
        )
        with patch.object(e2e.subprocess, "run", return_value=failure) as run:
            with self.assertRaises(e2e.E2EError) as error:
                client.run("inspect", "status")
        self.assertIn("auth_error", str(error.exception))
        self.assertNotIn("SECRET_TOKEN", str(error.exception))
        self.assertIs(run.call_args.kwargs["stdin"], subprocess.DEVNULL)
        self.assertEqual(run.call_args.kwargs["timeout"], 10)
        with patch.object(
            e2e.subprocess, "run",
            side_effect=subprocess.TimeoutExpired(["command"], 10, output="SECRET_TOKEN"),
        ):
            with self.assertRaisesRegex(e2e.E2EError, "timeout") as error:
                client.run("transfer", "sync")
        self.assertNotIn("SECRET_TOKEN", str(error.exception))

    def test_stop_all_targets_only_owned_processes_and_surfaces_forced_shutdown(self):
        client = self.client()
        process = Mock()
        process.poll.side_effect = [None, 0]
        output = io.BytesIO()
        client.processes = [(process, output)]
        client.stop_all()
        process.terminate.assert_called_once_with()
        process.kill.assert_not_called()
        self.assertTrue(output.closed)
        process.wait.side_effect = [subprocess.TimeoutExpired("watch", 10), 0]
        process.poll.side_effect = [None, 0]
        client.processes = [(process, io.BytesIO())]
        with self.assertRaisesRegex(e2e.E2EError, "forced kill"):
            client.stop_all()
        process.kill.assert_called_once_with()
        self.assertFalse(client.processes)

    def test_real_child_process_is_terminated_and_reaped(self):
        client = self.client()
        client.command = Mock(return_value=[
            sys.executable, "-c", "import time; time.sleep(60)",
        ])
        process = client.start("watch")
        self.addCleanup(lambda: client.stop_all())
        self.assertIsNone(process.poll())
        client.stop_all()
        self.assertEqual(process.returncode, -signal.SIGTERM)
        self.assertFalse(client.processes)

    def test_cleanup_uses_a_fresh_read_only_mirror_and_requires_matching_marker(self):
        runner = self.runner()
        runner.upload_attempted = True
        cleaner = Mock()
        subtree = self.home / "remote-mirror"
        subtree.mkdir()
        (subtree / "owner.txt").write_bytes(b"not-the-owner")
        cleaner.path.side_effect = lambda relative="": subtree / relative
        with patch.object(e2e, "credentials_snapshot", return_value=("account", b"{}", b"token")):
            with patch.object(e2e, "Client", return_value=cleaner):
                with self.assertRaisesRegex(e2e.E2EError, "ownership marker"):
                    runner.cleanup()
        cleaner.sync.assert_called_once_with("download_only")
        self.assertTrue(subtree.is_dir())

    def test_cleanup_deletes_only_owned_subtree_and_verifies_with_another_client(self):
        runner = self.runner()
        runner.upload_attempted = True
        cleaner, verifier = Mock(), Mock()
        subtree = self.home / "owned-remote-mirror"
        subtree.mkdir()
        (subtree / "owner.txt").write_bytes(runner.owner)
        cleaner.path.side_effect = lambda relative="": subtree / relative
        verifier.path.return_value = self.home / "already-removed"
        with patch.object(e2e, "credentials_snapshot", return_value=("account", b"{}", b"token")):
            with patch.object(e2e, "Client", side_effect=[cleaner, verifier]):
                runner.cleanup()
        self.assertFalse(subtree.exists())
        self.assertEqual(cleaner.sync.call_args_list, [unittest.mock.call("download_only"), unittest.mock.call()])
        verifier.sync.assert_called_once_with("download_only")

    def test_failure_still_stops_both_clients_and_attempts_cleanup(self):
        runner = self.runner()
        runner.identity = Mock(side_effect=e2e.E2EError("identity failed"))
        runner.a.stop_all = Mock()
        runner.b.stop_all = Mock()
        runner.cleanup = Mock(side_effect=e2e.E2EError("cleanup failed"))
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(e2e.E2EError) as error:
                runner.run()
        runner.a.stop_all.assert_called_once_with()
        runner.b.stop_all.assert_called_once_with()
        runner.cleanup.assert_called_once_with()
        self.assertIn("identity failed", str(error.exception))
        self.assertIn("Remote cleanup failed", str(error.exception))
        self.assertIn(runner.scope, str(error.exception))

    def test_cancel_cleans_up_and_does_not_hide_cleanup_failure(self):
        runner = self.runner()
        runner.identity = Mock(side_effect=KeyboardInterrupt)
        runner.cleanup = Mock()
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(KeyboardInterrupt):
                runner.run()
        runner.cleanup.assert_called_once_with()
        runner.cleanup.side_effect = e2e.E2EError("marker missing")
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(e2e.E2EError, "Remote cleanup failed"):
                runner.run()

    def test_personal_or_library_drive_fails_before_sync_or_remote_writes(self):
        runner = self.runner()
        for drive_type in ("personal", "documentLibrary", "unknown"):
            reply = subprocess.CompletedProcess(
                [], 0, json.dumps({"event": "status", "values": {
                    "drive_type": drive_type, "drive_id": "drive",
                }}), "",
            )
            with patch.object(runner.a, "run", return_value=reply):
                with patch.object(runner.b, "run", return_value=reply):
                    with patch.object(runner.a, "sync") as sync:
                        with self.assertRaisesRegex(e2e.E2EError, "driveType=business"):
                            runner.identity()
            sync.assert_not_called()
            self.assertFalse(runner.upload_attempted)

    def test_resolved_business_drive_is_pinned_for_both_clients_and_cleanup(self):
        runner = self.runner()
        completed = subprocess.CompletedProcess(
            [], 0, json.dumps({"event": "status", "values": {
                "drive_type": "business", "drive_id": "resolved-business-drive",
            }}), "",
        )
        for client in (runner.a, runner.b):
            client.run = Mock(return_value=completed)
            client.sync = Mock()
            client.path = Mock(return_value=self.home / "not-yet-created")
        runner.identity()
        for client in (runner.a, runner.b):
            generated = tomllib.loads(client.config_path.read_text())
            self.assertEqual(generated["sync"]["drive_id"], "resolved-business-drive")
            client.sync.assert_called_once_with("download_only")
        self.assertEqual(runner.drive, "resolved-business-drive")
        self.assertEqual(self.source["sync"]["drive_ids"], ["me", "second-drive"])
        different = subprocess.CompletedProcess(
            [], 0, json.dumps({"event": "status", "values": {
                "drive_type": "business", "drive_id": "different-business-drive",
            }}), "",
        )
        runner.b.run.return_value = different
        runner.a.sync.reset_mock()
        with self.assertRaisesRegex(e2e.E2EError, "different Drives"):
            runner.identity()
        runner.a.sync.assert_not_called()

    def test_real_cli_accepts_generated_configuration_without_authentication(self):
        if CLIENT is None:
            self.skipTest("Pass --client to validate isolated TOML with the actual CLI")
        client = self.client()
        client.executable = CLIENT
        completed = client.run("inspect", "config")
        self.assertTrue(e2e.parse_events(completed))
        self.assertFalse(list(client.state.glob("accounts/*/drives/*/items.sqlite3")))

    def test_real_monitor_commands_reach_control_handler_without_authentication(self):
        if CLIENT is None:
            self.skipTest("Pass --client to exercise real CLI monitor commands")
        self.source["console"] = {"ui": "tui", "theme": "hacker"}
        client = self.client()
        client.executable = CLIENT
        client.config["auth"]["endpoint"] = "https://127.0.0.1:1"
        client.config["graph"]["endpoint"] = "https://127.0.0.1:1/v1.0"
        client.configure()
        for operation in ("status", "trigger", "stop"):
            with self.subTest(operation=operation):
                completed = client.run("monitor", operation, expected=(1,))
                self.assertIn("Monitor control endpoint is unavailable", completed.stderr)
        self.assertEqual(self.source["console"]["ui"], "tui")
        self.assertFalse(list(client.state.glob("accounts/*/drives/*/items.sqlite3")))

    def test_watch_readiness_retries_until_own_successful_round(self):
        runner = self.runner()
        process = Mock(pid=42)
        process.poll.return_value = None
        runner.a.start = Mock(return_value=process)
        runner.busy_restore = Mock(side_effect=e2e.E2EError("ready-boundary"))

        def status(pid=42, rounds=1, result=0):
            return subprocess.CompletedProcess(
                [], 0, json.dumps({
                    "event": "monitor_control",
                    "values": {
                        "pid": str(pid), "completed_rounds": str(rounds),
                        "last_result": str(result),
                    },
                }), "",
            )

        replies = [
            subprocess.CompletedProcess([], 1, "", "endpoint unavailable"),
            status(rounds=0),
            status(pid=43),
            status(result=3),
            status(),
        ]
        with patch.object(e2e.subprocess, "run", side_effect=replies) as run:
            with patch.object(e2e.time, "sleep"):
                with self.assertRaisesRegex(e2e.E2EError, "ready-boundary"):
                    runner.watch(websocket=True)
        self.assertEqual(run.call_count, len(replies))
        runner.busy_restore.assert_called_once_with(process)

    def test_watch_readiness_does_not_retry_cli_argument_errors(self):
        runner = self.runner()
        process = Mock()
        process.poll.return_value = None
        runner.a.start = Mock(return_value=process)
        usage_error = subprocess.CompletedProcess([], 2, "", "unknown option")
        with patch.object(e2e.subprocess, "run", return_value=usage_error) as run:
            with self.assertRaisesRegex(e2e.E2EError, "monitor status exited 2"):
                runner.watch(websocket=True)
        run.assert_called_once()

    def test_busy_restore_assertion_matches_real_cli_before_authentication(self):
        if CLIENT is None:
            self.skipTest("Pass --client to exercise real CLI busy refusal")
        runner = self.runner()
        runner.a.executable = CLIENT
        runner.a.config["auth"]["endpoint"] = "https://127.0.0.1:1"
        runner.a.config["graph"]["endpoint"] = "https://127.0.0.1:1/v1.0"
        runner.a.configure()
        lock = runner.a.state / "onedrive-cpp.lock"
        e2e.private_write(lock, json.dumps({
            "pid": os.getpid(),
            "operation": "watch",
            "started_at": "2026-01-01T00:00:00Z",
            "state_directory": str(runner.a.state),
            "sync_directory": "",
        }).encode())
        process = Mock(pid=os.getpid())
        process.poll.return_value = None
        with lock.open("rb") as owner:
            fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
            runner.busy_restore(process)
        self.assertFalse(list(runner.a.state.glob("accounts/*/drives/*/items.sqlite3")))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--client", type=Path)
    arguments, remaining = parser.parse_known_args()
    if arguments.client:
        CLIENT = arguments.client.resolve(strict=True)
    unittest.main(argv=[sys.argv[0], *remaining])
