#!/usr/bin/env python3
"""Validate stable failure registry traceability and sentinel-free operations."""

from __future__ import annotations

import pathlib
import re
import sys

import yaml


def fail(message: str) -> None:
    raise ValueError(message)


def require_sorted_unique(values: list[str], label: str) -> None:
    if values != sorted(set(values)):
        fail(f"{label} must be sorted and unique")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} REPOSITORY_ROOT", file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1])
    schema = yaml.safe_load((root / "schemas/cxxlens_failure_contract.yaml").read_text())
    catalog = yaml.safe_load((root / "schemas/cxxlens_public_api_contract.yaml").read_text())
    source = (root / "src/core/failure.cpp").read_text()
    header = (root / "include/cxxlens/core/failure.hpp").read_text()

    try:
        schema_codes = schema["common_error_codes"]
        catalog_codes = catalog["registries"]["error_codes"]
        require_sorted_unique(schema_codes, "failure schema common_error_codes")
        require_sorted_unique(catalog_codes, "API catalog error_codes")
        if schema_codes != catalog_codes:
            fail("failure schema and API catalog stable code registries differ")

        array = re.search(r"built_in_codes\{(?P<body>.*?)\n\s*\};", source, re.DOTALL)
        if array is None:
            fail("C++ built_in_codes registry was not found")
        cpp_codes = re.findall(r'"([a-z][a-z0-9.-]+)"', array.group("body"))
        require_sorted_unique(cpp_codes, "C++ built_in_codes")
        if cpp_codes != schema_codes:
            fail("C++ and schema stable code registries differ")

        required_signatures = (
            "result<void>\n\t\tvalidate(",
            "result<void> register_code(",
            "result<T> propagate_failure(",
        )
        for signature in required_signatures:
            if signature not in header:
                fail(f"missing result-based public operation: {signature!r}")
        if re.search(r"(?:bool|std::string)\s+(?:validate|register_code)\s*\(", header):
            fail("expected failure operation uses a bool/string sentinel")
        if re.search(r"(?:message|summary).*find\s*\(", source):
            fail("diagnostic prose participates in failure control flow")
    except (KeyError, TypeError, ValueError) as error:
        print(f"failure contract quality check failed: {error}", file=sys.stderr)
        return 1

    print(f"validated {len(schema_codes)} stable failure codes and result-based operations")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
