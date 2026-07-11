#!/usr/bin/env python3
"""Check finding ID/order stability across process roots, locales, and hash seed."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile


def emit(executable: pathlib.Path, cwd: pathlib.Path, locale: str, seed: str) -> str:
    environment = os.environ.copy()
    environment["LC_ALL"] = locale
    environment["PYTHONHASHSEED"] = seed
    return subprocess.run(
        [executable, "--emit"],
        cwd=cwd,
        env=environment,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout


def main() -> int:
    executable = pathlib.Path(sys.argv[1]).resolve()
    golden = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="cxxlens-finding-a-") as first_root:
        with tempfile.TemporaryDirectory(prefix="cxxlens-finding-b-") as second_root:
            outputs = {
                emit(executable, pathlib.Path(first_root), "C", "1"),
                emit(executable, pathlib.Path(second_root), "C.UTF-8", "777"),
            }
    if outputs != {golden}:
        print("finding process/root/locale/hash-seed golden divergence", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
