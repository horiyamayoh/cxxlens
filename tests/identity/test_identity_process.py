#!/usr/bin/env python3
"""Cross-process and locale determinism check for canonical identity vectors."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys


def run(executable: str, locale: str) -> str:
    environment = dict(os.environ)
    environment["LC_ALL"] = locale
    return subprocess.run(
        [executable, "--emit"], check=True, capture_output=True, text=True, env=environment
    ).stdout


def main() -> int:
    executable, golden_path, schema_path = sys.argv[1:]
    outputs = [run(executable, "C"), run(executable, "C"), run(executable, "C.UTF-8")]
    if len(set(outputs)) != 1:
        raise ValueError("canonical vector changed across process/locale")
    golden = pathlib.Path(golden_path).read_text(encoding="utf-8")
    if outputs[0] != golden:
        raise ValueError("canonical identity golden changed")
    schema = pathlib.Path(schema_path).read_text(encoding="utf-8")
    for required in (
        "cxxlens.identity-contract.v1",
        "fixed-width-big-endian",
        "full_hexadecimal_digits: 64",
        "short_display_is_identity: false",
        "distinct_payload_same_key: hard-error",
    ):
        if required not in schema:
            raise ValueError(f"identity contract is missing {required}")
    if schema.count("prefix:") != 12:
        raise ValueError("identity domain registry is incomplete")
    print("validated cross-process canonical identity golden and schema")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
