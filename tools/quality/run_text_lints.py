#!/usr/bin/env python3
"""Run available non-C++ text linters without requiring them for basic builds."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys


def run(command: list[str], root: pathlib.Path) -> int:
    print("+ " + " ".join(command))
    return subprocess.run(command, cwd=root, check=False).returncode


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, required=True)
    args = parser.parse_args()
    result = 0

    cmake_format = shutil.which("cmake-format")
    if cmake_format:
        cmake_files = [
            str(path.relative_to(args.root))
            for path in sorted(args.root.rglob("*"))
            if "build" not in path.parts
            and (path.name == "CMakeLists.txt" or path.name.endswith((".cmake", ".cmake.in")))
        ]
        result |= run([cmake_format, "--check", *cmake_files], args.root)
    else:
        print("SKIP: cmake-format not installed")

    yamllint = shutil.which("yamllint")
    if yamllint:
        result |= run([yamllint, "schemas", ".github/workflows"], args.root)
    else:
        print("SKIP: yamllint not installed")

    markdownlint = shutil.which("markdownlint-cli2")
    if markdownlint:
        result |= run([markdownlint, "README.md", "CONTRIBUTING.md", "SECURITY.md", "docs/**/*.md"], args.root)
    else:
        print("SKIP: markdownlint-cli2 not installed")

    return result


if __name__ == "__main__":
    sys.exit(main())
