#!/usr/bin/env python3

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


SUITES = {"24.04": "noble", "26.04": "resolute"}


def download_releases(repository, destination):
    pages = json.loads(subprocess.check_output(
        ["gh", "api", "--paginate", "--slurp", f"repos/{repository}/releases"],
        text=True,
    ))
    count = 0
    for page in pages:
        for release in page:
            if release["draft"] or release["prerelease"]:
                continue
            directory = destination / str(release["id"])
            directory.mkdir(parents=True)
            subprocess.run([
                "gh", "release", "download", release["tag_name"],
                "--repo", repository, "--dir", str(directory),
                "--pattern", "onedrive-cpp_*.deb", "--pattern", "SHA256SUMS",
            ], check=True)
            count += 1
    if not count:
        raise ValueError("No published stable releases found")


def verified_packages(directory):
    packages = sorted(directory.glob("onedrive-cpp_*.deb"))
    if not packages:
        raise ValueError(f"No packages found in {directory}")
    checksums = {}
    for line in (directory / "SHA256SUMS").read_text().splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]{64}) [ *](.+)", line)
        if not match or match[2] in checksums:
            raise ValueError(f"Invalid or duplicate checksum entry in {directory}")
        checksums[match[2]] = match[1].lower()
    for package in packages:
        with package.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        if checksums.get(package.name) != digest:
            raise ValueError(f"SHA256 mismatch or missing checksum: {package}")
    return packages


def package_suite(package):
    fields = subprocess.check_output([
        "dpkg-deb", "--field", str(package), "Package", "Version", "Architecture",
    ], text=True)
    metadata = dict(line.split(": ", 1) for line in fields.splitlines())
    version = metadata["Version"]
    match = re.search(r"[.~]ubuntu(24\.04|26\.04)$", version)
    if (metadata["Package"] != "onedrive-cpp" or metadata["Architecture"] != "amd64"
            or not re.fullmatch(r"[0-9][A-Za-z0-9.+:~\-]*", version) or not match):
        raise ValueError(f"Unsupported package metadata: {package}: {metadata}")
    expected = f"onedrive-cpp_{version}_amd64.deb"
    if package.name != expected:
        raise ValueError(f"Package filename does not match metadata: {package}")
    return SUITES[match[1]]


def build_repository(releases, output, signing_key):
    if not re.fullmatch(r"(?:[0-9A-Fa-f]{40}|[0-9A-Fa-f]{64})", signing_key):
        raise ValueError("Signing key must be a full OpenPGP fingerprint")
    if output.exists():
        raise ValueError(f"Output directory must not already exist: {output}")
    output.mkdir(parents=True)
    suites = set()
    for directory in sorted(path for path in releases.iterdir() if path.is_dir()):
        for package in verified_packages(directory):
            suite = package_suite(package)
            suites.add(suite)
            destination = output / "pool/main" / suite / package.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            if destination.exists():
                if destination.read_bytes() != package.read_bytes():
                    raise ValueError(f"Conflicting package contents: {package.name}")
            else:
                shutil.copyfile(package, destination)
    if suites != set(SUITES.values()):
        raise ValueError(f"Expected packages for noble and resolute, found: {sorted(suites)}")

    public_key = subprocess.check_output(["gpg", "--batch", "--armor", "--export", signing_key])
    if not public_key:
        raise ValueError(f"Signing public key not found: {signing_key}")
    (output / "onedrive-cpp.asc").write_bytes(public_key)
    for suite in sorted(suites):
        distribution = output / "dists" / suite
        index = distribution / "main/binary-amd64"
        index.mkdir(parents=True)
        packages = subprocess.check_output(
            ["apt-ftparchive", "packages", f"pool/main/{suite}"], cwd=output,
        )
        (index / "Packages").write_bytes(packages)
        (index / "Packages.gz").write_bytes(gzip.compress(packages, mtime=0))
        by_hash = index / "by-hash/SHA256"
        by_hash.mkdir(parents=True)
        for name in ("Packages", "Packages.gz"):
            content = (index / name).read_bytes()
            (by_hash / hashlib.sha256(content).hexdigest()).write_bytes(content)
        options = {
            "Origin": "onedrive-cpp",
            "Label": "onedrive-cpp",
            "Suite": suite,
            "Codename": suite,
            "Architectures": "amd64",
            "Components": "main",
            "Description": "Stable onedrive-cpp packages for Ubuntu",
            "Acquire-By-Hash": "yes",
        }
        command = ["apt-ftparchive"]
        for name, value in options.items():
            command.extend(["-o", f"APT::FTPArchive::Release::{name}={value}"])
        command.extend(["release", f"dists/{suite}"])
        release = distribution / "Release"
        release.write_bytes(subprocess.check_output(command, cwd=output))
        for filename, mode in (("InRelease", "--clearsign"), ("Release.gpg", "--detach-sign")):
            subprocess.run([
                "gpg", "--batch", "--yes", "--pinentry-mode", "loopback",
                "--passphrase-fd", "0", "--local-user", signing_key,
                "--digest-algo", "SHA256", "--armor",
                "--output", str(distribution / filename), mode, str(release),
            ], input=os.environ.get("APT_SIGNING_PASSPHRASE", "").encode() + b"\n", check=True)
    (output / "index.html").write_text(
        "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\">"
        "<title>onedrive-cpp APT repository</title>"
        "<h1>onedrive-cpp APT repository</h1>"
        "<p>Ubuntu 24.04 (noble): CLI/TUI; Ubuntu 26.04 (resolute): CLI/TUI/GUI. "
        "amd64 only.</p><p><a href=\"onedrive-cpp.asc\">Repository signing key</a></p>"
        f"<p>Signing key fingerprint: <code>{signing_key.upper()}</code></p>"
        "<p><a href=\"https://github.com/cloudfyy/onedrivecpp#installation\">"
        "Installation instructions</a></p></html>\n"
    )
    (output / ".nojekyll").touch()


def main():
    parser = argparse.ArgumentParser(description="Build a signed APT repository from verified releases")
    parser.add_argument("--releases-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--github-repository", help="Download all published stable GitHub releases")
    parser.add_argument("--signing-key", required=True, help="Full OpenPGP fingerprint")
    options = parser.parse_args()
    try:
        if options.github_repository:
            options.releases_dir.mkdir(parents=True)
            download_releases(options.github_repository, options.releases_dir)
        build_repository(
            options.releases_dir.resolve(), options.output_dir.resolve(), options.signing_key,
        )
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"APT repository preparation failed: {error}\n")


if __name__ == "__main__":
    main()
