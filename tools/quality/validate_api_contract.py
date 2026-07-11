#!/usr/bin/env python3
"""Validate structural and semantic invariants of the public API catalog."""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import pathlib
import re
import sys
from typing import Any

import yaml


API_ID = re.compile(r"API-[A-Z]+-[0-9]{3}$")
ATOMIC_UNIT_ID = re.compile(r"AU-[A-Z]+-[0-9]{3}$")
PHASE = re.compile(r"M[0-9]+$")
HEADER = re.compile(r"<cxxlens/[a-z0-9_/]+\.hpp>$")
FINGERPRINT = re.compile(r"sha256:[0-9a-f]{64}$")
MATURITIES = {"contract-defined", "planned", "experimental", "stable", "deprecated"}


class ContractError(ValueError):
    """A catalog invariant violation with a stable diagnostic."""


def fail(message: str) -> None:
    raise ContractError(message)


def _sequence(value: Any, context: str) -> list:
    if not isinstance(value, list):
        fail(f"{context} must be a sequence")
    return value


def _unique_strings(value: Any, context: str) -> list[str]:
    items = _sequence(value, context)
    if any(not isinstance(item, str) or not item for item in items):
        fail(f"{context} must contain non-empty strings")
    if len(items) != len(set(items)):
        fail(f"{context} contains duplicates")
    return items


def signature_fingerprint(signature: str) -> str:
    return "sha256:" + hashlib.sha256(signature.encode("utf-8")).hexdigest()


def iter_apis(document: dict) -> list[tuple[dict, dict]]:
    return [(package, api) for package in document["packages"] for api in package["apis"]]


def canonical_summary(document: dict) -> dict:
    entries = iter_apis(document)
    phases = collections.Counter(api["phase"] for _, api in entries)
    maturities = collections.Counter(api["contract_maturity"] for _, api in entries)
    kinds = collections.Counter(api["kind"] for _, api in entries)
    states = collections.Counter(api["implementation_state"] for _, api in entries)
    for state in document["registries"]["implementation_states"]:
        states.setdefault(state, 0)
    atomic_units = {api["atomic_unit"]["id"] for _, api in entries}
    graph = {
        api["id"]: sorted(api.get("requires", {}).get("apis", []))
        for _, api in sorted(entries, key=lambda item: item[1]["id"])
    }
    return {
        "package_count": len(document["packages"]),
        "api_entry_count": len(entries),
        "atomic_unit_count": len(atomic_units),
        "phase_counts": dict(sorted(phases.items())),
        "contract_maturity_counts": dict(sorted(maturities.items())),
        "kind_counts": dict(sorted(kinds.items())),
        "implementation_state_counts": dict(sorted(states.items())),
        "dependency_graph": graph,
    }


def render_inventory(document: dict) -> str:
    rows = sorted(
        (
            api["id"],
            package["id"],
            package["header"],
            api["symbol"],
            api["atomic_unit"]["id"],
            api["implementation_state"],
        )
        for package, api in iter_apis(document)
    )
    lines = [
        "# Generated public API inventory",
        "",
        "This file is generated from `schemas/cxxlens_public_api_contract.yaml`; do not edit rows manually.",
        "",
        "| API ID | Package | Header | Symbol | Atomic unit | Implementation state |",
        "|---|---|---|---|---|---|",
    ]
    lines.extend("| " + " | ".join(row) + " |" for row in rows)
    lines.append("")
    return "\n".join(lines)


def _check_no_api_cycles(graph: dict[str, list[str]]) -> None:
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(node: str) -> None:
        if node in visiting:
            fail(f"API dependency cycle includes {node}")
        if node in visited:
            return
        visiting.add(node)
        for dependency in graph[node]:
            visit(dependency)
        visiting.remove(node)
        visited.add(node)

    for node in sorted(graph):
        visit(node)


