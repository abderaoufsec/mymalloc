#!/usr/bin/env python3
"""Check (or fix) formatting of all C++ sources with clang-format.

Usage:
    python scripts/check_format.py          # verify formatting (CI mode)
    python scripts/check_format.py --fix    # rewrite files in place

Requires `clang-format` on PATH (CI pins it via pip: clang-format==23.1.3).
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE_DIRS = ["include", "src", "tests", "examples"]
EXTENSIONS = {".c", ".cc", ".cpp", ".h", ".hpp"}


def collect_files():
    files = []
    for directory in SOURCE_DIRS:
        base = ROOT / directory
        if base.exists():
            files.extend(path for path in base.rglob("*") if path.suffix in EXTENSIONS)
    return sorted(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fix", action="store_true", help="rewrite files instead of checking")
    args = parser.parse_args()

    executable = shutil.which("clang-format")
    if executable is None:
        print("error: clang-format not found on PATH", file=sys.stderr)
        return 2

    files = collect_files()
    if not files:
        print("error: no source files found", file=sys.stderr)
        return 2

    command = [executable] + (["-i"] if args.fix else ["--dry-run", "--Werror"])
    result = subprocess.run(command + [str(path) for path in files], cwd=ROOT)

    if result.returncode == 0:
        action = "formatted" if args.fix else "OK"
        print(f"clang-format: {len(files)} file(s) {action}")
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())