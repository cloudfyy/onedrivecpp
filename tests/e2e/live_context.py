from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import subprocess

from .progress import E2EProgress


@dataclass
class LiveScenarioContext:
    client: Path
    workspace: Path
    sync_data_directory: Path
    state_directory: Path
    home: Path
    config: Path
    sync_list: Path
    log_file: Path
    source_text: str
    expected_path: Path
    stable: Path
    graph_endpoint: str
    access_token: str
    completed: list[subprocess.CompletedProcess[str]]
    progress: E2EProgress
