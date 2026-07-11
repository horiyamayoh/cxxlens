#!/usr/bin/env python3
"""Check evidence/coverage golden stability across process roots and locales."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile


def emit(executable: pathlib.Path, cwd: pathlib.Path, locale: str) -> str:
    environment = os.environ.copy()
    environment["LC_ALL"] = locale
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
    with tempfile.TemporaryDirectory(prefix="cxxlens-evidence-a-") as first_root:
        with tempfile.TemporaryDirectory(prefix="cxxlens-evidence-b-") as second_root:
            outputs = {
                emit(executable, pathlib.Path(first_root), "C"),
                emit(executable, pathlib.Path(second_root), "C.UTF-8"),
            }
    if outputs != {golden}:
        print("evidence/coverage process, root, locale, or golden divergence", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
