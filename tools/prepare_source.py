#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from urllib.request import urlopen


def verify_archive(path, dependency):
    with path.open("rb") as source:
        actual = hashlib.file_digest(source, "sha256").hexdigest()
    if actual != dependency["sha256"]:
        raise ValueError(f"SHA256 mismatch: {path}")


def dependency_archive(dependency, directory, offline):
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / dependency["archive"]
    if archive.exists():
        verify_archive(archive, dependency)
        return archive
    if offline:
        raise FileNotFoundError(f"Missing dependency archive: {archive}")
    with tempfile.TemporaryDirectory(dir=directory) as temporary:
        downloaded = Path(temporary) / dependency["archive"]
        with urlopen(dependency["url"], timeout=60) as response, downloaded.open("wb") as output:
            shutil.copyfileobj(response, output)
        verify_archive(downloaded, dependency)
        downloaded.replace(archive)
    return archive


def prepare_source(root, output_directory, archives_directory, offline=False):
    manifest = json.loads((root / "packaging/dependencies.json").read_text())
    version = json.loads((root / "vcpkg.json").read_text())["version-semver"]
    tracked = subprocess.check_output(["git", "-C", str(root), "ls-files", "-z"])
    paths = sorted(os.fsdecode(path) for path in tracked.split(b"\0") if path)
    required = {"tools/prepare_source.py", "packaging/dependencies.json"}
    if not required.issubset(paths):
        raise ValueError("Release preparation files must be tracked by Git; stage them first")
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch is None:
        epoch = subprocess.check_output(
            ["git", "-C", str(root), "log", "-1", "--format=%ct"], text=True
        ).strip()
    timestamp = int(epoch)
    dependencies = [
        dependency_archive(dependency, archives_directory, offline)
        for dependency in manifest.values()
    ]
    output_directory.mkdir(parents=True, exist_ok=True)
    prefix = f"onedrive-cpp-{version}"
    destination = output_directory / f"{prefix}.tar.xz"

    def normalize(info):
        info.uid = info.gid = 0
        info.uname = info.gname = "root"
        info.mtime = timestamp
        info.mode = 0o755 if info.mode & 0o111 else 0o644
        info.pax_headers = {}
        return info

    with tempfile.TemporaryDirectory(dir=output_directory) as temporary:
        prepared = Path(temporary) / destination.name
        with tarfile.open(prepared, "w:xz", format=tarfile.PAX_FORMAT) as archive:
            for path in paths:
                archive.add(root / path, arcname=f"{prefix}/{path}", recursive=False, filter=normalize)
            for path in dependencies:
                archive.add(
                    path, arcname=f"{prefix}/third_party/{path.name}", filter=normalize
                )
        prepared.replace(destination)
    return destination


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Prepare release source with verified offline dependencies")
    parser.add_argument("--output-dir", type=Path, default=root / "build/source")
    parser.add_argument("--archives-dir", type=Path, default=root / "build/dependency-archives")
    parser.add_argument("--offline", action="store_true", help="Require all archives to be cached")
    options = parser.parse_args()
    try:
        destination = prepare_source(root, options.output_dir, options.archives_dir, options.offline)
    except (OSError, ValueError, subprocess.CalledProcessError, tarfile.TarError) as error:
        parser.exit(1, f"Source preparation failed: {error}\n")
    print(destination)


if __name__ == "__main__":
    main()
