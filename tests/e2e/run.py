from __future__ import annotations

from collections.abc import Callable
import os
from pathlib import Path
import shutil
import signal
import subprocess

from .base import E2EError
from .util import client_environment, wait_until


def sync_arguments(config: Path, log_file: Path) -> list[str]:
    return [
        "sync",
        "--config",
        str(config),
        "--color",
        "never",
        "--output",
        "json",
        "--log-level",
        "trace",
        "--log-file",
        str(log_file),
    ]


def monitor_arguments(config: Path, log_file: Path) -> list[str]:
    arguments = sync_arguments(config, log_file)
    arguments[0] = "monitor"
    return arguments

def run_client(
    client: Path,
    arguments: list[str],
    home: Path,
    timeout: int,
    environment_overrides: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(client), *arguments],
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
        env=client_environment(home, environment_overrides),
    )


def run_sync(
    client: Path,
    config: Path,
    home: Path,
    log_file: Path,
    environment_overrides: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    return run_client(
        client,
        sync_arguments(config, log_file),
        home,
        timeout=600,
        environment_overrides=environment_overrides,
    )


def run_sync_after_stop(
    client: Path,
    config: Path,
    home: Path,
    log_file: Path,
    prepare: Callable[[int], None],
) -> subprocess.CompletedProcess[str]:
    arguments = sync_arguments(config, log_file)
    command = [
        "/bin/sh",
        "-c",
        'kill -STOP "$$"; exec "$@"',
        "onedrive-e2e-gate",
        str(client),
        *arguments,
    ]
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=client_environment(home),
    )
    try:
        status_path = Path("/proc") / str(process.pid) / "status"

        def stopped() -> bool:
            try:
                status = status_path.read_text(encoding="utf-8")
            except FileNotFoundError:
                return False
            return any(
                line.startswith("State:") and "T" in line
                for line in status.splitlines()
            )

        if not wait_until(stopped, 10):
            raise E2EError("gated client did not stop before execution")
        prepare(process.pid)
        os.kill(process.pid, signal.SIGCONT)
        stdout, stderr = process.communicate(timeout=600)
    except BaseException:
        process.kill()
        process.communicate()
        raise
    return subprocess.CompletedProcess(
        [str(client), *arguments],
        process.returncode,
        stdout,
        stderr,
    )


def run_sync_until_upload_progress_then_kill(
    client: Path,
    config: Path,
    home: Path,
    log_file: Path,
    state_directory: Path,
    remote_path: Path,
) -> tuple[subprocess.CompletedProcess[str], tuple[str, int, int, str]]:
    arguments = sync_arguments(config, log_file)
    process = subprocess.Popen(
        [str(client), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=client_environment(home),
    )
    progress: tuple[str, int, int, str] | None = None

    def upload_progressed() -> bool:
        nonlocal progress
        if process.poll() is not None:
            return False
        row = pending_upload_row(state_directory, remote_path)
        if row is not None and row[2] > 0:
            progress = row
            return True
        return False

    try:
        if not wait_until(upload_progressed, 180, 0.05):
            returncode = process.poll()
            raise E2EError(
                "upload did not reach a resumable checkpoint before "
                f"client exit ({returncode})"
            )
        process.kill()
        stdout, stderr = process.communicate(timeout=30)
    except BaseException:
        if process.poll() is None:
            process.kill()
        process.communicate()
        raise
    if process.returncode != -signal.SIGKILL or progress is None:
        raise E2EError("upload crash injection did not kill the client")
    return (
        subprocess.CompletedProcess(
            [str(client), *arguments],
            process.returncode,
            stdout,
            stderr,
        ),
        progress,
    )


def run_monitor_boundary(
    client: Path,
    config: Path,
    home: Path,
    log_file: Path,
    workspace: Path,
    exercise: Callable[[subprocess.Popen[str], Path], None],
) -> subprocess.CompletedProcess[str]:
    arguments = monitor_arguments(config, log_file)
    stdout_path = workspace / "monitor.stdout.log"
    stderr_path = workspace / "monitor.stderr.log"
    stdbuf = shutil.which("stdbuf")
    if stdbuf is None:
        raise E2EError("monitor boundary E2E requires the stdbuf command")
    command = [stdbuf, "-oL", "-eL", str(client), *arguments]
    with stdout_path.open("w", encoding="utf-8") as stdout_stream, \
         stderr_path.open("w", encoding="utf-8") as stderr_stream:
        process = subprocess.Popen(
            command,
            stdout=stdout_stream,
            stderr=stderr_stream,
            text=True,
            env=client_environment(home),
        )
        try:
            def monitor_ready() -> bool:
                if process.poll() is not None:
                    return False
                try:
                    output = stdout_path.read_text(encoding="utf-8")
                    return (
                        "monitor_ready" in output
                        and "sync_completed" in output
                    )
                except FileNotFoundError:
                    return False

            if not wait_until(monitor_ready, 60, 0.1):
                raise E2EError(
                    f"monitor did not become ready ({process.poll()})"
                )
            exercise(process, stdout_path)
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=30)
        except BaseException:
            if process.poll() is None:
                process.kill()
            process.wait()
            raise
    stdout = stdout_path.read_text(encoding="utf-8")
    stderr = stderr_path.read_text(encoding="utf-8")
    if process.returncode != 0:
        raise E2EError(
            f"monitor did not terminate cleanly ({process.returncode})"
        )
    return subprocess.CompletedProcess(
        [str(client), *arguments],
        process.returncode,
        stdout,
        stderr,
    )


def run_single_download(
    client: Path,
    config: Path,
    remote_path: Path,
    home: Path,
    log_file: Path,
) -> subprocess.CompletedProcess[str]:
    return run_client(
        client,
        [
            "download",
            remote_path.as_posix(),
            "--config",
            str(config),
            "--color",
            "never",
            "--output",
            "json",
            "--log-level",
            "trace",
            "--log-file",
            str(log_file),
        ],
        home,
        timeout=600,
    )

def reset_copied_state(
    client: Path,
    config: Path,
    home: Path,
) -> subprocess.CompletedProcess[str]:
    return run_client(
        client,
        [
            "reset-state",
            "--config",
            str(config),
            "--clear-all",
            "--yes",
            "--color",
            "never",
            "--output",
            "json",
        ],
        home,
        timeout=60,
    )


def reset_delta_cursor(
    client: Path,
    config: Path,
    home: Path,
) -> subprocess.CompletedProcess[str]:
    return run_client(
        client,
        [
            "reset-state",
            "--config",
            str(config),
            "--color",
            "never",
            "--output",
            "json",
        ],
        home,
        timeout=60,
    )
