from __future__ import annotations

import os
from pathlib import Path
import secrets
import shutil
import sqlite3

from .base import E2EError, sha256


def tracked_item_count(state_directory: Path, remote_path: Path) -> int:
    databases = list(state_directory.rglob("items.sqlite3"))
    if not databases:
        raise E2EError("isolated state contains no item database")
    count = 0
    for database in databases:
        try:
            with sqlite3.connect(database) as connection:
                row = connection.execute(
                    "SELECT count(*) FROM item WHERE remote_path = ?",
                    (remote_path.as_posix(),),
                ).fetchone()
        except sqlite3.Error as error:
            raise E2EError(f"cannot query item database: {database}") from error
        if row is None:
            raise E2EError(f"cannot query item database: {database}")
        count += int(row[0])
    return count


def tracked_drive_id(state_directory: Path, remote_path: Path) -> str:
    drive_ids: set[str] = set()
    for database in state_directory.rglob("items.sqlite3"):
        try:
            with sqlite3.connect(database) as connection:
                rows = connection.execute(
                    "SELECT DISTINCT drive_id FROM item WHERE remote_path = ?",
                    (remote_path.as_posix(),),
                ).fetchall()
        except sqlite3.Error as error:
            raise E2EError(f"cannot query item database: {database}") from error
        drive_ids.update(str(row[0]) for row in rows)
    if len(drive_ids) != 1:
        raise E2EError(
            "move fixture requires exactly one tracked Graph Drive"
        )
    return next(iter(drive_ids))


def inject_completed_upload(
    state_directory: Path,
    local_path: Path,
    remote_path: Path,
) -> Path:
    databases = list(state_directory.rglob("items.sqlite3"))
    matches: list[tuple[Path, tuple[object, ...]]] = []
    for database in databases:
        try:
            with sqlite3.connect(database) as connection:
                row = connection.execute(
                    "SELECT drive_id, remote_id, local_size, "
                    "local_modified_ticks FROM item WHERE remote_path = ?",
                    (remote_path.as_posix(),),
                ).fetchone()
        except sqlite3.Error as error:
            raise E2EError(f"cannot query item database: {database}") from error
        if row is not None:
            matches.append((database, row))
    if len(matches) != 1:
        raise E2EError(
            "upload recovery injection requires exactly one tracked item"
        )
    database, row = matches[0]
    drive_id, remote_id, local_size, local_modified_ticks = row
    snapshot = local_path.with_name(
        f".{local_path.name}.onedrive-upload-e2e-{secrets.token_hex(8)}"
    )
    shutil.copyfile(local_path, snapshot)
    try:
        with sqlite3.connect(database) as connection:
            connection.execute("BEGIN IMMEDIATE")
            connection.execute(
                "DELETE FROM item WHERE drive_id = ? AND remote_id = ?",
                (drive_id, remote_id),
            )
            connection.execute(
                "INSERT INTO pending_upload ("
                "drive_id, remote_path, local_path, snapshot_path, "
                "content_fingerprint, local_size, local_modified_ticks, "
                "remote_id, expected_etag, upload_url, upload_expiration, "
                "completed_bytes, failure_code, failure_message, "
                "failure_attempt_count, directory"
                ") VALUES (?, ?, ?, ?, ?, ?, ?, '', '', '', '', 0, '', '', 0, 0)",
                (
                    drive_id,
                    remote_path.as_posix(),
                    str(local_path),
                    str(snapshot),
                    sha256(snapshot),
                    local_size,
                    local_modified_ticks,
                ),
            )
            connection.commit()
    except sqlite3.Error as error:
        try:
            snapshot.unlink()
        except FileNotFoundError:
            pass
        raise E2EError("cannot inject completed upload recovery") from error
    return snapshot


def active_refresh_token_path(state_directory: Path) -> Path:
    marker = state_directory / "active_account"
    try:
        account = marker.read_text(encoding="utf-8").strip()
    except OSError as error:
        raise E2EError("cannot read isolated active-account marker") from error
    if not account or Path(account).name != account or account in {".", ".."}:
        raise E2EError("isolated active-account marker is invalid")
    token = state_directory / "accounts" / account / "refresh_token"
    if token.is_symlink() or not token.is_file():
        raise E2EError("isolated refresh token is not a regular file")
    return token


def replace_private_file(path: Path, contents: str) -> None:
    temporary = path.with_name(
        f".{path.name}.graph-e2e-{secrets.token_hex(8)}"
    )
    descriptor = -1
    try:
        descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL,
            0o600,
        )
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            descriptor = -1
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if descriptor != -1:
            os.close(descriptor)
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass

def pending_upload_row(
    state_directory: Path,
    remote_path: Path,
) -> tuple[str, int, int, str] | None:
    rows: list[tuple[str, int, int, str]] = []
    for database in state_directory.rglob("items.sqlite3"):
        try:
            with sqlite3.connect(database, timeout=0.1) as connection:
                row = connection.execute(
                    "SELECT failure_code, failure_attempt_count, "
                    "completed_bytes, snapshot_path FROM pending_upload "
                    "WHERE remote_path = ?",
                    (remote_path.as_posix(),),
                ).fetchone()
        except sqlite3.OperationalError as error:
            if "locked" in str(error).lower():
                return None
            raise E2EError(
                f"cannot query pending upload database: {database}"
            ) from error
        if row is not None:
            rows.append(
                (str(row[0]), int(row[1]), int(row[2]), str(row[3]))
            )
    if len(rows) > 1:
        raise E2EError(
            f"multiple pending uploads found for '{remote_path.as_posix()}'"
        )
    return rows[0] if rows else None
