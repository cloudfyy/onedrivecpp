"""Report project coverage using profiles matched to each executable's Build ID."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess


def run(command: list[str]) -> str:
    return subprocess.run(
        command, check=True, text=True, stdout=subprocess.PIPE
    ).stdout


def merge_lcov(text: str, root: Path, files: dict) -> None:
    current = None
    for line in text.splitlines():
        if line.startswith("SF:"):
            path = Path(line[3:])
            current = None
            if path.is_relative_to(root / "src") or path.is_relative_to(
                root / "include" / "onedrive"
            ):
                current = files.setdefault(str(path), {
                    "lines": {}, "branches": {}, "functions": {},
                })
        elif current is not None:
            if line.startswith("DA:"):
                number, count, *_ = line[3:].split(",")
                metric, key, value = "lines", number, int(count)
            elif line.startswith("FNDA:"):
                count, key = line[5:].split(",", 1)
                metric, value = "functions", int(count)
            elif line.startswith("BRDA:"):
                number, block, branch, taken = line[5:].split(",")
                metric, key = "branches", f"{number},{block},{branch}"
                value = 0 if taken == "-" else int(taken)
            else:
                continue
            current[metric][key] = max(current[metric].get(key, 0), value)


def summarize(values: dict) -> dict:
    count = len(values)
    covered = sum(value > 0 for value in values.values())
    return {
        "count": count,
        "covered": covered,
        "percent": 100 * covered / count if count else 100,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--llvm-cov", default="llvm-cov-20")
    parser.add_argument("--llvm-profdata", default="llvm-profdata-20")
    parser.add_argument("--name", default="report")
    args = parser.parse_args()
    build = args.build.resolve()
    root = Path(__file__).resolve().parents[1]
    output = build / "coverage"
    profiles = sorted((output / "profiles").glob("*.profraw"))
    binaries = sorted(build.glob("*_tests"))
    binaries += [
        path for path in (build / "onedrive-cpp", build / "onedrive-cpp-gui")
        if path.is_file()
    ]
    if not profiles or not binaries:
        parser.error("No profiles or test executables; build and run coverage tests first")
    for tool in (args.llvm_cov, args.llvm_profdata, "readelf"):
        if shutil.which(tool) is None:
            parser.error(f"Required tool not found: {tool}")
    if not args.name or Path(args.name).name != args.name:
        parser.error("--name must be a filename, not a path")

    by_id: dict[str, list[Path]] = {}
    for profile in profiles:
        info = run([args.llvm_profdata, "show", "--binary-ids", str(profile)])
        if "Binary IDs:" not in info:
            parser.error(f"Profile has no binary identity: {profile}")
        for identifier in info.split("Binary IDs:", 1)[1].split():
            by_id.setdefault(identifier, []).append(profile)

    parts = output / f"{args.name}-objects"
    parts.mkdir(exist_ok=True)
    zero_text = parts / "zero.proftext"
    zero_text.write_text(":ir\n")
    zero_profile = parts / "zero.profdata"
    run([args.llvm_profdata, "merge", "-sparse", str(zero_text),
         "-o", str(zero_profile)])
    files: dict = {}
    matched_ids = set()
    unexecuted = []
    for binary in binaries:
        match = re.search(r"Build ID:\s*([0-9a-f]+)", run(["readelf", "-n", str(binary)]))
        if match is None:
            parser.error(f"Executable has no Build ID: {binary}")
        identifier = match.group(1)
        matched_ids.add(identifier)
        matching = by_id.get(identifier, [])
        profile = zero_profile
        if matching:
            profile = parts / f"{binary.name}.profdata"
            run([args.llvm_profdata, "merge", "-sparse",
                 *map(str, matching), "-o", str(profile)])
        else:
            unexecuted.append(binary.name)
        text = run([
            args.llvm_cov, "export", str(binary), f"-instr-profile={profile}",
            "-format=lcov", "-debuginfod=false",
            "-ignore-filename-regex=(/usr/|/build/|/tests/)",
        ])
        (parts / f"{binary.name}.info").write_text(text)
        merge_lcov(text, root, files)
    stale_ids = set(by_id) - matched_ids
    if stale_ids:
        parser.error("Profiles contain unknown Build IDs; start with a clean profiles directory")
    if not files:
        parser.error("LLVM export contained no project source files")

    entries = [
        {
            "filename": filename,
            "summary": {metric: summarize(values) for metric, values in metrics.items()},
            "uncovered_lines": sorted(int(line) for line, count in metrics["lines"].items() if not count),
        }
        for filename, metrics in files.items()
    ]
    totals = {}
    lines = ["Project-only LCOV coverage; excludes tests, dependencies and generated code."]
    for metric in ("lines", "branches", "functions"):
        count = sum(entry["summary"][metric]["count"] for entry in entries)
        covered = sum(entry["summary"][metric]["covered"] for entry in entries)
        percent = 100 * covered / count if count else 100
        totals[metric] = {"count": count, "covered": covered, "percent": percent}
        lines.append(f"{metric}: {covered}/{count} ({percent:.2f}%)")
    lines.append("No execution profile: " + ", ".join(unexecuted))
    lines.append("\nFile\tLines\tBranches")
    for entry in sorted(entries, key=lambda entry: entry["summary"]["lines"]["percent"]):
        summary = entry["summary"]
        path = Path(entry["filename"]).relative_to(root)
        lines.append(
            f"{path}\t{summary['lines']['percent']:.2f}%"
            f"\t{summary['branches']['percent']:.2f}%"
        )
    report = "\n".join(lines) + "\n"
    (output / f"{args.name}.json").write_text(json.dumps({
        "totals": totals, "files": entries, "unexecuted_binaries": unexecuted,
    }))
    (output / f"{args.name}.txt").write_text(report)
    print(report, end="")
    print(f"Per-executable LLVM LCOV exports: {parts}")


if __name__ == "__main__":
    main()
