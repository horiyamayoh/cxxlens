#!/usr/bin/env python3
"""Verify the canonical fact snapshot binary golden and process determinism."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


binary = pathlib.Path(sys.argv[1])
golden = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
outputs = [
    subprocess.run([binary, "--emit"], check=True, text=True, capture_output=True).stdout
    for _ in range(3)
]
assert outputs[0] == outputs[1] == outputs[2]
assert re.fullmatch(r"[0-9a-f]{16}\n", outputs[0])
assert outputs[0] == golden
print("validated canonical fact snapshot binary golden and process determinism")
