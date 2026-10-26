#!/usr/bin/env python3
"""Offline unit tests for the opt-in SharePoint documentLibrary E2E runner."""

from __future__ import annotations

import argparse
from contextlib import closing, redirect_stderr, redirect_stdout
import fcntl
import io
import json
from pathlib import Path
import shutil
import sqlite3
import tempfile
import textwrap
import threading
import tomllib
import unittest
from unittest.mock import patch

import sharepoint_e2e


REAL_CLIENT: Path | None = None


def write_source_fixture(
    root: Path,
    *,
    drives: list[str] | None = None,
    saved_types: dict[str, str] | None = None,
) -> tuple[Path, Path, Path]:
    state = root / "source-state"
    data = root / "source-data"
    account = state / "accounts" / "user--0123456789abcdef"
    account.mkdir(parents=True, mode=0o700)
    data.mkdir(parents=True, mode=0o700)
    (data / "keep-local.txt").write_text("source data must remain untouched\n", encoding="utf-8")
    sharepoint_e2e.private_write(state / "onedrive-cpp.lock", b'{"pid":0}\n')
    sharepoint_e2e.private_write(state / "active_account", b"user--0123456789abcdef\n")
    sharepoint_e2e.private_write(
        account / "account.json",
        b'{"display_name":"Test User","user_id":"user-1"}\n',
    )
    sharepoint_e2e.private_write(account / "refresh_token", b"refresh-token-value\n")
    if saved_types:
        drives_dir = account / "drives"
        drives_dir.mkdir(mode=0o700)
        for index, (drive_id, drive_type) in enumerate(saved_types.items()):
            entry = drives_dir / f"drive-{index}"
            entry.mkdir(mode=0o700)
            sharepoint_e2e.private_write(
                entry / "drive.json",
                json.dumps(
                    {
                        "configured_drive_id": drive_id,
                        "drive_id": drive_id,
                        "name": f"Drive {index}",
                        "drive_type": drive_type,
                    }
                ).encode("utf-8"),
            )
    config = root / "config.toml"
    drive_setting = (
        f"drive_ids = {json.dumps(drives)}"
        if drives is not None
        else 'drive_ids = ["me", "b!sp-doclib-1"]'
    )
    config.write_text(
        textwrap.dedent(
            f"""
            config_version = 2
            [state]
            directory = "{state}"
            [sync]
            data_directory = "{data}"
            {drive_setting}
            """
        ).strip()
        + "\n",
        encoding="utf-8",
    )
    return config, state, data


