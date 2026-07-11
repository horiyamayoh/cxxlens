#!/usr/bin/env python3
"""Validate checked-in source-span canonical JSON vectors without third-party packages."""

from __future__ import annotations

import json
import pathlib
import sys


EXPECTED_KEYS = [
    "schema", "primary", "spelling", "expansion", "macro_stack", "origin", "digest", "read_only"
]
RANGE_KEYS = [
    "file", "begin", "end", "begin_line", "begin_column", "end_line", "end_column", "kind"
]


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    schema = (root / "schemas/cxxlens_source_span.schema.yaml").read_text(encoding="utf-8")
    if "cxxlens.source-span.v1" not in schema or "minimum: 1" not in schema:
        raise ValueError("source schema version/coordinate constraints are missing")
    for path in sorted((root / "tests/golden/source").glob("*.json")):
        raw = path.read_text(encoding="utf-8").rstrip("\n")
        value = json.loads(raw, object_pairs_hook=dict)
        if list(value) != EXPECTED_KEYS or value["schema"] != "cxxlens.source-span.v1":
            raise ValueError(f"{path}: non-canonical top-level fields")
        ranges = [value["primary"], value["spelling"], value["expansion"]]
        ranges.extend(frame["invocation"] for frame in value["macro_stack"])
        ranges.extend(frame["definition"] for frame in value["macro_stack"])
        for item in filter(None, ranges):
            if list(item) != RANGE_KEYS or not item["file"].startswith("file_"):
                raise ValueError(f"{path}: non-canonical range")
            if item["begin"] > item["end"] or min(item["begin_line"], item["begin_column"]) < 1:
                raise ValueError(f"{path}: invalid half-open coordinates")
        if "/home/" in raw or "file:/" in raw:
            raise ValueError(f"{path}: absolute root leaked")
        if json.loads(json.dumps(value, separators=(",", ":"), ensure_ascii=False)) != value:
            raise ValueError(f"{path}: JSON round-trip changed semantic fields")
    print("validated source-span schema and canonical golden vectors")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
