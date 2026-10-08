from __future__ import annotations

from contextlib import redirect_stdout
import io

from .base import E2EError
from .progress import E2EProgress


def self_test_progress() -> None:
    output = io.StringIO()
    with redirect_stdout(output):
        progress = E2EProgress(2)
        progress.step("first stage")
        progress.step("second stage")
    if output.getvalue().splitlines() != [
        "[E2E 1/2  50%] first stage",
        "[E2E 2/2 100%] second stage",
    ]:
        raise E2EError("E2E progress output self-test failed")
    try:
        progress.step("unexpected stage")
    except RuntimeError:
        pass
    else:
        raise E2EError("E2E progress accepted too many stages")
    try:
        E2EProgress(0)
    except ValueError:
        pass
    else:
        raise E2EError("E2E progress accepted an empty plan")
