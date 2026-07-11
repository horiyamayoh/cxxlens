#!/usr/bin/env python3
"""Check C++ files with clang-format without modifying the worktree."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang-format", required=True)
    parser.add_argument("--root", type=pathlib.Path, required=True)
    args = parser.parse_args()

    files: list[pathlib.Path] = []
    for directory in ("include", "src", "tests", "examples"):
        base = args.root / directory
        if base.exists():
            files.extend(path for path in base.rglob("*") if path.suffix in {".cpp", ".hpp"})
    if not files:
        return 0

    command = [args.clang_format, "--dry-run", "--Werror", *map(str, sorted(files))]
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
