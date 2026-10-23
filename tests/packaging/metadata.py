import argparse
import gzip
from email.parser import Parser
from email.utils import parseaddr
from pathlib import Path, PurePosixPath
import subprocess
import tarfile


def check_metadata(source, package, doc_dir, dpkg_deb):
    control = (source / "debian/control").read_text()
    source_fields, binary_fields = [Parser().parsestr(block) for block in control.strip().split("\n\n")]
    homepage = "https://github.com/cloudfyy/onedrivecpp"
    assert source_fields["Homepage"] == homepage, "Incorrect project homepage"
    assert source_fields["Vcs-Git"] == f"{homepage}.git", "Incorrect VCS URL"
    assert source_fields["Vcs-Browser"] == homepage, "Incorrect VCS browser URL"
    maintainer, address = parseaddr(source_fields["Maintainer"])
    assert maintainer and "@" in address, "Maintainer must have a name and email address"
    assert not address.endswith(".invalid"), "Placeholder maintainer"
    copyright_fields = Parser().parsestr((source / "debian/copyright").read_text().split("\n\n")[0])
    assert copyright_fields["Source"] == homepage, "Incorrect upstream copyright source"
    assert "scaffold" not in binary_fields["Description"].lower(), "Outdated package description"
    for field, expected in (
        ("Maintainer", source_fields["Maintainer"]),
        ("Homepage", homepage),
        ("Description", binary_fields["Description"]),
        ("Section", source_fields["Section"]),
    ):
        actual = subprocess.check_output([dpkg_deb, "--field", str(package), field], text=True)
        assert " ".join(actual.split()) == " ".join(expected.split()), f"Incorrect DEB {field}: {actual}"

    expected_files = {
        PurePosixPath(doc_dir.lstrip("/"), "LICENSE"): (source / "LICENSE").read_bytes(),
        PurePosixPath(doc_dir.lstrip("/"), "copyright"): (source / "debian/copyright").read_bytes(),
    }
    assert (
        b"Version 3, 29 June 2007" in expected_files[PurePosixPath(doc_dir.lstrip("/"), "LICENSE")]
    ), "Project license must include the full GPLv3 text"
    found = set()
    with subprocess.Popen([dpkg_deb, "--fsys-tarfile", str(package)], stdout=subprocess.PIPE) as process:
        with tarfile.open(fileobj=process.stdout, mode="r|") as archive:
            for member in archive:
                path = PurePosixPath(member.name)
                compressed = path.suffix == ".gz"
                if compressed:
                    path = path.with_suffix("")
                if path in expected_files:
                    assert member.isfile(), f"Not a regular file: {path}"
                    with archive.extractfile(member) as installed:
                        contents = installed.read()
                        if compressed:
                            contents = gzip.decompress(contents)
                        assert contents == expected_files[path], f"Incorrect installed {path}"
                    found.add(path)
        result = process.wait()
        if result:
            raise subprocess.CalledProcessError(result, process.args)
    assert found == set(expected_files), f"Missing package license files: {set(expected_files) - found}"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Check installed DEB metadata and project license files")
    parser.add_argument("source", type=Path)
    parser.add_argument("package", type=Path)
    parser.add_argument("doc_dir")
    parser.add_argument("dpkg_deb")
    args = parser.parse_args()
    check_metadata(args.source, args.package, args.doc_dir, args.dpkg_deb)
