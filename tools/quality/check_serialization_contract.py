#!/usr/bin/env python3
"""Enforce the common canonical writer and schema registry authority."""

from __future__ import annotations

import pathlib
import re
import sys

import yaml


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    manifest = yaml.safe_load((root / "schemas/cxxlens_schema_compatibility.yaml").read_text())
    registry = (root / "src/core/schema_registry.cpp").read_text(encoding="utf-8")
    writer = (root / "src/core/canonical_json.cpp").read_text(encoding="utf-8")
    tests = (root / "tests/unit/core/canonical_json_test.cpp").read_text(encoding="utf-8")
    failures: list[str] = []

    document_ids = [entry["id"] for entry in manifest["documents"]]
    if document_ids != sorted(set(document_ids)):
        failures.append("schema manifest IDs must be sorted unique")
    for entry in manifest["documents"]:
        if not (root / "schemas" / entry["file"]).is_file():
            failures.append(f"missing registered schema file: {entry['file']}")
        if f'"{entry["id"]}"' not in registry:
            failures.append(f"C++ registry missing schema: {entry['id']}")

    serializer_files = [
        root / "src/config/configuration.cpp",
        root / "src/source/source_span.cpp",
        root / "src/core/evidence.cpp",
        root / "src/core/failure.cpp",
        root / "src/core/finding.cpp",
        root / "src/core/json_projections.cpp",
    ]
    ad_hoc = re.compile(r'(?:R"\(\{|"\{\\?"|json_escape\s*\()')
    for path in serializer_files:
        text = path.read_text(encoding="utf-8")
        if ad_hoc.search(text):
            failures.append(f"{path}: ad-hoc JSON construction is forbidden")
        if "std::unordered_" in text:
            failures.append(f"{path}: unordered serializer iteration is forbidden")
    for forbidden in ("system_clock::now", "getpid", "this_thread::get_id", "cache_hit"):
        if forbidden in writer or any(forbidden in path.read_text() for path in serializer_files):
            failures.append(f"semantic serializer references operational metadata: {forbidden}")

    for fixture in (
        "duplicate key accepted",
        "infinite number accepted",
        "NaN accepted",
        "invalid UTF-8 accepted",
        "deep nesting limit not enforced",
        "locale changed number encoding",
    ):
        if fixture not in tests:
            failures.append(f"canonical writer branch fixture missing: {fixture}")

    if failures:
        print("serialization contract quality check failed:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    print(f"validated common writer and {len(document_ids)} registered M0 schemas")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
