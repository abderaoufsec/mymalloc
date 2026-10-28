#!/usr/bin/env python3
"""Run clang-tidy over every translation unit in a compilation database.

Usage:
    python scripts/run_clang_tidy.py <path-to-compile_commands.json>

Each source file recorded in the compilation database (library, tests and
examples) is analyzed with the repository's .clang-tidy. Findings are treated
as errors (``--warnings-as-errors='*'``), so any diagnostic fails the run with
a non-zero exit status. This is the same set of files cppcheck analyzes, so a
finding anywhere in the tree is caught.

Requires `clang-tidy` on PATH (CI pins it via pip: clang-tidy==22.1.8).
"""

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# A clang-tidy finding names one of the enabled checks in a trailing
# "[check-name]" token, e.g. "[bugprone-narrowing-conversions]". Match only
# diagnostic lines ("warning:"/"error:") so the code-context lines clang-tidy
# prints around a diagnostic (which contain array subscripts such as
# "fresh[index]") are never mistaken for findings. Compiler diagnostics
# surfaced while parsing (missing/mismatched system headers) are tagged
# "[clang-diagnostic...]" and are NOT findings: on a MSYS2/MinGW host the
# bundled clang-tidy can mis-parse the toolchain's own headers, so that noise
# must not fail the run. On Linux CI the headers parse cleanly and every real
# finding is still caught.
FINDING_RE = re.compile(r"(?:warning|error):.*\[(?!clang-diagnostic)[a-zA-Z][\w\-]*\]")


def main(argv):
    if len(argv) != 2:
        print(f"usage: {Path(argv[0]).name} <compile_commands.json>", file=sys.stderr)
        return 2

    database = Path(argv[1])
    if not database.is_file():
        print(f"error: compilation database not found: {database}", file=sys.stderr)
        return 2

    executable = shutil.which("clang-tidy")
    if executable is None:
        print("error: clang-tidy not found on PATH", file=sys.stderr)
        return 2

    # utf-8-sig tolerates an optional BOM (some editors/tools emit one).
    entries = json.loads(database.read_text(encoding="utf-8-sig"))
    # The database lists one entry per TU; deduplicate the source paths while
    # preserving order so each file is analyzed exactly once.
    sources = []
    seen = set()
    for entry in entries:
        source = entry.get("file")
        if source and source not in seen:
            seen.add(source)
            sources.append(source)

    if not sources:
        print("error: no source files in compilation database", file=sys.stderr)
        return 2

    # -p points clang-tidy at the build directory so it reuses the exact
    # compiler invocation (include paths, -std, defines) for each file. Run
    # from the repo root so the .clang-tidy config and the source paths (which
    # are absolute in the database) resolve the same way they do in CI.
    build_dir = str(database.resolve().parent)
    command = [
        executable,
        "-p",
        build_dir,
        "--config-file=.clang-tidy",
    ]
    # Optional extra flags, primarily for local MSYS2/MinGW runs where the pip
    # clang-tidy does not know the toolchain's own C++ standard-library include
    # path. CI (Linux) leaves this unset and reads everything from the compile
    # database. Space-separated, each passed as its own argument.
    extra = os.environ.get("CLANG_TIDY_EXTRA_ARGS", "")
    if extra:
        command += extra.split()

    print(f"clang-tidy: analyzing {len(sources)} file(s)")
    failed = []
    for source in sources:
        result = subprocess.run(
            command + [source], cwd=ROOT, capture_output=True, text=True
        )
        findings = [
            line
            for line in (result.stdout + result.stderr).splitlines()
            if FINDING_RE.search(line)
        ]
        if findings:
            failed.append((source, findings))

    if failed:
        print(f"clang-tidy: {len(failed)} file(s) with findings:", file=sys.stderr)
        for source, findings in failed:
            print(f"  {source}", file=sys.stderr)
            for line in findings:
                print(f"    {line.strip()}", file=sys.stderr)
        return 1

    print(f"clang-tidy: {len(sources)} file(s) OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
