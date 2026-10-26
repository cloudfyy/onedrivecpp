from __future__ import annotations

from contextlib import contextmanager
import fcntl
import os
from pathlib import Path
import secrets
import shutil
import sqlite3
import stat
from typing import Iterator

from .base import E2EError, require, sha256


def private_read(path: Path, *, token: bool = False) -> bytes:
    for component in (path, *path.parents):
        require(
            not component.is_symlink(),
            "Credential paths must not use symlinks",
        )
    descriptor = os.open(
        path,
        os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC,
    )
    try:
        metadata = os.fstat(descriptor)
        require(
            stat.S_ISREG(metadata.st_mode) and metadata.st_uid == os.geteuid(),
            "Credentials must be regular files owned by the current user",
        )
        if token:
            require(
                stat.S_IMODE(metadata.st_mode) == 0o600,
                "Token must be mode 0600",
            )
        with os.fdopen(descriptor, "rb", closefd=False) as source:
            contents = source.read(1024 * 1024 + 1)
        require(
            0 < len(contents) <= 1024 * 1024,
            "Credential file is empty or too large",
        )
        return contents
    finally:
        os.close(descriptor)


def private_write(path: Path, contents: bytes) -> None:
    descriptor = os.open(
        path,
        os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW,
        0o600,
    )
    with os.fdopen(descriptor, "wb") as destination:
        os.fchmod(destination.fileno(), 0o600)
        destination.write(contents)


def _read_account_credentials(state: Path) -> tuple[str, bytes, bytes]:
    marker = (
        private_read(state / "active_account").decode("utf-8").rstrip("\r\n")
    )
    require(
        marker not in ("", ".", "..")
        and "/" not in marker
        and "\\" not in marker
        and "\0" not in marker,
        "Invalid active-account marker",
    )
    account = state / "accounts" / marker
    token = private_read(account / "refresh_token", token=True)
    require(bool(token.strip()), "Refresh token contains only whitespace")
    return (
        marker,
        private_read(account / "account.json"),
        token,
    )


def credentials_snapshot(
    state: Path,
    *,
    require_lock: bool = True,
) -> tuple[str, bytes, bytes]:
    lock = state / "onedrive-cpp.lock"
    if not require_lock and not lock.exists():
        return _read_account_credentials(state)
    private_read(lock)
    descriptor = os.open(lock, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    with os.fdopen(descriptor, "rb") as source_lock:
        try:
            fcntl.flock(source_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise E2EError(
                "Source account is busy; stop sync/watch before copying credentials"
            ) from error
        return _read_account_credentials(state)


def write_isolated_credentials(
    state_directory: Path,
    credentials: tuple[str, bytes, bytes],
) -> None:
    marker, account, token = credentials
    state_directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    state_directory.chmod(0o700)
    accounts = state_directory / "accounts"
    accounts.mkdir(exist_ok=True, mode=0o700)
    accounts.chmod(0o700)
    account_directory = accounts / marker
    account_directory.mkdir(exist_ok=True, mode=0o700)
    account_directory.chmod(0o700)
    private_write(state_directory / "active_account", (marker + "\n").encode())
    private_write(account_directory / "account.json", account)
    private_write(account_directory / "refresh_token", token)


@contextmanager
def hold_source_state_lock(source_state: Path) -> Iterator[None]:
    lock_path = source_state / "onedrive-cpp.lock"
    if not lock_path.exists():
        yield
        return
    private_read(lock_path)
    descriptor = os.open(lock_path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    with os.fdopen(descriptor, "rb") as lock_file:
        try:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise E2EError(
                "Source account is busy; stop sync/watch before copying credentials"
            ) from error
        try:
            yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def copy_isolated_state(source_state: Path, state_directory: Path) -> None:
    if state_directory.exists():
        raise E2EError(
            f"isolated state directory already exists: {state_directory}"
        )
    credentials = credentials_snapshot(source_state, require_lock=False)
    write_isolated_credentials(state_directory, credentials)


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
