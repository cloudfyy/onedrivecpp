import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from build_apt_repository import build_repository, download_releases, verified_packages


class AptRepositoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.home = Path(cls.temporary.name)
        cls.gnupg = cls.home / "gnupg"
        cls.gnupg.mkdir(mode=0o700)
        cls.environment = patch.dict(os.environ, {"GNUPGHOME": str(cls.gnupg)})
        cls.environment.start()
        subprocess.run([
            "gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase", "",
            "--quick-generate-key", "APT test <apt@example.invalid>", "ed25519", "sign", "1d",
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        keys = subprocess.check_output(["gpg", "--with-colons", "--list-secret-keys"], text=True)
        cls.fingerprint = next(line.split(":")[9] for line in keys.splitlines()
                               if line.startswith("fpr:"))

    @classmethod
    def tearDownClass(cls):
        subprocess.run(["gpgconf", "--kill", "gpg-agent"], check=True)
        cls.environment.stop()
        cls.temporary.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=self.home)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.releases = self.root / "releases"
        self.releases.mkdir()

    def package(self, directory, version, architecture="amd64", description="APT repository test"):
        directory.mkdir(parents=True, exist_ok=True)
        source = self.root / f"source-{directory.name}-{version}-{architecture}"
        (source / "DEBIAN").mkdir(parents=True)
        (source / "DEBIAN/control").write_text(
            f"Package: onedrive-cpp\nVersion: {version}\nArchitecture: {architecture}\n"
            f"Maintainer: Test <apt@example.invalid>\nDescription: {description}\n"
        )
        package = directory / f"onedrive-cpp_{version}_{architecture}.deb"
        subprocess.run(["dpkg-deb", "--build", "--root-owner-group", str(source), str(package)],
                       check=True, stdout=subprocess.DEVNULL)
        with (directory / "SHA256SUMS").open("a") as manifest:
            manifest.write(f"{hashlib.sha256(package.read_bytes()).hexdigest()}  {package.name}\n")
        return package

    def packages(self):
        self.package(self.releases / "old", "1.0.0-1.ubuntu24.04")
        self.package(self.releases / "current", "1.2.8-1~ubuntu24.04")
        self.package(self.releases / "current", "1.2.8-1~ubuntu26.04")

    def test_signed_repository_is_consumed_by_apt_and_preserves_old_versions(self):
        self.packages()
        output = self.root / "site"
        build_repository(self.releases, output, self.fingerprint)
        keyring = self.root / "keyring.gpg"
        subprocess.run(["gpg", "--batch", "--output", str(keyring), "--dearmor",
                        str(output / "onedrive-cpp.asc")], check=True)
        for suite, expected, other in (
            ("noble", "ubuntu24.04", "ubuntu26.04"),
            ("resolute", "ubuntu26.04", "ubuntu24.04"),
        ):
            distribution = output / "dists" / suite
            subprocess.run(["gpgv", "--keyring", str(keyring), str(distribution / "InRelease")],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            subprocess.run(["gpgv", "--keyring", str(keyring),
                            str(distribution / "Release.gpg"), str(distribution / "Release")],
                           check=True, stderr=subprocess.PIPE)
            index = distribution / "main/binary-amd64/Packages"
            content = index.read_text()
            self.assertIn(expected, content)
            self.assertNotIn(other, content)
            digest = hashlib.sha256(index.read_bytes()).hexdigest()
            self.assertEqual((index.parent / "by-hash/SHA256" / digest).read_bytes(),
                             index.read_bytes())
            lists = self.root / f"lists-{suite}"
            (lists / "partial").mkdir(parents=True)
            source = self.root / f"{suite}.list"
            source.write_text(f"deb [arch=amd64 signed-by={keyring}] file:{output} {suite} main\n")
            options = [
                "-o", f"Dir::Etc::sourcelist={source}",
                "-o", "Dir::Etc::sourceparts=-",
                "-o", f"Dir::State::lists={lists}",
                "-o", f"Dir::State::status={self.root}/status",
                "-o", "APT::Architecture=amd64",
                "-o", f"APT::Sandbox::User={os.getuid()}",
                "-o", "APT::Get::List-Cleanup=0",
            ]
            subprocess.run(["apt-get", *options, "--error-on=any", "update"],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            policy = subprocess.check_output(["apt-cache", *options, "policy", "onedrive-cpp"],
                                             text=True)
            self.assertIn(f"Candidate: 1.2.8-1~{expected}", policy)
            if suite == "noble":
                self.assertIn("1.0.0-1.ubuntu24.04", policy)

        inrelease = output / "dists/resolute/InRelease"
        inrelease.write_bytes(inrelease.read_bytes().replace(
            b"Stable onedrive-cpp packages", b"Tampered onedrive-cpp packages",
        ))
        untrusted_lists = self.root / "untrusted-lists"
        (untrusted_lists / "partial").mkdir(parents=True)
        options[options.index(f"Dir::State::lists={lists}")] = (
            f"Dir::State::lists={untrusted_lists}"
        )
        rejected = subprocess.run(
            ["apt-get", *options, "--error-on=any", "update"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn(b"BADSIG", rejected.stderr)

    def test_tampered_package_and_missing_checksum_are_rejected(self):
        directory = self.releases / "release"
        package = self.package(directory, "1.2.8-1~ubuntu24.04")
        package.write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
            verified_packages(directory)
        (directory / "SHA256SUMS").write_text("")
        with self.assertRaisesRegex(ValueError, "missing checksum"):
            verified_packages(directory)

    def test_package_metadata_not_filename_determines_suite(self):
        self.packages()
        package = next((self.releases / "current").glob("*ubuntu26.04*.deb"))
        renamed = package.with_name(package.name.replace("ubuntu26.04", "ubuntu24.04-fake"))
        package.rename(renamed)
        manifest = renamed.parent / "SHA256SUMS"
        manifest.write_text(manifest.read_text().replace(package.name, renamed.name))
        with self.assertRaisesRegex(ValueError, "filename does not match metadata"):
            build_repository(self.releases, self.root / "site", self.fingerprint)

    def test_missing_suite_and_unsupported_architecture_fail(self):
        self.package(self.releases / "release", "1.2.8-1~ubuntu24.04")
        with self.assertRaisesRegex(ValueError, "Expected packages for noble and resolute"):
            build_repository(self.releases, self.root / "missing", self.fingerprint)
        self.package(self.releases / "release", "1.2.8-1~ubuntu26.04", "arm64")
        with self.assertRaisesRegex(ValueError, "Unsupported package metadata"):
            build_repository(self.releases, self.root / "unsupported", self.fingerprint)

    def test_conflicting_versions_fail_instead_of_overwriting(self):
        self.packages()
        self.package(
            self.releases / "conflicting", "1.2.8-1~ubuntu24.04",
            description="Different contents for the same version",
        )
        with self.assertRaisesRegex(ValueError, "Conflicting package contents"):
            build_repository(self.releases, self.root / "site", self.fingerprint)

    @patch("build_apt_repository.subprocess.run")
    @patch("build_apt_repository.subprocess.check_output")
    def test_downloader_excludes_drafts_and_prereleases(self, query, run):
        query.return_value = json.dumps([[
            {"id": 1, "tag_name": "v1", "draft": False, "prerelease": False},
            {"id": 2, "tag_name": "v2", "draft": True, "prerelease": False},
            {"id": 3, "tag_name": "v3", "draft": False, "prerelease": True},
        ]])
        download_releases("owner/repo", self.releases)
        run.assert_called_once()
        self.assertIn("v1", run.call_args.args[0])
        query.return_value = "[[]]"
        with self.assertRaisesRegex(ValueError, "No published stable releases"):
            download_releases("owner/repo", self.releases)


if __name__ == "__main__":
    unittest.main()
