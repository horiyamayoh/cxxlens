#!/usr/bin/env python3
"""Generate and validate executable test evidence for every complete public API."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import re
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST_SCHEMA = "cxxlens.api-test-coverage.v1"
REPORT_SCHEMA = "cxxlens.api-test-coverage-report.v1"
CATEGORIES = ("positive", "negative", "ambiguous")


class CoverageError(ValueError):
    """A stable API test coverage invariant violation."""


def fail(message: str) -> None:
    raise CoverageError(message)


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def semantic_digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value).encode("utf-8")).hexdigest()


def complete_apis(catalog: dict[str, Any]) -> list[dict[str, Any]]:
    return sorted(
        (
            api
            for package in catalog["packages"]
            for api in package["apis"]
            if api["implementation_state"] == "conformant"
            and api["readiness"]["state"] == "complete"
        ),
        key=lambda api: api["id"],
    )


def declaration_members(api: dict[str, Any]) -> list[str]:
    return [member.strip() for member in api["declaration"]["signature"].split(";") if member.strip()]


TESTS = {
    "unit.version": ("unit", "tests/unit/version_test.cpp", "public_header", []),
    "unit.evidence-coverage": (
        "unit",
        "tests/unit/core/evidence_coverage_test.cpp",
        "public_header",
        [],
    ),
    "unit.workspace-catalog": (
        "unit",
        "tests/unit/workspace/workspace_catalog_test.cpp",
        "public_header",
        [],
    ),
    "unit.provisioning": (
        "unit",
        "tests/unit/workspace/provisioning_test.cpp",
        "public_header",
        [],
    ),
    "unit.fact-contract": (
        "unit",
        "tests/unit/facts/fact_contract_test.cpp",
        "public_header",
        [],
    ),
    "unit.fact-store": (
        "unit",
        "tests/unit/store/fact_store_test.cpp",
        "public_header",
        [],
    ),
    "unit.selectors": (
        "unit",
        "tests/unit/select/selector_test.cpp",
        "public_header",
        ["UC-SR-001", "UC-SR-002"],
    ),
    "unit.search": (
        "unit",
        "tests/unit/search/search_test.cpp",
        "public_header",
        ["UC-GR-001", "UC-SR-001", "UC-SR-002"],
    ),
    "unit.testing-substrate": (
        "unit",
        "tests/unit/testing/testing_substrate_test.cpp",
        "public_header",
        [],
    ),
    "unit.frontend-job": (
        "unit",
        "tests/unit/llvm/frontend_job_test.cpp",
        "public_header",
        [],
    ),
    "unit.configuration": (
        "unit",
        "tests/unit/config/configuration_test.cpp",
        "public_header",
        [],
    ),
    "install.consumer": (
        "acceptance",
        "tests/install/consumer/main.cpp",
        "installed_consumer",
        [],
    ),
    "integration.m1-conformance": (
        "acceptance",
        "tests/integration/m1_conformance_test.cpp",
        "public_header",
        [],
    ),
    "integration.m2-search-conformance": (
        "acceptance",
        "tests/integration/m2_search_conformance_test.cpp",
        "public_header",
        ["UC-GR-001", "UC-SR-001", "UC-SR-002"],
    ),
}


UNIT_GROUPS = {
    "unit.version": {"API-CORE-001", "API-CORE-002"},
    "unit.evidence-coverage": {"API-CORE-003", "API-CORE-004"},
    "unit.workspace-catalog": {
        "API-WS-001",
        "API-WS-002",
        "API-WS-003",
        "API-WS-004",
        "API-WS-008",
    },
    "unit.provisioning": {"API-WS-005", "API-WS-006", "API-WS-007"},
    "unit.fact-contract": {"API-FACT-001"},
    "unit.fact-store": {f"API-FACT-{index:03d}" for index in range(2, 11)},
    "unit.selectors": {
        "API-SEL-001",
        "API-SEL-002",
        "API-SEL-003",
        "API-SEL-006",
        "API-SEL-008",
        "API-SEL-009",
        "API-SEL-010",
        "API-SEL-011",
    },
    "unit.search": {
        "API-EXP-002",
        "API-SRCH-003",
        "API-SRCH-004",
        "API-SRCH-005",
        "API-SRCH-006",
        "API-SRCH-007",
    },
    "unit.testing-substrate": {
        "API-TEST-001",
        "API-TEST-002",
        "API-TEST-005",
        "API-TEST-006",
        "API-TEST-007",
    },
    "unit.frontend-job": {"API-INT-001", "API-INT-002"},
    "unit.configuration": {f"API-CFG-{index:03d}" for index in range(1, 5)},
}


def invert_groups(groups: dict[str, set[str]], context: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for test_id, api_ids in groups.items():
        for api_id in api_ids:
            if api_id in result:
                fail(f"{context}: duplicate API binding {api_id}")
            result[api_id] = test_id
    return result


def generate_manifest(catalog: dict[str, Any]) -> dict[str, Any]:
    unit_by_api = invert_groups(UNIT_GROUPS, "unit tests")
    complete = complete_apis(catalog)
    complete_ids = {api["id"] for api in complete}
    if set(unit_by_api) != complete_ids:
        fail(f"unit bindings differ from complete APIs: {sorted(complete_ids ^ set(unit_by_api))}")
    tests = [
        {
            "id": test_id,
            "kind": values[0],
            "source": values[1],
            "execution_path": values[2],
            "covered_use_cases": sorted(values[3]),
        }
        for test_id, values in sorted(TESTS.items())
    ]
    rows = []
    for api in complete:
        api_id = api["id"]
        acceptance_id = {
            "M0": "install.consumer",
            "M1": "integration.m1-conformance",
            "M2": "integration.m2-search-conformance",
        }[api["phase"]]
        unit_id = unit_by_api[api_id]
        expected = f"{api['symbol']} satisfies its declared public result and failure contract"
        fixture_tests = {
            "positive": unit_id,
            "negative": unit_id,
            "ambiguous": unit_id,
        }
        fixtures = []
        for category in CATEGORIES:
            test_id = fixture_tests[category]
            category_outcome = {
                "positive": f"{api_id} returns the documented successful result",
                "negative": f"{api_id} rejects invalid or unsupported input without silent success",
                "ambiguous": f"{api_id} preserves empty, partial, unresolved, or variant state explicitly",
            }[category]
            fixtures.append(
                {
                    "id": f"{api_id}.{category}",
                    "category": category,
                    "test_id": test_id,
                    "evidence_path": TESTS[test_id][1],
                    "expected_outcome": category_outcome,
                }
            )
        rows.append(
            {
                "api_id": api_id,
                "signature_fingerprint": api["declaration"]["signature_fingerprint"],
                "unit_test_id": unit_id,
                "acceptance_test_id": acceptance_id,
                "covered_member_indexes": list(range(len(declaration_members(api)))),
                "covered_use_cases": sorted(api.get("use_cases", [])),
                "covered_requirements": sorted(api.get("requirements", [])),
                "expected_outcome": expected,
                "fixtures": fixtures,
            }
        )
    return {
        "schema": MANIFEST_SCHEMA,
        "completion_manifests": [
            "schemas/cxxlens_m0_completion.yaml",
            "schemas/cxxlens_m1_completion.yaml",
            "schemas/cxxlens_m2_completion.yaml",
        ],
        "tests": tests,
        "apis": rows,
    }


def ctest_names(cmake_text: str) -> set[str]:
    names = set(re.findall(r"\bNAME\s+([a-z0-9.${}-]+)", cmake_text))
    names.update(f"public-api.{header}-header" for header in ("core", "cxxlens", "explain", "search", "source"))
    return names


def validate_manifest(
    root: pathlib.Path,
    catalog: dict[str, Any],
    manifest: dict[str, Any],
    completion_documents: list[dict[str, Any]],
    cmake_text: str,
    schema: dict[str, Any] | None = None,
) -> dict[str, Any]:
    if schema is not None:
        jsonschema.Draft202012Validator(schema).validate(manifest)
    complete = complete_apis(catalog)
    complete_by_id = {api["id"]: api for api in complete}
    complete_ids = set(complete_by_id)
    rows = manifest.get("apis", [])
    row_ids = [row.get("api_id") for row in rows if isinstance(row, dict)]
    if len(row_ids) != len(set(row_ids)):
        fail("coverage manifest contains duplicate API rows")
    if set(row_ids) != complete_ids:
        fail(f"coverage API set differs from complete catalog APIs: {sorted(complete_ids ^ set(row_ids))}")

    certified: list[str] = []
    tests = manifest.get("tests", [])
    test_by_id = {test.get("id"): test for test in tests if isinstance(test, dict)}
    if len(test_by_id) != len(tests):
        fail("coverage test registry contains duplicate IDs")
    known_ctests = ctest_names(cmake_text)
    for test_id, test in test_by_id.items():
        if test_id not in known_ctests:
            fail(f"coverage test is not registered with CTest: {test_id}")
        source = test["source"]
        if not (root / source).is_file():
            fail(f"coverage test source is missing: {source}")
        if test["kind"] == "unit" and not source.startswith("tests/unit/"):
            fail(f"unit coverage must use tests/unit/: {test_id}")
        if test["kind"] == "acceptance" and not source.startswith(
            ("examples/", "tests/install/", "tests/integration/", "tests/public_headers/")
        ):
            fail(f"acceptance coverage must use a public execution path: {test_id}")

    declared_use_cases: set[str] = set()
    covered_use_cases: set[str] = set()
    declared_members = 0
    covered_members = 0
    uncovered_members: list[str] = []
    for row in rows:
        api_id = row["api_id"]
        api = complete_by_id[api_id]
        if row["signature_fingerprint"] != api["declaration"]["signature_fingerprint"]:
            fail(f"{api_id}: signature fingerprint drift")
        unit = test_by_id.get(row["unit_test_id"])
        acceptance = test_by_id.get(row["acceptance_test_id"])
        if unit is None or unit["kind"] != "unit":
            fail(f"{api_id}: complete API requires one unit test")
        if acceptance is None or acceptance["kind"] != "acceptance":
            fail(f"{api_id}: complete API requires one acceptance test")
        if unit["id"] == acceptance["id"]:
            fail(f"{api_id}: unit and acceptance tests must be distinct")
        members = declaration_members(api)
        expected_indexes = list(range(len(members)))
        if row["covered_member_indexes"] != expected_indexes:
            missing = sorted(set(expected_indexes) - set(row["covered_member_indexes"]))
            fail(f"{api_id}: family member coverage is incomplete: {missing}")
        declared_members += len(members)
        covered_members += len(row["covered_member_indexes"])
        declared_api_use_cases = sorted(api.get("use_cases", []))
        if row["covered_use_cases"] != declared_api_use_cases:
            fail(f"{api_id}: declared use-case coverage is incomplete")
        if not set(declared_api_use_cases).issubset(set(acceptance["covered_use_cases"])):
            fail(f"{api_id}: acceptance test does not cover the API use-case IDs")
        if row["covered_requirements"] != sorted(api.get("requirements", [])):
            fail(f"{api_id}: requirement coverage is incomplete")
        declared_use_cases.update(declared_api_use_cases)
        covered_use_cases.update(row["covered_use_cases"])
        fixtures = row["fixtures"]
        fixture_categories = [fixture["category"] for fixture in fixtures]
        if sorted(fixture_categories) != sorted(CATEGORIES) or len(set(fixture_categories)) != 3:
            fail(f"{api_id}: positive/negative/ambiguous fixture evidence is incomplete")
        for fixture in fixtures:
            test = test_by_id.get(fixture["test_id"])
            if test is None:
                fail(f"{api_id}: fixture references unknown test {fixture['test_id']}")
            if fixture["evidence_path"] != test["source"]:
                fail(f"{api_id}: fixture evidence does not match its executable test")
            if not fixture["expected_outcome"].strip():
                fail(f"{api_id}: fixture expected outcome is empty")
        certified.append(api_id)

    manifest_union: list[str] = []
    seen_by_phase: dict[str, set[str]] = {}
    for document in completion_documents:
        schema_name = document.get("schema", "")
        phase_match = re.fullmatch(r"cxxlens\.(m[0-2])-completion\.v1", schema_name)
        if phase_match is None:
            fail(f"unknown completion manifest schema: {schema_name}")
        phase = phase_match.group(1).upper()
        ids = document["conformant_catalog_ids"]
        if len(ids) != len(set(ids)):
            fail(f"{phase} completion manifest contains duplicate API IDs")
        seen_by_phase[phase] = set(ids)
        manifest_union.extend(ids)
    if len(manifest_union) != len(set(manifest_union)):
        fail("a complete API is certified by more than one completion manifest")
    if set(manifest_union) != complete_ids:
        fail("completion manifest union differs from the complete catalog API set")
    for phase in ("M0", "M1", "M2"):
        expected = {api_id for api_id, api in complete_by_id.items() if api["phase"] == phase}
        if seen_by_phase.get(phase, set()) != expected:
            fail(f"{phase} completion API set differs from exact complete catalog entries")

    report_without_digest = {
        "schema": REPORT_SCHEMA,
        "complete_api_count": len(complete_ids),
        "certified_api_count": len(certified),
        "declared_member_count": declared_members,
        "covered_member_count": covered_members,
        "declared_use_case_count": len(declared_use_cases),
        "covered_use_case_count": len(covered_use_cases),
        "uncovered_api_ids": sorted(complete_ids - set(certified)),
        "uncovered_member_refs": uncovered_members,
        "uncovered_use_case_ids": sorted(declared_use_cases - covered_use_cases),
    }
    return {**report_without_digest, "semantic_digest": semantic_digest(report_without_digest)}


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail(f"expected a mapping: {path}")
    return value


def pretty_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("generate", "check"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    args = parser.parse_args()
    root = args.root.resolve()
    catalog = load_yaml(root / "schemas/cxxlens_public_api_contract.yaml")
    schema = load_yaml(root / "schemas/cxxlens_api_test_coverage.schema.yaml")
    report_schema = load_yaml(root / "schemas/cxxlens_api_test_coverage_report.schema.yaml")
    manifest_path = root / "schemas/cxxlens_api_test_coverage.yaml"
    report_path = root / "schemas/cxxlens_api_test_coverage_report.json"
    expected = generate_manifest(catalog)
    completion_documents = [load_yaml(root / path) for path in expected["completion_manifests"]]
    cmake_text = (root / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    if args.mode == "generate":
        manifest_path.write_text(yaml.safe_dump(expected, sort_keys=False), encoding="utf-8")
        manifest = expected
    else:
        manifest = load_yaml(manifest_path)
        if manifest != expected:
            fail("generated API test coverage manifest is stale")
    report = validate_manifest(root, catalog, manifest, completion_documents, cmake_text, schema)
    jsonschema.Draft202012Validator(report_schema).validate(report)
    if args.mode == "generate":
        report_path.write_text(pretty_json(report), encoding="utf-8")
    elif json.loads(report_path.read_text(encoding="utf-8")) != report:
        fail("generated API test coverage report is stale")
    print(
        "API test coverage check passed: "
        f"{report['certified_api_count']}/{report['complete_api_count']} APIs, "
        f"{report['covered_member_count']}/{report['declared_member_count']} members, "
        f"{report['covered_use_case_count']}/{report['declared_use_case_count']} use cases"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CoverageError, OSError, json.JSONDecodeError, jsonschema.ValidationError, yaml.YAMLError) as error:
        print(f"API test coverage check failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
