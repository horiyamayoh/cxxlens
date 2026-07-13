#!/usr/bin/env python3
"""Generate and independently validate deterministic API implementation task packets."""

from __future__ import annotations

import argparse
import collections
import copy
import hashlib
import json
import pathlib
import re
import sys
from typing import Any

import jsonschema
import yaml


REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY_ROOT / "tools" / "quality"))

from validate_api_contract import ContractError as CatalogError  # noqa: E402
from validate_api_contract import validate_document  # noqa: E402


PACKET_SCHEMA = "cxxlens.agent-task-packet.v1"
CORPUS_SCHEMA = "cxxlens.agent-task-packet-corpus.v1"
REPORT_SCHEMA = "cxxlens.agent-task-packet-validation-report.v1"
FAMILY_KINDS = {
    "builder_family",
    "builder_operation",
    "free_function_family",
    "method_family",
    "static_factory_family",
}
SHA256 = re.compile(r"sha256:[0-9a-f]{64}$")
SHARED_COMPONENT_KINDS = (
    "package_internal_engine",
    "schema_fixture_contract",
    "shared_public_type",
)


class TaskPacketError(ValueError):
    """A task-packet invariant violation with a stable code."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code


def fail(code: str, message: str) -> None:
    raise TaskPacketError(code, message)


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def pretty_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value).encode("utf-8")).hexdigest()


def file_digest(path: pathlib.Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def canonicalize_catalog_value(value: Any) -> Any:
    """Normalize catalog containers whose order is not semantic."""
    if isinstance(value, dict):
        return {
            key: canonicalize_catalog_value(item)
            for key, item in sorted(value.items())
            if key != "generated_on"
        }
    if isinstance(value, list):
        normalized = [canonicalize_catalog_value(item) for item in value]
        if all(isinstance(item, dict) and isinstance(item.get("id"), str) for item in normalized):
            return sorted(normalized, key=lambda item: item["id"])
        if all(isinstance(item, (str, int, float, bool)) or item is None for item in normalized):
            return sorted(normalized, key=canonical_json)
        return normalized
    return value


def safe_source_path(root: pathlib.Path, relative: str, api_id: str) -> pathlib.Path:
    source = pathlib.PurePosixPath(relative)
    if source.is_absolute() or ".." in source.parts:
        fail("task_packet.unsafe-source", f"{api_id}: unsafe declaration source {relative}")
    path = root.joinpath(*source.parts)
    if not path.is_file():
        fail("task_packet.source-missing", f"{api_id}: declaration source is missing: {relative}")
    return path


def iter_apis(document: dict[str, Any]) -> list[tuple[dict[str, Any], dict[str, Any]]]:
    return sorted(
        (
            (package, api)
            for package in document["packages"]
            for api in package["apis"]
        ),
        key=lambda item: item[1]["id"],
    )


def api_lookup(document: dict[str, Any]) -> dict[str, tuple[dict[str, Any], dict[str, Any]]]:
    return {api["id"]: (package, api) for package, api in iter_apis(document)}


def expression_lookup(document: dict[str, Any]) -> dict[str, list[str]]:
    return {
        expression["id"]: sorted(expression["expands_to"])
        for expression in document["registries"]["dependency_expressions"]
    }


def shared_contract_lookup(document: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        contract["package"]: contract
        for contract in document["registries"]["shared_implementation_contracts"]
    }


def provider_subject_lookup(document: dict[str, Any]) -> dict[tuple[str, str], dict[str, Any]]:
    providers: dict[tuple[str, str], dict[str, Any]] = {}
    for contract in document["registries"]["provider_implementation_contracts"]:
        for subject in contract["subjects"]:
            key = (contract["kind"], subject)
            if key in providers:
                fail("task_packet.provider-ambiguous", f"multiple providers for {subject}")
            providers[key] = contract
    return providers


def implementation_components(
    package: dict[str, Any],
    api: dict[str, Any],
    expressions: dict[str, list[str]],
    shared_contracts: dict[str, dict[str, Any]],
    providers: dict[tuple[str, str], dict[str, Any]],
) -> list[dict[str, Any]]:
    shared = shared_contracts[package["id"]]
    components = [
        {
            "id": f"{shared['id']}.{kind}",
            "kind": kind,
            "subject": package["id"],
            "owner_atomic_unit": shared["owner_atomic_unit"],
            "steward": shared["steward"],
            "reason": {
                "package_internal_engine": "package APIs share one internal semantic engine",
                "schema_fixture_contract": "package schema, golden, and fixture semantics are shared",
                "shared_public_type": "package public value types require one contract owner",
            }[kind],
            "source_contract": shared["source_contract"],
            "required_semantics_version": shared["semantics_version"],
        }
        for kind in SHARED_COMPONENT_KINDS
    ]
    requires = api.get("requires", {})
    fact_subjects = set(requires.get("facts", []))
    for expression_id in requires.get("expressions", []):
        fact_subjects.update(expressions[expression_id])
    subjects_by_kind = {
        "fact_provider_implementation": sorted(fact_subjects),
        "capability_provider_implementation": sorted(requires.get("capabilities", [])),
    }
    for kind, subjects in subjects_by_kind.items():
        for subject in subjects:
            contract = providers[(kind, subject)]
            components.append(
                {
                    "id": f"{contract['id']}.{subject}",
                    "kind": kind,
                    "subject": subject,
                    "owner_atomic_unit": contract["owner_atomic_unit"],
                    "steward": contract["steward"],
                    "reason": f"{subject} requires its declared implementation provider",
                    "source_contract": contract["source_contract"],
                    "required_semantics_version": contract["semantics_version"],
                }
            )
    return sorted(components, key=lambda item: (item["kind"], item["id"]))


def family_surface(api: dict[str, Any]) -> list[str]:
    declaration = api["declaration"]
    if api["kind"] not in FAMILY_KINDS:
        return [declaration["signature"] or api["symbol"]]
    if declaration["signature"]:
        members = [member.strip() for member in declaration["signature"].split(";")]
        return sorted(member for member in members if member)
    match = re.fullmatch(r"(.+::)\{([^{}]+)\}", api["symbol"])
    if match:
        return sorted(f"{match.group(1)}{member.strip()}" for member in match.group(2).split(","))
    return [api["symbol"]]


def generation_state(api: dict[str, Any]) -> str:
    state = api["readiness"]["state"]
    if state not in {"complete", "ready", "blocked"}:
        fail("task_packet.readiness-state", f"{api['id']}: unsupported readiness state {state}")
    return state


def normalized_strings(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    return sorted(value)


def acceptance_commands() -> list[dict[str, Any]]:
    return [
        {
            "id": "configure",
            "argv": ["cmake", "--preset", "dev-clang"],
            "environment": {"CXX": "clang++"},
        },
        {
            "id": "build",
            "argv": ["cmake", "--build", "--preset", "dev-clang"],
            "environment": {},
        },
        {
            "id": "test",
            "argv": ["ctest", "--preset", "dev-clang"],
            "environment": {},
        },
        {
            "id": "quality",
            "argv": [
                "cmake",
                "--build",
                "--preset",
                "dev-clang",
                "--target",
                "cxxlens-quality",
            ],
            "environment": {},
        },
    ]


def fixture_requirements(
    api: dict[str, Any], coverage: dict[str, Any] | None
) -> list[dict[str, Any]]:
    references = {
        "positive": "docs/design/cxxlens_integrated_design_ja.md#36.14-positive",
        "negative": "docs/design/cxxlens_integrated_design_ja.md#36.14-negative",
        "ambiguous": "docs/design/cxxlens_integrated_design_ja.md#36.14-ambiguous",
    }
    cases = {
        fixture["category"]: fixture
        for fixture in (coverage or {}).get("fixtures", [])
    }
    requirements = []
    for category in ("positive", "negative", "ambiguous"):
        fixture = cases.get(category)
        requirements.append(
            {
                "category": category,
                "required": True,
                "contract_reference": references[category],
                "case_ids": [] if fixture is None else [fixture["id"]],
                "test_ids": [] if fixture is None else [fixture["test_id"]],
                "expected_outcomes": []
                if fixture is None
                else [fixture["expected_outcome"]],
                "evidence_candidates": []
                if fixture is None
                else [fixture["evidence_path"]],
            }
        )
    return requirements


def make_packet(
    package: dict[str, Any],
    api: dict[str, Any],
    root: pathlib.Path,
    api_to_unit: dict[str, str],
    expressions: dict[str, list[str]],
    shared_contracts: dict[str, dict[str, Any]],
    providers: dict[tuple[str, str], dict[str, Any]],
    coverage: dict[str, Any] | None,
) -> dict[str, Any]:
    api_id = api["id"]
    declaration = api["declaration"]
    source_fingerprint = file_digest(safe_source_path(root, declaration["source"], api_id))
    contract_fingerprint = digest(
        {
            "signature_fingerprint": declaration["signature_fingerprint"],
            "source_fingerprint": source_fingerprint,
        }
    )
    requires = api.get("requires", {})
    dependency_apis = sorted(requires.get("apis", []))
    dependency_expressions = [
        {"id": expression_id, "expands_to": expressions[expression_id]}
        for expression_id in sorted(requires.get("expressions", []))
    ]
    components = implementation_components(
        package, api, expressions, shared_contracts, providers
    )
    dependency_units = {
        api_to_unit[item] for item in dependency_apis
    } | {component["owner_atomic_unit"] for component in components}
    dependency_units.discard(api["atomic_unit"]["id"])
    packet: dict[str, Any] = {
        "schema": PACKET_SCHEMA,
        "api_id": api_id,
        "atomic_unit_id": api["atomic_unit"]["id"],
        "package": {"id": package["id"], "header": package["header"]},
        "symbol": api["symbol"],
        "kind": api["kind"],
        "phase": api["phase"],
        "contract_maturity": api["contract_maturity"],
        "implementation_state": api["implementation_state"],
        "declaration": {
            "status": declaration["status"],
            "source": declaration["source"],
            "signature": declaration["signature"],
            "signature_fingerprint": declaration["signature_fingerprint"],
            "source_fingerprint": source_fingerprint,
            "contract_fingerprint": contract_fingerprint,
        },
        "family_contract": {
            "mode": "coherent_family" if api["kind"] in FAMILY_KINDS else "single_operation",
            "indivisible": True,
            "surface_members": family_surface(api),
        },
        "dependencies": {
            "facts": sorted(requires.get("facts", [])),
            "capabilities": sorted(requires.get("capabilities", [])),
            "expressions": dependency_expressions,
            "minimum_precision": requires.get("minimum_precision"),
            "apis": dependency_apis,
            "atomic_units": sorted(dependency_units),
            "components": components,
        },
        "traceability": {
            "use_cases": sorted(api.get("use_cases", [])),
            "requirements": sorted(api.get("requirements", [])),
        },
        "behavior": {
            "errors": normalized_strings(api.get("errors")),
            "guarantees": normalized_strings(api.get("guarantees")),
            "side_effects": normalized_strings(api.get("side_effects")),
        },
        "implementation_evidence": sorted(api.get("implementation_evidence", [])),
        "quality_obligations": {
            "evidence": {
                "required": True,
                "contract_reference": "docs/design/cxxlens_integrated_design_ja.md#39.10-evidence",
            },
            "coverage": {
                "required": True,
                "contract_reference": "docs/design/cxxlens_integrated_design_ja.md#39.10-coverage",
            },
            "unresolved": {
                "required": True,
                "contract_reference": "docs/design/cxxlens_integrated_design_ja.md#32.1-unresolved",
            },
            "invariant_schema": {
                "required": True,
                "contract_reference": "docs/design/cxxlens_integrated_design_ja.md#39.12-schema",
            },
        },
        "fixtures": fixture_requirements(api, coverage),
        "acceptance_commands": acceptance_commands(),
        "coordination": {
            "ownership_contract": "cxxlens.agent-ownership.v1",
            "ownership_provider_issue": "#28",
            "shared_contract_steward_refs": [shared_contracts[package["id"]]["steward"]],
        },
        "generation": {
            "state": generation_state(api),
            "block_reasons": sorted(api["readiness"]["blockers"]),
        },
    }
    packet["semantic_digest"] = digest(packet)
    return packet


def make_atomic_units(packets: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
    for packet in packets:
        grouped[packet["atomic_unit_id"]].append(packet)
    units = []
    for unit_id, members in sorted(grouped.items()):
        states = {member["generation"]["state"] for member in members}
        if len(states) != 1:
            fail("task_packet.split-readiness", f"{unit_id}: member packets have split readiness")
        surface_members = sorted(
            f"{member['api_id']}::{surface}"
            for member in members
            for surface in member["family_contract"]["surface_members"]
        )
        units.append(
            {
                "id": unit_id,
                "indivisible": True,
                "member_api_ids": sorted(member["api_id"] for member in members),
                "family_surface_members": surface_members,
                "readiness_state": next(iter(states)),
                "generation_state": next(iter(states)),
            }
        )
    return units


def generate_corpus(
    document: dict[str, Any],
    root: pathlib.Path,
    coverage_document: dict[str, Any] | None = None,
) -> dict[str, Any]:
    try:
        validate_document(document)
    except CatalogError as error:
        fail("task_packet.catalog-invalid", str(error))
    entries = iter_apis(document)
    api_to_unit = {api["id"]: api["atomic_unit"]["id"] for _, api in entries}
    expressions = expression_lookup(document)
    shared_contracts = shared_contract_lookup(document)
    providers = provider_subject_lookup(document)
    if coverage_document is None:
        coverage_document = yaml.safe_load(
            (REPOSITORY_ROOT / "schemas/cxxlens_api_test_coverage.yaml").read_text(
                encoding="utf-8"
            )
        )
    coverage_by_api = {
        row["api_id"]: row for row in coverage_document.get("apis", [])
    }
    packets = [
        make_packet(
            package,
            api,
            root,
            api_to_unit,
            expressions,
            shared_contracts,
            providers,
            coverage_by_api.get(api["id"]),
        )
        for package, api in entries
    ]
    units = make_atomic_units(packets)
    state_counts = collections.Counter(packet["generation"]["state"] for packet in packets)
    declaration_counts = collections.Counter(
        packet["declaration"]["status"] for packet in packets
    )
    blocker_counts = collections.Counter(
        blocker for packet in packets for blocker in packet["generation"]["block_reasons"]
    )
    corpus: dict[str, Any] = {
        "schema": CORPUS_SCHEMA,
        "catalog_fingerprint": digest(canonicalize_catalog_value(document)),
        "summary": {
            "package_count": len(document["packages"]),
            "api_count": len(packets),
            "atomic_unit_count": len(units),
            "generation_state_counts": {
                state: state_counts.get(state, 0) for state in ("blocked", "complete", "ready")
            },
            "declaration_state_counts": {
                state: declaration_counts.get(state, 0) for state in ("exact", "unresolved")
            },
            "blocker_counts": dict(sorted(blocker_counts.items())),
        },
        "atomic_units": units,
        "packets": packets,
    }
    corpus["semantic_digest"] = digest(corpus)
    return corpus


def make_report(corpus: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema": REPORT_SCHEMA,
        "status": "passed",
        "catalog_fingerprint": corpus["catalog_fingerprint"],
        "corpus_semantic_digest": corpus["semantic_digest"],
        "api_count": corpus["summary"]["api_count"],
        "atomic_unit_count": corpus["summary"]["atomic_unit_count"],
        "generation_state_counts": corpus["summary"]["generation_state_counts"],
        "blocker_counts": corpus["summary"]["blocker_counts"],
        "checks": [
            "atomic-unit-exactly-once",
            "catalog-and-source-fingerprint",
            "family-indivisibility",
            "root-order-process-determinism",
            "schema-and-semantic-digest",
            "shared-implementation-dependency-closure",
            "typed-dependency-resolution",
        ],
    }


def _check_cycle(graph: dict[str, list[str]]) -> None:
    visiting: list[str] = []
    visited: set[str] = set()

    def visit(node: str) -> None:
        if node in visiting:
            cycle = visiting[visiting.index(node) :] + [node]
            fail("task_packet.dependency-cycle", " -> ".join(cycle))
        if node in visited:
            return
        visiting.append(node)
        for dependency in graph[node]:
            visit(dependency)
        visiting.pop()
        visited.add(node)

    for node in sorted(graph):
        visit(node)


def _validate_schema(value: Any, schema: dict[str, Any], context: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(value)
    except jsonschema.SchemaError as error:
        fail("task_packet.schema-definition-invalid", f"{context}: {error.message}")
    except jsonschema.ValidationError as error:
        path = ".".join(str(item) for item in error.absolute_path) or "<root>"
        fail("task_packet.schema-invalid", f"{context} at {path}: {error.message}")


def validate_corpus(
    corpus: dict[str, Any],
    document: dict[str, Any],
    root: pathlib.Path,
    schema: dict[str, Any] | None = None,
) -> dict[str, Any]:
    if not isinstance(corpus, dict) or corpus.get("schema") != CORPUS_SCHEMA:
        fail("task_packet.unknown-schema", "task packet corpus schema is not supported")
    catalog = api_lookup(document)
    packets = corpus.get("packets")
    if not isinstance(packets, list):
        fail("task_packet.schema-invalid", "packets must be an array")
    seen: set[str] = set()
    packet_by_api: dict[str, dict[str, Any]] = {}
    known_facts = set(document["registries"]["fact_kinds"])
    known_capabilities = set(document["registries"]["capabilities"])
    known_expressions = expression_lookup(document)
    shared_contracts = shared_contract_lookup(document)
    provider_contracts = provider_subject_lookup(document)
    known_units = {api["atomic_unit"]["id"] for _, api in iter_apis(document)}
    expected_api_ids = set(catalog)

    for packet in packets:
        if not isinstance(packet, dict) or packet.get("schema") != PACKET_SCHEMA:
            fail("task_packet.unknown-schema", "packet schema is not supported")
        api_id = packet.get("api_id")
        if api_id in seen:
            fail("task_packet.duplicate-membership", f"{api_id}: duplicate packet")
        if api_id not in catalog:
            fail("task_packet.unknown-api", f"unknown API packet: {api_id}")
        seen.add(api_id)
        packet_by_api[api_id] = packet
        package, api = catalog[api_id]
        family = api["kind"] in FAMILY_KINDS
        if packet.get("atomic_unit_id") != api["atomic_unit"]["id"]:
            code = "task_packet.family-split" if family else "task_packet.atomic-unit-drift"
            fail(code, f"{api_id}: atomic unit differs from the catalog")
        steward_refs = packet.get("coordination", {}).get("shared_contract_steward_refs", [])
        if (
            not isinstance(steward_refs, list)
            or len(steward_refs) > 1
            or len(steward_refs) != len(set(steward_refs))
        ):
            fail("task_packet.shared-steward-ambiguous", f"{api_id}: multiple shared stewards")
        expected_stewards = [shared_contracts[package["id"]]["steward"]]
        if steward_refs != expected_stewards:
            fail(
                "task_packet.shared-steward-drift",
                f"{api_id}: shared steward must be derived from the catalog",
            )
        state = packet.get("generation", {}).get("state")
        expected_state = generation_state(api)
        if state != expected_state:
            if state == "ready" and api["readiness"]["state"] != "ready":
                fail(
                    "task_packet.maturity-not-readiness",
                    f"{api_id}: contract maturity cannot imply readiness",
                )
            fail("task_packet.readiness-drift", f"{api_id}: generation state differs")
        fixtures = packet.get("fixtures", [])
        categories = [fixture.get("category") for fixture in fixtures]
        if sorted(categories) != ["ambiguous", "negative", "positive"] or len(
            set(categories)
        ) != 3:
            fail("task_packet.fixture-category-missing", f"{api_id}: fixture categories differ")
        if state in {"complete", "ready"}:
            for fixture in fixtures:
                for field in (
                    "case_ids",
                    "test_ids",
                    "expected_outcomes",
                    "evidence_candidates",
                ):
                    if not fixture.get(field):
                        fail(
                            "task_packet.fixture-evidence-missing",
                            f"{api_id}: {fixture['category']} lacks concrete {field}",
                        )
            if len({tuple(fixture["case_ids"]) for fixture in fixtures}) != 3:
                fail(
                    "task_packet.fixture-evidence-duplicated",
                    f"{api_id}: category-specific fixture cases are duplicated",
                )
        declaration = packet.get("declaration", {})
        expected_declaration = api["declaration"]
        if state == "ready" and expected_declaration["status"] != "exact":
            fail("task_packet.unresolved-ready", f"{api_id}: unresolved declaration is ready")
        if (
            declaration.get("status") != expected_declaration["status"]
            or declaration.get("signature") != expected_declaration["signature"]
            or declaration.get("signature_fingerprint")
            != expected_declaration["signature_fingerprint"]
        ):
            fail("task_packet.signature-drift", f"{api_id}: declaration fingerprint differs")
        if packet.get("family_contract", {}).get("surface_members") != family_surface(api):
            fail("task_packet.family-split", f"{api_id}: coherent family surface was split")
        source_path = safe_source_path(root, expected_declaration["source"], api_id)
        if declaration.get("source_fingerprint") != file_digest(source_path):
            fail("task_packet.source-drift", f"{api_id}: frozen declaration source changed")
        dependencies = packet.get("dependencies", {})
        components = dependencies.get("components", [])
        if not isinstance(components, list):
            fail("task_packet.schema-invalid", f"{api_id}: components must be an array")
        component_ids = [component.get("id") for component in components if isinstance(component, dict)]
        if len(component_ids) != len(components) or len(component_ids) != len(set(component_ids)):
            fail(
                "task_packet.shared-dependency-ambiguous",
                f"{api_id}: component dependencies are ambiguous",
            )
        expected_components = implementation_components(
            package, api, known_expressions, shared_contracts, provider_contracts
        )
        expected_component_ids = {component["id"] for component in expected_components}
        actual_component_ids = set(component_ids)
        if expected_component_ids - actual_component_ids:
            fail(
                "task_packet.shared-dependency-undeclared",
                f"{api_id}: missing shared dependencies "
                f"{sorted(expected_component_ids - actual_component_ids)}",
            )
        if components != expected_components:
            fail(
                "task_packet.shared-dependency-drift",
                f"{api_id}: shared dependency contract differs from the catalog",
            )
        dependency_apis = dependencies.get("apis", [])
        dangling_apis = set(dependency_apis) - expected_api_ids
        if dangling_apis:
            fail(
                "task_packet.dangling-dependency",
                f"{api_id}: dangling API dependencies {sorted(dangling_apis)}",
            )
        dangling_units = set(dependencies.get("atomic_units", [])) - known_units
        if dangling_units:
            fail(
                "task_packet.dangling-dependency",
                f"{api_id}: dangling atomic units {sorted(dangling_units)}",
            )
        expected_units = {
            catalog[dependency][1]["atomic_unit"]["id"] for dependency in dependency_apis
        } | {component["owner_atomic_unit"] for component in components}
        expected_units.discard(api["atomic_unit"]["id"])
        if dependencies.get("atomic_units") != sorted(expected_units):
            fail(
                "task_packet.shared-dependency-drift",
                f"{api_id}: atomic-unit dependency closure differs",
            )
        if set(dependencies.get("facts", [])) - known_facts:
            fail("task_packet.dangling-dependency", f"{api_id}: dangling fact dependency")
        if set(dependencies.get("capabilities", [])) - known_capabilities:
            fail("task_packet.dangling-dependency", f"{api_id}: dangling capability dependency")
        for expression in dependencies.get("expressions", []):
            expression_id = expression.get("id")
            if expression_id not in known_expressions:
                fail("task_packet.dangling-dependency", f"{api_id}: dangling expression")
            if expression.get("expands_to") != known_expressions[expression_id]:
                fail("task_packet.expression-drift", f"{api_id}: expression expansion differs")
        packet_without_digest = copy.deepcopy(packet)
        packet_digest = packet_without_digest.pop("semantic_digest", None)
        if packet_digest != digest(packet_without_digest):
            fail("task_packet.digest-mismatch", f"{api_id}: semantic digest mismatch")

    missing = expected_api_ids - seen
    if missing:
        fail("task_packet.omitted-api", f"catalog APIs omitted: {sorted(missing)}")

    memberships: collections.Counter[str] = collections.Counter()
    membership_units: dict[str, list[str]] = collections.defaultdict(list)
    units = corpus.get("atomic_units")
    if not isinstance(units, list):
        fail("task_packet.schema-invalid", "atomic_units must be an array")
    unit_ids: set[str] = set()
    for unit in units:
        unit_id = unit.get("id") if isinstance(unit, dict) else None
        if unit_id in unit_ids:
            fail("task_packet.duplicate-membership", f"duplicate atomic unit {unit_id}")
        unit_ids.add(unit_id)
        for api_id in unit.get("member_api_ids", []):
            memberships[api_id] += 1
            membership_units[api_id].append(unit_id)
            if api_id not in packet_by_api:
                fail("task_packet.unknown-api", f"{unit_id}: unknown member {api_id}")
    invalid_memberships = sorted(
        api_id for api_id in expected_api_ids if memberships[api_id] != 1
    )
    if invalid_memberships:
        fail(
            "task_packet.duplicate-membership",
            f"APIs must belong to exactly one unit: {invalid_memberships}",
        )
    for api_id, member_units in membership_units.items():
        if packet_by_api[api_id]["atomic_unit_id"] != member_units[0]:
            fail("task_packet.family-split", f"{api_id}: membership unit differs")

    graph_sets: dict[str, set[str]] = collections.defaultdict(set)
    for packet in packets:
        unit_id = packet["atomic_unit_id"]
        graph_sets[unit_id].update(
            dependency
            for dependency in packet["dependencies"]["atomic_units"]
            if dependency != unit_id
        )
    graph = {unit_id: sorted(graph_sets[unit_id]) for unit_id in sorted(known_units)}
    _check_cycle(graph)
    corpus_without_digest = copy.deepcopy(corpus)
    corpus_digest = corpus_without_digest.pop("semantic_digest", None)
    if corpus_digest != digest(corpus_without_digest):
        fail("task_packet.digest-mismatch", "corpus semantic digest mismatch")
    if schema is not None:
        _validate_schema(corpus, schema, "task packet corpus")
    expected = generate_corpus(document, root)
    if corpus != expected:
        fail("task_packet.stale-corpus", "generated task packet corpus is stale")
    return make_report(corpus)


def load_yaml(path: pathlib.Path) -> Any:
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def resolve(root: pathlib.Path, path: pathlib.Path) -> pathlib.Path:
    return path if path.is_absolute() else root / path


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("generate", "check"))
    parser.add_argument("--root", type=pathlib.Path, default=REPOSITORY_ROOT)
    parser.add_argument(
        "--catalog",
        type=pathlib.Path,
        default=pathlib.Path("schemas/cxxlens_public_api_contract.yaml"),
    )
    parser.add_argument(
        "--schema",
        type=pathlib.Path,
        default=pathlib.Path("schemas/cxxlens.agent-task-packet.v1.schema.yaml"),
    )
    parser.add_argument(
        "--corpus",
        type=pathlib.Path,
        default=pathlib.Path("schemas/cxxlens.agent-task-packet-corpus.v1.json"),
    )
    parser.add_argument(
        "--report-schema",
        type=pathlib.Path,
        default=pathlib.Path(
            "schemas/cxxlens.agent-task-packet-validation-report.v1.schema.yaml"
        ),
    )
    parser.add_argument(
        "--report",
        type=pathlib.Path,
        default=pathlib.Path("schemas/cxxlens.agent-task-packet-validation-report.v1.json"),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    root = args.root.resolve()
    catalog_path = resolve(root, args.catalog)
    schema_path = resolve(root, args.schema)
    corpus_path = resolve(root, args.corpus)
    report_schema_path = resolve(root, args.report_schema)
    report_path = resolve(root, args.report)
    document = load_yaml(catalog_path)
    schema = load_yaml(schema_path)
    report_schema = load_yaml(report_schema_path)
    generated = generate_corpus(document, root)
    generated_report = make_report(generated)
    _validate_schema(generated, schema, "generated task packet corpus")
    _validate_schema(generated_report, report_schema, "generated validation report")
    if args.mode == "generate":
        corpus_path.write_text(pretty_json(generated), encoding="utf-8")
        report_path.write_text(pretty_json(generated_report), encoding="utf-8")
    else:
        corpus = json.loads(corpus_path.read_text(encoding="utf-8"))
        report = json.loads(report_path.read_text(encoding="utf-8"))
        expected_report = validate_corpus(corpus, document, root, schema)
        _validate_schema(report, report_schema, "validation report")
        if report != expected_report:
            fail("task_packet.stale-report", "task packet validation report is stale")
    print(
        f"task packet corpus {args.mode} passed: "
        f"{generated['summary']['api_count']} APIs, "
        f"{generated['summary']['atomic_unit_count']} atomic units, "
        f"digest {generated['semantic_digest']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        json.JSONDecodeError,
        OSError,
        TaskPacketError,
        yaml.YAMLError,
    ) as error:
        print(f"task packet generation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
