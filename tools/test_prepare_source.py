import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

from prepare_source import dependency_archive, prepare_source


class PrepareSourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.content = b"verified upstream archive"
        self.dependency = {
            "version": "1.0.0",
            "archive": "dependency-1.0.0.tar.gz",
            "url": "https://example.invalid/dependency.tar.gz",
            "sha256": hashlib.sha256(self.content).hexdigest(),
        }
        self.cache = self.root / "cache"
        self.cache.mkdir()

    def cached_archive(self):
        archive = self.cache / self.dependency["archive"]
        archive.write_bytes(self.content)
        return archive

    @patch("prepare_source.urlopen")
    def test_offline_cache_never_uses_network(self, request):
        expected = self.cached_archive()
        self.assertEqual(dependency_archive(self.dependency, self.cache, True), expected)
        request.assert_not_called()

    @patch("prepare_source.urlopen")
    def test_offline_missing_archive_fails_without_network(self, request):
        with self.assertRaisesRegex(FileNotFoundError, "Missing dependency archive"):
            dependency_archive(self.dependency, self.cache, True)
        request.assert_not_called()

    @patch("prepare_source.urlopen")
    def test_corrupted_cache_fails_without_download_fallback(self, request):
        self.cached_archive().write_bytes(b"corrupted")
        with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
            dependency_archive(self.dependency, self.cache, False)
        request.assert_not_called()

    @patch("prepare_source.urlopen")
    def test_download_is_verified_before_caching(self, request):
        request.return_value = io.BytesIO(self.content)
        archive = dependency_archive(self.dependency, self.cache, False)
        self.assertEqual(archive.read_bytes(), self.content)
        request.assert_called_once_with(self.dependency["url"], timeout=60)

    @patch("prepare_source.urlopen")
    def test_corrupted_download_is_not_cached(self, request):
        request.return_value = io.BytesIO(b"incorrect archive")
        with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
            dependency_archive(self.dependency, self.cache, False)
        self.assertEqual(list(self.cache.iterdir()), [])

    def source_tree(self):
        source = self.root / "source"
        (source / "packaging").mkdir(parents=True)
        (source / "tools").mkdir()
        (source / "packaging/dependencies.json").write_text(
            json.dumps({"dependency": self.dependency})
        )
        (source / "vcpkg.json").write_text('{"version-semver": "1.0.1"}')
        (source / "tools/prepare_source.py").write_text("tracked tool\n")
        (source / "CMakeLists.txt").write_text("tracked source\n")
        subprocess.run(["git", "init", "--quiet", str(source)], check=True)
        subprocess.run(["git", "-C", str(source), "add", "."], check=True)
        self.cached_archive()
        return source

    @patch.dict(os.environ, {"SOURCE_DATE_EPOCH": "1234567890"})
    def test_source_bundle_contains_only_tracked_files_and_verified_archives(self):
        source = self.source_tree()
        (source / "untracked.txt").write_text("must not be distributed\n")
        destination = prepare_source(source, self.root / "output", self.cache, True)
        with tarfile.open(destination) as archive:
            names = set(archive.getnames())
            prefix = "onedrive-cpp-1.0.1/"
            self.assertIn(prefix + "CMakeLists.txt", names)
            self.assertNotIn(prefix + "untracked.txt", names)
            self.assertNotIn(prefix + ".git", names)
            bundled = archive.extractfile(prefix + "third_party/" + self.dependency["archive"])
            self.assertEqual(bundled.read(), self.content)
            for member in archive:
                self.assertEqual((member.uid, member.gid, member.mtime), (0, 0, 1234567890))

    @patch.dict(os.environ, {"SOURCE_DATE_EPOCH": "1234567890"})
    def test_repeated_preparation_is_byte_identical(self):
        source = self.source_tree()
        first = prepare_source(source, self.root / "first", self.cache, True)
        (source / "CMakeLists.txt").touch()
        second = prepare_source(source, self.root / "second", self.cache, True)
        self.assertEqual(first.read_bytes(), second.read_bytes())

    @patch.dict(os.environ, {"SOURCE_DATE_EPOCH": "1234567890"})
    def test_deleted_tracked_file_is_an_explicit_error(self):
        source = self.source_tree()
        (source / "CMakeLists.txt").unlink()
        with self.assertRaises(FileNotFoundError):
            prepare_source(source, self.root / "output", self.cache, True)
        self.assertEqual(list((self.root / "output").iterdir()), [])


if __name__ == "__main__":
    unittest.main()
