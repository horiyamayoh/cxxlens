#!/usr/bin/env python3
"""Reject finding identity/order shortcuts and require conformance vectors."""

from __future__ import annotations

import pathlib
import re
import sys

import yaml


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    schema = yaml.safe_load((root / "schemas/cxxlens_finding_contract.yaml").read_text())
    source = (root / "src/core/finding.cpp").read_text(encoding="utf-8")
    tests = (root / "tests/unit/core/finding_contract_test.cpp").read_text(encoding="utf-8")
    failures: list[str] = []

    identity_block = source.split("make_finding_id", 1)[1].split("normalize_unresolved", 1)[0]
    for forbidden in schema["finding_identity"]["excludes"]:
        token = forbidden.replace("severity", "level").replace("checkout_root", "root")
        if token in identity_block:
            failures.append(f"finding identity contains forbidden field: {forbidden}")
    if "std::unordered_" in source:
        failures.append("finding path uses unordered iteration")
    if re.search(r"(?:message|diagnostic).*find\s*\(", source):
        failures.append("diagnostic prose controls finding behavior")
    for required in schema["finding_identity"]["includes"]:
        mapped = {
            "primary_semantic_file_id": "source_file",
            "primary_begin_offset": "source_begin",
            "primary_end_offset": "source_end",
            "identity_parameters": "parameters",
            "subject_semantic_id": "subject",
            "variant_signature": "variant",
        }.get(required, required)
        if f'"{mapped}"' not in identity_block:
            failures.append(f"finding identity missing field: {required}")
    for fixture in (
        "message/severity changed finding ID",
        "subject did not change ID",
        "source did not change ID",
        "variant did not change ID",
        "identity parameter did not change ID",
        "conflicting duplicate did not hard-fail",
        "filter dropped evidence/coverage/unresolved",
        "jobs=",
    ):
        if fixture not in tests:
            failures.append(f"finding conformance fixture missing: {fixture}")

    if failures:
        print("finding contract quality check failed:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    print("validated finding identity, ordering, duplicate, and filter contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
