#!/usr/bin/env python3
"""Enforce production-path testing substrate boundaries and branch fixtures."""

from __future__ import annotations

import pathlib
import sys


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    public = (root / "include/cxxlens/testing.hpp").read_text(encoding="utf-8")
    implementation = "\n".join(
        path.read_text(encoding="utf-8") for path in sorted((root / "src/testing").glob("*.cpp"))
    )
    unit = (root / "tests/unit/testing/testing_substrate_test.cpp").read_text(encoding="utf-8")
    failures: list[str] = []
    for symbol in (
        "workspace_fixture",
        "fixture_bundle",
        "fault_plan",
        "result_assertion",
        "normalize_golden",
        "assert_schema_conforms",
        "check_property",
        "check_determinism",
    ):
        if symbol not in public:
            failures.append(f"missing testing API symbol: {symbol}")
    for forbidden in ("clang::", "llvm::", "fake_fact_backend", "fake_semantic_backend"):
        if forbidden in public or forbidden in implementation:
            failures.append(f"testing substrate fabricates or leaks backend surface: {forbidden}")
    if "cxxlens::detail::json::write" not in implementation:
        failures.append("testing serializers bypass common canonical writer")
    for fixture in (
        "fixture insertion order or root",
        "success-empty assertion",
        "partial unresolved assertion",
        "failed error was not preserved",
        "schema failure path",
        "golden normalizer hid semantic",
        "property case did not replay",
        "deterministic port fault",
    ):
        if fixture not in unit:
            failures.append(f"testing branch fixture missing: {fixture}")
    categories = root / "tests/fixtures/m0"
    for category in ("positive", "negative", "ambiguous"):
        if not any((categories / category).glob("*.yaml")):
            failures.append(f"fixture discovery category is empty: {category}")
    if not (root / "schemas/cxxlens_testing_fixture.schema.yaml").is_file():
        failures.append("testing fixture schema is missing")

    if failures:
        print("testing contract quality check failed:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    print("validated production-path fixture, assertion, property, schema, and golden substrate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