def write_fake_cli(path: Path, cloud: Path, *, drive_types: dict[str, str] | None = None):
    mapping = json.dumps(
        drive_types
        or {
            "me": "business",
            "b!biz-drive": "business",
            "b!sp-doclib-1": "documentLibrary",
        }
    )
    path.write_text(
        textwrap.dedent(
            f'''\
            #!/usr/bin/env python3
            from contextlib import closing, contextmanager
            import fcntl, hashlib, json, os, shutil, signal, sqlite3, sys, tempfile, time, tomllib
            from pathlib import Path

            CLOUD = Path({str(cloud)!r})
            DRIVE_TYPES = {mapping}
            args = sys.argv[1:]
            config_path = Path(args[args.index("--config") + 1])
            with config_path.open("rb") as handle:
                config = tomllib.load(handle)
            state = Path(config["state"]["directory"])
            data = Path(config["sync"]["data_directory"])
            configured_drive = config["sync"].get("drive_id", "me")
            drive_type = DRIVE_TYPES.get(configured_drive, "documentLibrary")
            drive_slug = hashlib.sha256(configured_drive.encode()).hexdigest()[:12]
            root = data / "accounts" / "user--0123456789abcdef" / "drives" / f"Documents--{{drive_slug}}"
            state_drive = state / "accounts" / "user--0123456789abcdef" / "drives" / f"Documents--{{drive_slug}}"
            db_path = state_drive / "items.sqlite3"
            scope = Path(config["sync"]["sync_list"]).read_text(encoding="utf-8").strip().strip("/")
            pid_file = state / "watch.pid"
            stop_file = state / "watch.stop"
            busy_file = state / "busy.pid"
            cloud_meta = CLOUD / "meta.json"
            cloud_lock_file = CLOUD / "cloud.lock"
            cloud_tmp = CLOUD / ".tmp"
            cloud_files = CLOUD / "files"
            cloud_files.mkdir(parents=True, exist_ok=True)
            cloud_tmp.mkdir(parents=True, exist_ok=True)

            def atomic_write_bytes(path, raw, mode=None):
                path.parent.mkdir(parents=True, exist_ok=True)
                cloud_tmp.mkdir(parents=True, exist_ok=True)
                fd, tmp_name = tempfile.mkstemp(prefix=f".{{path.name}}.", dir=str(cloud_tmp))
                tmp_path = Path(tmp_name)
                try:
                    with os.fdopen(fd, "wb") as handle:
                        if mode is not None:
                            os.fchmod(handle.fileno(), mode)
                        handle.write(raw)
                        handle.flush()
                    os.replace(tmp_path, path)
                finally:
                    tmp_path.unlink(missing_ok=True)

            def atomic_write_text(path, text):
                atomic_write_bytes(path, text.encode("utf-8"))

            @contextmanager
            def cloud_lock():
                CLOUD.mkdir(parents=True, exist_ok=True)
                with cloud_lock_file.open("a+b") as handle:
                    fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
                    try:
                        yield
                    finally:
                        fcntl.flock(handle.fileno(), fcntl.LOCK_UN)

            def emit(payload):
                print(json.dumps(payload), flush=True)

            def connect():
                root.mkdir(parents=True, exist_ok=True)
                state_drive.mkdir(parents=True, exist_ok=True)
                lock_path = state / "onedrive-cpp.lock"
                if not lock_path.exists():
                    atomic_write_bytes(lock_path, b'{{"pid":0}}\\n', mode=0o600)
                db = sqlite3.connect(db_path)
                db.executescript("""
                    CREATE TABLE IF NOT EXISTS item(remote_id TEXT, remote_path TEXT PRIMARY KEY, local_path TEXT, kind TEXT, etag TEXT, ctag TEXT, sha256 TEXT);
                    CREATE TABLE IF NOT EXISTS drive_state(id INTEGER PRIMARY KEY, delta_token TEXT);
                    CREATE TABLE IF NOT EXISTS pending_delta(id INTEGER PRIMARY KEY, token TEXT);
                    CREATE TABLE IF NOT EXISTS pending_delta_change(seq INTEGER PRIMARY KEY, remote_id TEXT);
                    CREATE TABLE IF NOT EXISTS partial_download(remote_id TEXT, remote_path TEXT PRIMARY KEY, local_path TEXT, temporary_path TEXT, completed_bytes INTEGER);
                    CREATE TABLE IF NOT EXISTS blocked_item(remote_path TEXT PRIMARY KEY, reason_code TEXT);
                """)
                db.execute("INSERT OR IGNORE INTO drive_state VALUES (1, 'delta-1')")
                db.commit()
                return db

            def load_cloud():
                if cloud_meta.exists():
                    return json.loads(cloud_meta.read_text(encoding="utf-8"))
                return {{"next": 1, "items": {{}}}}

            def save_cloud(meta):
                atomic_write_text(cloud_meta, json.dumps(meta, sort_keys=True))

            def digest(raw):
                return hashlib.sha256(raw).hexdigest()

            def tag(prefix, raw, path):
                return f"{{prefix}}-{{digest(raw)[:12]}}-{{hashlib.sha256(path.encode()).hexdigest()[:8]}}"

            def tracked(db):
                with closing(db.cursor()) as cur:
                    rows = cur.execute("SELECT remote_path,remote_id,local_path,kind,etag,ctag,sha256 FROM item").fetchall()
                return {{row[0]: dict(zip(("remote_path","remote_id","local_path","kind","etag","ctag","sha256"), row)) for row in rows}}

            def scan_local():
                base = root / scope
                entries = {{}}
                if not base.exists():
                    return entries
                entries[scope] = ("directory", b"")
                for item in sorted(base.rglob("*")):
                    rel = scope + "/" + item.relative_to(base).as_posix()
                    entries[rel] = ("directory", b"") if item.is_dir() else ("file", item.read_bytes())
                return entries

            def record(db, remote_path, info):
                local_path = str(root / remote_path)
                db.execute(
                    "INSERT OR REPLACE INTO item VALUES (?,?,?,?,?,?,?)",
                    (info["id"], remote_path, local_path, info["kind"], info["etag"], info["ctag"], info["sha256"]),
                )

            def sync(dry_run=False):
                if dry_run:
                    return 0
                with closing(connect()) as db:
                    with cloud_lock():
                        meta = load_cloud()
                        cloud_items = meta["items"]
                        known = tracked(db)
                        mode = config["sync"]["mode"]
                        rate = int(config["download"]["maximum_rate_bytes_per_second"])
                        if mode in ("bidirectional", "upload_only"):
                            local = scan_local()
                            removed = [path for path in list(known) if path not in local]
                            added = [path for path in local if path not in known]
                            for old in list(removed):
                                old_row = known[old]
                                for new in list(added):
                                    kind, raw = local[new]
                                    if kind == old_row["kind"] == "file" and digest(raw) == old_row["sha256"]:
                                        info = cloud_items.pop(old)
                                        (cloud_files / old).unlink(missing_ok=True)
                                        info["etag"] = tag("etag", raw, new)
                                        cloud_items[new] = info
                                        atomic_write_bytes(cloud_files / new, raw)
                                        db.execute("DELETE FROM item WHERE remote_path=?", (old,))
                                        record(db, new, info)
                                        removed.remove(old)
                                        added.remove(new)
                                        break
                                    if kind == old_row["kind"] == "directory" and old.count("/") == new.count("/"):
                                        for path in [ key for key in list(cloud_items) if key == old or key.startswith(old + "/") ]:
                                            moved = new + path[len(old):]
                                            info = cloud_items.pop(path)
                                            raw_child = (cloud_files / path).read_bytes() if info["kind"] == "file" else b""
                                            info["etag"] = tag("etag", raw_child, moved)
                                            cloud_items[moved] = info
                                            if info["kind"] == "file":
                                                target = cloud_files / moved
                                                target.parent.mkdir(parents=True, exist_ok=True)
                                                shutil.move(cloud_files / path, target)
                                        for path in [ key for key in list(known) if key == old or key.startswith(old + "/") ]:
                                            moved = new + path[len(old):]
                                            db.execute("DELETE FROM item WHERE remote_path=?", (path,))
                                            record(db, moved, cloud_items[moved])
                                        removed = [path for path in removed if path != old and not path.startswith(old + "/")]
                                        added = [path for path in added if path != new and not path.startswith(new + "/")]
                                        break
                            known = tracked(db)
                            for path in sorted(removed, reverse=True):
                                cloud_items.pop(path, None)
                                target = cloud_files / path
                                if target.is_file():
                                    target.unlink()
                                db.execute("DELETE FROM item WHERE remote_path=?", (path,))
                            for path, (kind, raw) in local.items():
                                prev = known.get(path)
                                if prev and prev["kind"] == kind and (kind == "directory" or prev["sha256"] == digest(raw)):
                                    continue
                                ident = prev["remote_id"] if prev else f"id-{{meta['next']}}"
                                if not prev:
                                    meta["next"] += 1
                                info = {{
                                    "id": ident,
                                    "kind": kind,
                                    "sha256": digest(raw),
                                    "ctag": tag("ctag", raw, kind),
                                    "etag": tag("etag", raw, path),
                                }}
                                cloud_items[path] = info
                                if kind == "file":
                                    atomic_write_bytes(cloud_files / path, raw)
                                record(db, path, info)
                            save_cloud(meta)
                        if mode == "upload_only":
                            db.commit()
                            return 0
                        cloud_snapshot = {{
                            path: (
                                dict(info),
                                (cloud_files / path).read_bytes() if info["kind"] == "file" else b"",
                            )
                            for path, info in sorted(cloud_items.items())
                        }}
                    known = tracked(db)
                    for path in sorted(set(known) - set(cloud_snapshot), reverse=True):
                        target = root / path
                        if target.is_file():
                            target.unlink()
                        elif target.is_dir():
                            shutil.rmtree(target, ignore_errors=True)
                        db.execute("DELETE FROM item WHERE remote_path=?", (path,))
                    for path, (info, raw) in sorted(cloud_snapshot.items()):
                        if path != scope and not path.startswith(scope + "/"):
                            continue
                        target = root / path
                        prev = known.get(path)
                        if info["kind"] == "directory":
                            target.mkdir(parents=True, exist_ok=True)
                            record(db, path, info)
                            continue
                        if (
                            target.exists()
                            and prev
                            and digest(target.read_bytes()) != prev["sha256"]
                            and prev["sha256"] != info["sha256"]
                            and digest(target.read_bytes()) != info["sha256"]
                        ):
                            db.execute("INSERT OR REPLACE INTO blocked_item VALUES (?,?)", (path, "local_modification_conflict"))
                            emit({{"event": "item_blocked", "level": "warning", "reason_code": "local__modification_conflict", "path": path}})
                            db.commit()
                            return 3
                        db.execute("DELETE FROM blocked_item WHERE remote_path=?", (path,))
                        if rate > 0 and path.endswith("resume.bin") and len(raw) > 65536:
                            part = target.with_name(".onedrive-part-resume")
                            part.parent.mkdir(parents=True, exist_ok=True)
                            part.write_bytes(raw[:65536])
                            db.execute("INSERT OR REPLACE INTO partial_download VALUES (?,?,?,?,?)", (info["id"], path, str(target), str(part), 65536))
                            db.commit()
                            atomic_write_text(busy_file, str(os.getpid()))
                            def stop(_sig, _frame):
                                busy_file.unlink(missing_ok=True)
                                sys.exit(130)
                            signal.signal(signal.SIGTERM, stop)
                            time.sleep(30)
                        part_row = db.execute("SELECT temporary_path FROM partial_download WHERE remote_path=?", (path,)).fetchone()
                        if part_row:
                            part = Path(part_row[0])
                            with part.open("r+b") as handle:
                                handle.seek(0)
                                handle.write(raw)
                                handle.truncate(len(raw))
                            os.replace(part, target)
                            db.execute("DELETE FROM partial_download WHERE remote_path=?", (path,))
                        elif not target.exists() or not prev or prev["sha256"] != info["sha256"]:
                            atomic_write_bytes(target, raw)
                        record(db, path, info)
                    db.commit()
                return 0

            effective_drive_id = "drive-business-1" if drive_type == "business" else configured_drive
            if args[:2] == ["inspect", "status"]:
                emit({{
                    "event": "status",
                    "values": {{
                        "drive_type": drive_type,
                        "drive_id": effective_drive_id,
                    }},
                }})
                sys.exit(0)
            if args[:2] == ["inspect", "config"]:
                emit({{
                    "event": "effective_config",
                    "values": {{
                        "drive_id": effective_drive_id,
                        "data_directory": str(data),
                        "state_directory": str(state),
                    }},
                }})
                sys.exit(0)
            if args[:2] == ["inspect", "quota"]:
                emit({{
                    "event": "quota",
                    "values": {{
                        "drive_id": effective_drive_id,
                        "state": "normal",
                    }},
                }})
                sys.exit(0)
            if args[:2] == ["inspect", "drives"]:
                emit({{
                    "event": "drive",
                    "values": {{
                        "drive_id": effective_drive_id,
                        "type": drive_type,
                    }},
                }})
                sys.exit(0)
            if args[:2] == ["inspect", "sites"]:
                emit({{
                    "event": "site_drive",
                    "values": {{
                        "site_id": "site-1",
                        "drive_id": effective_drive_id,
                        "type": drive_type,
                    }},
                }})
                sys.exit(0)
            if args[:2] == ["inspect", "storage"]:
                with closing(connect()) as db:
                    file_count = sum(1 for row in tracked(db).values() if row["kind"] == "file")
                emit({{
                    "event": "storage",
                    "values": {{
                        "root_exists": "true" if root.is_dir() else "false",
                        "tracked_files": str(file_count),
                    }},
                }})
                sys.exit(0)
            if args[:2] == ["inspect", "files"]:
                with closing(connect()) as db:
                    file_count = sum(1 for row in tracked(db).values() if row["kind"] == "file")
                emit({{
                    "event": "downloaded_file_summary",
                    "values": {{
                        "ok": str(file_count),
                        "missing": "0",
                        "modified": "0",
                    }},
                }})
                sys.exit(0)
            if args[:2] == ["inspect", "verify"]:
                with closing(connect()) as db:
                    file_count = sum(1 for row in tracked(db).values() if row["kind"] == "file")
                emit({{
                    "event": "file_verification_summary",
                    "values": {{
                        "verified": str(file_count),
                        "failed": "0",
                    }},
                }})
                sys.exit(0)
            if args[:2] == ["inspect", "health"]:
                emit({{"event": "database_integrity", "values": {{"status": "healthy"}}}})
                emit({{"event": "database_integrity_passed", "level": "info", "message": "healthy"}})
                sys.exit(0)
            if args[:2] == ["inspect", "blocked"]:
                with closing(connect()) as db:
                    rows = db.execute("SELECT remote_path, reason_code FROM blocked_item").fetchall()
                for remote_path, reason_code in rows:
                    emit({{"event": "blocked_item", "values": {{"remote_path": remote_path, "reason_code": reason_code}}}})
                emit({{"event": "blocked_item_summary", "values": {{"recorded": str(len(rows)), "shown": str(len(rows))}}}})
                sys.exit(0)
            if args[:2] == ["inspect", "partials"]:
                with closing(connect()) as db:
                    rows = db.execute("SELECT remote_path FROM partial_download").fetchall()
                for (remote_path,) in rows:
                    emit({{"event": "partial_download", "values": {{"remote_path": remote_path, "status": "resumable"}}}})
                emit({{"event": "partial_download_summary", "values": {{"recorded": str(len(rows)), "resumable": str(len(rows))}}}})
                sys.exit(0)
            if args[:2] == ["state", "reset-cursor"]:
                emit({{"event": "state_cursor_reset", "level": "info", "message": "reset"}})
                sys.exit(0)
            if args[:2] == ["transfer", "sync"]:
                sys.exit(sync(dry_run="--dry-run" in args))
            if args[:2] == ["transfer", "download"]:
                dry_run = "--dry-run" in args
                remote_target = args[2]
                target = root / remote_target
                if dry_run:
                    emit({{"event": "single_download_plan", "values": {{"remote_path": remote_target, "local_path": str(target)}}}})
                    sys.exit(0)
                with cloud_lock():
                    raw = (cloud_files / remote_target).read_bytes()
                    meta = load_cloud()
                    info = dict(meta["items"][remote_target])
                with closing(connect()) as db:
                    if target.is_file() and target.read_bytes() == raw:
                        emit({{"event": "single_download_reused", "level": "info", "message": "reused"}})
                        sys.exit(0)
                    atomic_write_bytes(target, raw)
                    record(db, remote_target, info)
                    db.commit()
                sys.exit(0)
            if args[:2] == ["transfer", "restore"]:
                for holder in (busy_file, pid_file):
                    if holder.exists():
                        pid = holder.read_text(encoding="utf-8").strip()
                        emit({{"event": "restore_busy", "level": "error", "reason_code": "restore_busy", "message": f"held by PID {{pid}}"}})
                        sys.exit(1)
                dry_run = "--dry-run" in args
                target_prefix = args[2]
                with closing(connect()) as db:
                    rows = [row for path, row in sorted(tracked(db).items()) if path == target_prefix or path.startswith(target_prefix + "/")]
                    files = dirs = 0
                    for row in rows:
                        local = Path(row["local_path"])
                        if local.exists():
                            continue
                        if row["kind"] == "directory":
                            dirs += 1
                            if not dry_run:
                                local.mkdir(parents=True, exist_ok=True)
                        else:
                            files += 1
                            if not dry_run:
                                with cloud_lock():
                                    raw = (cloud_files / row["remote_path"]).read_bytes()
                                atomic_write_bytes(local, raw)
                emit({{"event": "restore_plan" if dry_run else "restore_result", "values": {{"files": str(files), "directories": str(dirs)}}}})
                sys.exit(0)
            if args[:2] == ["transfer", "watch"]:
                stop_file.unlink(missing_ok=True)
                def stop(_sig, _frame):
                    pid_file.unlink(missing_ok=True)
                    sys.exit(0)
                signal.signal(signal.SIGTERM, stop)
                while not stop_file.exists():
                    sync()
                    if not pid_file.exists():
                        atomic_write_text(pid_file, str(os.getpid()))
                    time.sleep(0.05)
                pid_file.unlink(missing_ok=True)
                stop_file.unlink(missing_ok=True)
                sys.exit(0)
            if args[:2] == ["monitor", "status"]:
                if not pid_file.exists():
                    sys.exit(1)
                emit({{"event": "monitor_control", "values": {{"pid": pid_file.read_text(encoding="utf-8").strip(), "completed_rounds": "1", "last_result": "0"}}}})
                sys.exit(0)
            if args[:2] == ["monitor", "trigger"]:
                if not pid_file.exists():
                    sys.exit(1)
                emit({{"event": "monitor_control", "values": {{"request": "monitor trigger", "acknowledgement": "queued"}}}})
                sys.exit(0)
            if args[:2] == ["monitor", "stop"]:
                atomic_write_text(stop_file, "stop")
                emit({{"event": "monitor_control", "values": {{"stopped": "true"}}}})
                sys.exit(0)
            sys.exit(2)
            '''
        ),
        encoding="utf-8",
    )
    path.chmod(0o700)