def validate_document(document: Any, inventory_text: str | None = None) -> dict:
    if not isinstance(document, dict):
        fail("catalog root must be a mapping")
    if document.get("schema") != "cxxlens.api-catalog.v2":
        fail("unexpected catalog schema; v2 is required")
    if document.get("language") != "C++23":
        fail("catalog language must be C++23")

    authority = document.get("authority")
    if not isinstance(authority, dict) or authority.get("machine_readable_source") != (
        "schemas/cxxlens_public_api_contract.yaml"
    ):
        fail("authority must identify the machine-readable catalog")

    registries = document.get("registries")
    if not isinstance(registries, dict):
        fail("registries must be a mapping")
    fact_kinds = set(_unique_strings(registries.get("fact_kinds"), "registries.fact_kinds"))
    capabilities = set(
        _unique_strings(registries.get("capabilities"), "registries.capabilities")
    )
    implementation_states = set(
        _unique_strings(
            registries.get("implementation_states"), "registries.implementation_states"
        )
    )
    readiness_states = set(
        _unique_strings(registries.get("readiness_states"), "registries.readiness_states")
    )
    use_cases = set(_unique_strings(registries.get("use_cases"), "registries.use_cases"))
    requirements = set(
        _unique_strings(registries.get("requirements"), "registries.requirements")
    )
    error_codes = set(_unique_strings(registries.get("error_codes"), "registries.error_codes"))
    expressions: dict[str, list[str]] = {}
    for expression in _sequence(
        registries.get("dependency_expressions"), "registries.dependency_expressions"
    ):
        if not isinstance(expression, dict) or not isinstance(expression.get("id"), str):
            fail("dependency expression must have an id")
        expression_id = expression["id"]
        if expression_id in expressions:
            fail(f"duplicate dependency expression: {expression_id}")
        expansion = _unique_strings(expression.get("expands_to"), f"{expression_id}.expands_to")
        dangling = set(expansion) - fact_kinds
        if dangling:
            fail(f"{expression_id}: dangling expanded fact kinds: {sorted(dangling)}")
        if expression.get("expansion_order") != "lexicographic_unique":
            fail(f"{expression_id}: expansion order must be lexicographic_unique")
        if expansion != sorted(expansion):
            fail(f"{expression_id}: expansion must be in canonical order")
        expressions[expression_id] = sorted(expansion)

    packages = _sequence(document.get("packages"), "packages")
    if not packages:
        fail("packages must be non-empty")
    package_ids: set[str] = set()
    api_ids: set[str] = set()
    atomic_states: dict[str, set[str]] = collections.defaultdict(set)
    graph: dict[str, list[str]] = {}
    required_package = {"id", "header", "purpose", "public_types", "apis"}
    required_api = {
        "id",
        "symbol",
        "kind",
        "phase",
        "contract_maturity",
        "implementation_state",
        "declaration",
        "atomic_unit",
        "readiness",
    }

    for package in packages:
        if not isinstance(package, dict):
            fail("every package must be a mapping")
        missing = required_package - package.keys()
        if missing:
            fail(f"package is missing fields: {sorted(missing)}")
        package_id = package["id"]
        if not isinstance(package_id, str) or package_id in package_ids:
            fail(f"duplicate or invalid package id: {package_id}")
        package_ids.add(package_id)
        if not isinstance(package["header"], str) or not HEADER.fullmatch(package["header"]):
            fail(f"package {package_id}: invalid public header")
        _unique_strings(package["public_types"], f"package {package_id}.public_types")

        for api in _sequence(package["apis"], f"package {package_id}.apis"):
            if not isinstance(api, dict):
                fail(f"package {package_id}: API entry must be a mapping")
            missing = required_api - api.keys()
            if missing:
                fail(f"package {package_id}: API is missing fields: {sorted(missing)}")
            api_id = api["id"]
            if not isinstance(api_id, str) or not API_ID.fullmatch(api_id):
                fail(f"invalid API id: {api_id}")
            if api_id in api_ids:
                fail(f"duplicate API id: {api_id}")
            api_ids.add(api_id)
            if not isinstance(api["symbol"], str) or not api["symbol"].strip():
                fail(f"{api_id}: symbol must be non-empty")
            if not isinstance(api["phase"], str) or not PHASE.fullmatch(api["phase"]):
                fail(f"{api_id}: invalid phase")
            if api["contract_maturity"] not in MATURITIES:
                fail(f"{api_id}: unknown contract maturity")
            if api["implementation_state"] not in implementation_states:
                fail(f"{api_id}: unknown implementation state")

            declaration = api["declaration"]
            if not isinstance(declaration, dict) or declaration.get("status") not in {
                "exact",
                "unresolved",
            }:
                fail(f"{api_id}: invalid declaration status")
            signature = declaration.get("signature")
            declared_fingerprint = declaration.get("signature_fingerprint")
            if declaration["status"] == "exact":
                if not isinstance(signature, str) or not signature.strip():
                    fail(f"{api_id}: exact declaration requires a signature")
                if not isinstance(declared_fingerprint, str) or not FINGERPRINT.fullmatch(
                    declared_fingerprint
                ):
                    fail(f"{api_id}: invalid signature fingerprint")
                if declared_fingerprint != signature_fingerprint(signature):
                    fail(f"{api_id}: signature fingerprint mismatch")
            elif signature is not None or declared_fingerprint is not None:
                fail(f"{api_id}: unresolved declaration cannot carry a signature")

            atomic_unit = api["atomic_unit"]
            if not isinstance(atomic_unit, dict) or not ATOMIC_UNIT_ID.fullmatch(
                str(atomic_unit.get("id", ""))
            ):
                fail(f"{api_id}: invalid atomic unit")
            if atomic_unit.get("indivisible") is not True:
                fail(f"{api_id}: atomic unit must be indivisible")
            readiness = api["readiness"]
            if not isinstance(readiness, dict) or readiness.get("state") not in readiness_states:
                fail(f"{api_id}: invalid readiness state")
            blockers = _unique_strings(readiness.get("blockers"), f"{api_id}.readiness.blockers")
            if readiness["state"] == "ready" and declaration["status"] != "exact":
                fail(f"{api_id}: unresolved declaration cannot be ready")
            if readiness["state"] == "blocked" and not blockers:
                fail(f"{api_id}: blocked readiness requires blockers")
            if readiness["state"] in {"ready", "complete"} and blockers:
                fail(f"{api_id}: non-blocked readiness cannot have blockers")
            atomic_states[atomic_unit["id"]].add(readiness["state"])

            if api["implementation_state"] != "unimplemented":
                evidence = _unique_strings(
                    api.get("implementation_evidence"), f"{api_id}.implementation_evidence"
                )
                if not evidence:
                    fail(f"{api_id}: implementation state requires evidence")

            requires = api.get("requires", {})
            if not isinstance(requires, dict):
                fail(f"{api_id}: requires must be a mapping")
            referenced_facts = set(
                _unique_strings(requires.get("facts", []), f"{api_id}.requires.facts")
            )
            referenced_capabilities = set(
                _unique_strings(
                    requires.get("capabilities", []), f"{api_id}.requires.capabilities"
                )
            )
            referenced_expressions = set(
                _unique_strings(
                    requires.get("expressions", []), f"{api_id}.requires.expressions"
                )
            )
            dependencies = _unique_strings(
                requires.get("apis", []), f"{api_id}.requires.apis"
            )
            if referenced_facts - fact_kinds:
                fail(f"{api_id}: dangling fact references: {sorted(referenced_facts - fact_kinds)}")
            if referenced_capabilities - capabilities:
                fail(
                    f"{api_id}: dangling capability references: "
                    f"{sorted(referenced_capabilities - capabilities)}"
                )
            if referenced_expressions - expressions.keys():
                fail(
                    f"{api_id}: dangling dependency expressions: "
                    f"{sorted(referenced_expressions - expressions.keys())}"
                )
            graph[api_id] = dependencies
            for field in ("use_cases", "requirements", "errors", "guarantees"):
                if field in api:
                    _unique_strings(api[field], f"{api_id}.{field}")
            dangling_use_cases = set(api.get("use_cases", [])) - use_cases
            dangling_requirements = set(api.get("requirements", [])) - requirements
            dangling_errors = set(api.get("errors", [])) - error_codes
            if dangling_use_cases:
                fail(f"{api_id}: dangling use-case references: {sorted(dangling_use_cases)}")
            if dangling_requirements:
                fail(f"{api_id}: dangling requirement references: {sorted(dangling_requirements)}")
            if dangling_errors:
                fail(f"{api_id}: dangling error references: {sorted(dangling_errors)}")
            if readiness["state"] in {"ready", "complete"} and not (
                api.get("use_cases") or api.get("requirements")
            ):
                fail(f"{api_id}: ready or complete API requires traceability")

    for atomic_unit, states in atomic_states.items():
        if len(states) != 1:
            fail(f"{atomic_unit}: an atomic unit cannot have split readiness")
    for api_id, dependencies in graph.items():
        dangling = set(dependencies) - api_ids
        if dangling:
            fail(f"{api_id}: dangling API dependencies: {sorted(dangling)}")
    _check_no_api_cycles(graph)

    summary = document.get("summary")
    calculated = canonical_summary(document)
    if not isinstance(summary, dict):
        fail("summary must be a mapping")
    for key in ("package_count", "api_entry_count", "atomic_unit_count"):
        if summary.get(key) != calculated[key]:
            fail(f"summary.{key} does not match the catalog")
    if summary.get("implementation_state_counts") != calculated["implementation_state_counts"]:
        fail("summary.implementation_state_counts does not match the catalog")
    if calculated["package_count"] != 22 or calculated["api_entry_count"] != 123:
        fail("full catalog regression: expected 22 packages and 123 API entries")
    implemented = sorted(
        api["id"] for _, api in iter_apis(document) if api["implementation_state"] != "unimplemented"
    )
    expected_implemented = [
        "API-CFG-001",
        "API-CFG-002",
        "API-CFG-003",
        "API-CFG-004",
        "API-CORE-001",
        "API-CORE-003",
        "API-CORE-004",
        "API-TEST-001",
        "API-TEST-002",
        "API-TEST-005",
        "API-TEST-006",
        "API-WS-001",
        "API-WS-002",
        "API-WS-003",
        "API-WS-004",
        "API-WS-008",
    ]
    if implemented != expected_implemented:
        fail(f"implementation evidence must match the completed foundation APIs: {expected_implemented}")
    if inventory_text is not None and inventory_text != render_inventory(document):
        fail("chapter 40 generated inventory does not match the YAML catalog")
    return calculated


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("catalog", type=pathlib.Path)
    parser.add_argument("--inventory", type=pathlib.Path)
    parser.add_argument("--summary-json", action="store_true")
    args = parser.parse_args()
    document = yaml.safe_load(args.catalog.read_text(encoding="utf-8"))
    inventory = args.inventory.read_text(encoding="utf-8") if args.inventory else None
    summary = validate_document(document, inventory)
    if args.summary_json:
        print(json.dumps(summary, ensure_ascii=False, sort_keys=True, indent=2))
    else:
        print(
            f"validated {summary['package_count']} packages, {summary['api_entry_count']} APIs, "
            f"and {summary['atomic_unit_count']} atomic units"
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ContractError, yaml.YAMLError) as error:
        print(f"API catalog validation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
