#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]


def main() -> int:
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools/quality/check_ng_provider_runtime.py"),
            "check",
            "--root",
            str(ROOT),
        ],
        check=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
