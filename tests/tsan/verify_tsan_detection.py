#!/usr/bin/env python3
"""Prove that the configured TSan runtime rejects an intentional data race."""

from __future__ import annotations

import pathlib
import subprocess
import sys


fixture = pathlib.Path(sys.argv[1])
log = pathlib.Path(sys.argv[2])
completed = subprocess.run(
    [str(fixture)],
    check=False,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
)
log.parent.mkdir(parents=True, exist_ok=True)
log.write_text(completed.stdout, encoding="utf-8")

diagnostic = "WARNING: ThreadSanitizer: data race"
if completed.returncode != 66 or diagnostic not in completed.stdout:
    raise SystemExit(
        "TSan did not reject the intentional race with exit code 66; "
        f"see {log} (exit={completed.returncode})"
    )

print("validated TSan data-race detection with the intentional fixture")
