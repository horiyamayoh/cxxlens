#!/usr/bin/env python3
"""Check evidence/coverage schema, authoritative rows, and branch fixtures."""

from __future__ import annotations

import pathlib
import sys

import yaml


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    schema = yaml.safe_load(
        (root / "schemas/cxxlens_evidence_coverage_contract.yaml").read_text(encoding="utf-8")
    )
    header = (root / "include/cxxlens/core/evidence.hpp").read_text(encoding="utf-8")
    source = (root / "src/core/evidence.cpp").read_text(encoding="utf-8")
    tests = (root / "tests/unit/core/evidence_coverage_test.cpp").read_text(encoding="utf-8")
    failures: list[str] = []

    states = schema["coverage"]["terminal_states"]
    guarantees = schema["guarantee"]["values"]
    if len(states) != len(set(states)) or len(states) != 5:
        failures.append("coverage state taxonomy must contain five unique states")
    if len(guarantees) != len(set(guarantees)) or len(guarantees) != 5:
        failures.append("guarantee taxonomy must contain five unique values")
    if "summary_" in header or "count_" in header:
        failures.append("coverage report contains separately mutable summary state")
    for required in ("count(states.at(index))", "complete() ?", "requested_", "units_"):
        if required not in source:
            failures.append(f"authoritative-row projection is missing {required!r}")
    for state in states:
        cpp_name = state.replace("not_applicable", "not_applicable")
        if f"coverage_state::{cpp_name}" not in tests:
            failures.append(f"coverage branch fixture missing: {state}")
    for guarantee in guarantees:
        if f"result_guarantee::{guarantee}" not in tests:
            failures.append(f"guarantee branch fixture missing: {guarantee}")
    for negative in (
        "duplicate request accepted",
        "missing terminal state accepted",
        "multiple terminal states accepted",
        "unrequested terminal row accepted",
        "exact guarantee accepted incomplete coverage",
        "exact guarantee accepted insufficient precision",
        "exact guarantee accepted missing evidence",
    ):
        if negative not in tests:
            failures.append(f"negative decision fixture missing: {negative}")

    if failures:
        print("evidence/coverage quality check failed:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    print("validated authoritative evidence/coverage rows and decision fixtures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