class SharePointE2ERunnerTests(unittest.TestCase):
    def test_requires_allow_remote_writes_before_touching_paths(self):
        with self.assertRaises(SystemExit) as raised, redirect_stderr(io.StringIO()):
            sharepoint_e2e.main(["--config", "/nonexistent/config.toml"])
        self.assertEqual(raised.exception.code, 2)

    def test_select_drive_distinguishes_sharepoint_library_from_onedrive(self):
        doc = {"sync": {"drive_ids": ["me", "b!sp-doclib-1"]}}
        self.assertEqual(sharepoint_e2e.select_drive(doc, None), ["b!sp-doclib-1"])

        doc_multi = {"sync": {"drive_ids": ["me", "b!biz-drive", "b!sp-doclib-1"]}}
        self.assertEqual(
            sharepoint_e2e.select_drive(
                doc_multi,
                None,
                known_types={"me": "business", "b!biz-drive": "business", "b!sp-doclib-1": "documentLibrary"},
            ),
            ["b!sp-doclib-1"],
        )
        self.assertEqual(
            sharepoint_e2e.select_drive(doc_multi, None, known_types={}),
            ["b!biz-drive", "b!sp-doclib-1"],
        )
        with self.assertRaisesRegex(sharepoint_e2e.E2EError, "Multiple documentLibrary Drives"):
            sharepoint_e2e.select_drive(
                {"sync": {"drive_ids": ["b!sp-1", "b!sp-2"]}},
                None,
                known_types={"b!sp-1": "documentLibrary", "b!sp-2": "documentLibrary"},
            )
        with self.assertRaisesRegex(sharepoint_e2e.E2EError, "No SharePoint documentLibrary"):
            sharepoint_e2e.select_drive(
                {"sync": {"drive_ids": ["me", "b!biz-drive"]}},
                None,
                known_types={"me": "business", "b!biz-drive": "business"},
            )

    def test_saved_drive_types_reads_persisted_drive_json(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _, state, _ = write_source_fixture(
                root,
                drives=["me", "b!sp-doclib-1"],
                saved_types={"me": "business", "b!sp-doclib-1": "documentLibrary"},
            )
            types = sharepoint_e2e.saved_drive_types(state, "user--0123456789abcdef")
            self.assertEqual(
                types,
                {"me": "business", "b!sp-doclib-1": "documentLibrary"},
            )

    def test_refuses_locked_source_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config, state, _ = write_source_fixture(root)
            fake = root / "fake-cli"
            write_fake_cli(fake, root / "cloud")
            with (state / "onedrive-cpp.lock").open("rb") as lock_handle:
                fcntl.flock(lock_handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                stderr = io.StringIO()
                with redirect_stderr(stderr):
                    code = sharepoint_e2e.main(
                        ["--allow-remote-writes", "--client", str(fake), "--config", str(config)]
                    )
            self.assertEqual(code, 1)
            self.assertIn("Source account is busy", stderr.getvalue())

    def test_refuses_non_document_library_drive_type(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config, _, _ = write_source_fixture(root, drives=["b!biz-drive"])
            fake = root / "fake-cli"
            write_fake_cli(fake, root / "cloud", drive_types={"b!biz-drive": "business"})
            stderr = io.StringIO()
            with redirect_stdout(io.StringIO()), redirect_stderr(stderr):
                code = sharepoint_e2e.main(
                    ["--allow-remote-writes", "--client", str(fake), "--config", str(config)]
                )
            self.assertEqual(code, 1)
            self.assertIn("driveType=documentLibrary", stderr.getvalue())

    def test_all_scenarios_and_cleanup_succeed_with_multi_drive_probe(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config, state, data = write_source_fixture(
                root,
                drives=["me", "b!biz-drive", "b!sp-doclib-1"],
            )
            cloud = root / "cloud"
            fake = root / "fake-cli"
            write_fake_cli(fake, cloud)
            stdout = io.StringIO()
            stderr = io.StringIO()
            stop_reader = threading.Event()
            decode_errors: list[Exception] = []
            cloud_meta = cloud / "meta.json"

            def reader() -> None:
                while not stop_reader.is_set():
                    try:
                        if cloud_meta.exists():
                            json.loads(cloud_meta.read_text(encoding="utf-8"))
                    except FileNotFoundError:
                        pass
                    except Exception as error:  # pragma: no cover - failure path
                        decode_errors.append(error)
                        break

            reader_thread = threading.Thread(target=reader, daemon=True)
            reader_thread.start()
            try:
                with redirect_stdout(stdout), redirect_stderr(stderr):
                    code = sharepoint_e2e.main(
                        [
                            "--allow-remote-writes",
                            "--client",
                            str(fake),
                            "--config",
                            str(config),
                            "--timeout",
                            "15",
                        ]
                    )
            finally:
                stop_reader.set()
                reader_thread.join(timeout=5)
            self.assertEqual(decode_errors, [])
            self.assertEqual(code, 0, stderr.getvalue())
            self.assertIn("PASS: 12 SharePoint documentLibrary scenarios", stdout.getvalue())
            meta = json.loads((cloud / "meta.json").read_text(encoding="utf-8"))
            self.assertEqual(meta["items"], {})
            self.assertEqual(
                (data / "keep-local.txt").read_text(encoding="utf-8"),
                "source data must remain untouched\n",
            )
            self.assertFalse((state / "accounts" / "user--0123456789abcdef" / "drives").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--client", type=Path)
    known, remaining = parser.parse_known_args()
    REAL_CLIENT = known.client.resolve(strict=True) if known.client else None
    unittest.main(argv=[ __file__, *remaining ])
