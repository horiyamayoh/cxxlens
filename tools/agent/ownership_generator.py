#!/usr/bin/env python3
"""Generate and validate ownership, frozen skeleton, and dependency-request contracts."""

from __future__ import annotations

import argparse
import collections
import copy
import hashlib
import json
import pathlib
import subprocess
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
OWNERSHIP_SCHEMA = "cxxlens.agent-ownership.v1"
REQUEST_SCHEMA = "cxxlens.dependency-request.v1"


class OwnershipError(ValueError):
    """An ownership invariant violation with a stable code."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code


def fail(code: str, message: str) -> None:
    raise OwnershipError(code, message)


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def pretty_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value).encode("utf-8")).hexdigest()


ROLE_DEFINITIONS: dict[str, tuple[str, str, str]] = {
    "generator.catalog": (
        "generator",
        "#2",
        "API catalog, inventory, identifiers, and typed dependency authority.",
    ),
    "integration.global": (
        "package_integration",
        "#26",
        "Global CMake, CI, install, Doxygen, and umbrella integration.",
    ),
    "steward.repository": (
        "shared_steward",
        "#28",
        "Repository-wide policy files not delegated to another steward.",
    ),
    "steward.design": (
        "shared_steward",
        "#1",
        "Integrated design and accepted design-package history.",
    ),
    "steward.documentation": (
        "shared_steward",
        "#28",
        "Contributor and agent workflow documentation.",
    ),
    "steward.quality": (
        "shared_steward",
        "#12",
        "Global validators, quality fixtures, and conformance tooling.",
    ),
    "steward.schema": (
        "shared_steward",
        "#9",
        "Shared schema registry, compatibility, and canonical serialization contracts.",
    ),
    "steward.runtime": (
        "shared_steward",
        "#3",
        "Filesystem, process, time, hash, and runtime fault ports.",
    ),
    "steward.source": (
        "shared_steward",
        "#4",
        "Source point, span, macro origin, and source schema contracts.",
    ),
    "steward.identity": (
        "shared_steward",
        "#5",
        "Canonical encoding, typed identity, and collision policy.",
    ),
    "steward.failure": (
        "shared_steward",
        "#6",
        "Error, result, unresolved, and stable failure taxonomy.",
    ),
    "steward.evidence": (
        "shared_steward",
        "#7",
        "Evidence, coverage accounting, and result guarantees.",
    ),
    "steward.finding": (
        "shared_steward",
        "#8",
        "Diagnostic, finding identity, explanation, and canonical ordering.",
    ),
    "steward.configuration": (
        "shared_steward",
        "#10",
        "Typed configuration loading, precedence, validation, and explanation.",
    ),
    "steward.testing": (
        "shared_steward",
        "#11",
        "Fixture builder, assertions, property, schema, and golden substrate.",
    ),
    "steward.workspace": (
        "shared_steward",
        "#13",
        "Workspace catalog, compile commands, variants, and snapshot identity.",
    ),
    "steward.facts": (
        "shared_steward",
        "#14",
        "Observation and immutable fact public contracts.",
    ),
    "steward.clang-adapter": (
        "shared_steward",
        "#15",
        "Exact Clang adapter boundary, frontend lifetime, and link closure.",
    ),
    "steward.scheduler": (
        "shared_steward",
        "#16",
        "Deterministic scheduler, cancellation, and partial failure state machine.",
    ),
    "steward.preprocessor": (
        "shared_steward",
        "#17",
        "Source, macro, include observation extraction.",
    ),
    "steward.semantic-extractor": (
        "shared_steward",
        "#18",
        "Symbol, type, call, inheritance, and override extraction.",
    ),
    "steward.reducer": (
        "shared_steward",
        "#19",
        "Cross-TU and multi-variant fact reduction and conflict semantics.",
    ),
    "steward.store": (
        "shared_steward",
        "#20",
        "Fact-store port, snapshot transaction, binary codec, memory, and SQLite backends.",
    ),
    "steward.provisioning": (
        "shared_steward",
        "#21",
        "Fact coverage and incremental provisioning state machine.",
    ),
    "steward.selector": (
        "shared_steward",
        "#23",
        "Immutable selector syntax, validation, requirements, and serialization.",
    ),
    "steward.query": (
        "shared_steward",
        "#24",
        "Query planning, execution, refinement, and virtual candidate resolution.",
    ),
    "steward.search": (
        "shared_steward",
        "#25",
        "Search reports, explanation projections, and flagship result invariants.",
    ),
    "steward.task-packet": (
        "generator",
        "#27",
        "Task-packet schemas, corpus, atomic-unit mapping, and validation report.",
    ),
    "steward.ownership": (
        "generator",
        "#28",
        "Ownership, frozen skeleton, dependency request, and diff-audit contracts.",
    ),
    "steward.runner": (
        "generator",
        "#29",
        "Ready DAG, prompt resolution, API shard generation, and agent runner contracts.",
    ),
    "steward.readiness": (
        "generator",
        "#30",
        "Independent readiness audit, authorization, rollback, and re-audit contracts.",
    ),
    "steward.ng-authority": (
        "shared_steward",
        "#57",
        "Next-generation authority transition, baseline, ADR, and fail-closed gate.",
    ),
    "steward.ng-documentation": (
        "shared_steward",
        "#58",
        "Next-generation documentation, catalog bootstrap, migration ledger, and drift gate.",
    ),
    "steward.ng-release": (
        "shared_steward",
        "#59",
        "Next-generation product boundary, release profile, compatibility bundle, and decision tooling.",
    ),
    "steward.ng-relations": (
        "shared_steward",
        "#60",
        "NG0 exact relation IDL, system claim envelope, evolution vectors, and conformance tooling.",
    ),
    "steward.ng-query": (
        "shared_steward",
        "#61",
        "Logical Query IR algebra, ordering, partiality, normalization, and backend reference conformance.",
    ),
}


NG_AUTHORITY_PATHS = {
    "docs/design/adr/0002-semantic-relation-platform.md",
    "docs/design/adr/0003-versioned-relation-kernel.md",
    "docs/design/adr/0004-legacy-contract-reset.md",
    "docs/design/cxxlens_next_generation_integrated_design_ja.md",
    "schemas/cxxlens_legacy_api_baseline.schema.yaml",
    "schemas/cxxlens_legacy_api_baseline.yaml",
    "schemas/cxxlens_ng_authority_transition.schema.yaml",
    "schemas/cxxlens_ng_authority_transition.yaml",
    "schemas/cxxlens_ng_authority_transition_report.schema.yaml",
    "tests/quality/test_ng_authority.py",
    "tools/quality/check_ng_authority.py",
}


NG_DOCUMENTATION_PATHS = {
    "docs/README.md",
    "docs/archive/README.md",
    "docs/design/README.md",
    "docs/design/adr/README.md",
    "docs/design/catalogs/README.md",
    "docs/doxygen-main.md",
    "docs/support-matrix.md",
    "docs/tutorials/README.md",
    "schemas/cxxlens_asset_migration_ledger.schema.yaml",
    "schemas/cxxlens_asset_migration_policy.schema.yaml",
    "schemas/cxxlens_asset_migration_policy.yaml",
    "schemas/cxxlens_ng_acceptance_manifest.yaml",
    "schemas/cxxlens_ng_catalog_bootstrap.schema.yaml",
    "schemas/cxxlens_ng_provider_protocol.yaml",
    "schemas/cxxlens_ng_public_api_catalog.yaml",
    "schemas/cxxlens_ng_security_profile.yaml",
    "tests/quality/test_documentation_consistency.py",
    "tools/quality/check_documentation_consistency.py",
}


NG_RELEASE_PATHS = {
    "docs/design/adr/0005-product-boundary-release-compatibility.md",
    "schemas/cxxlens_ng_compatibility_report.schema.yaml",
    "schemas/cxxlens_ng_compatibility_request.schema.yaml",
    "schemas/cxxlens_ng_release_bundle.schema.yaml",
    "schemas/cxxlens_ng_release_bundle.yaml",
    "tests/quality/test_ng_release_contract.py",
    "tools/quality/check_ng_release_contract.py",
}


NG_RELATION_PATHS = {
    "docs/design/adr/0006-ng0-relation-and-claim-envelope.md",
    "schemas/cxxlens_ng_claim_envelope.schema.yaml",
    "schemas/cxxlens_ng_relation_conformance_report.schema.yaml",
    "schemas/cxxlens_ng_relation_conformance_vectors.schema.yaml",
    "schemas/cxxlens_ng_relation_conformance_vectors.yaml",
    "schemas/cxxlens_ng_relation_registry.schema.yaml",
    "schemas/cxxlens_ng_relation_registry.yaml",
    "tests/quality/test_ng_relation_contract.py",
    "tools/quality/check_ng_relation_contract.py",
}


NG_QUERY_PATHS = {
    "docs/design/adr/0007-logical-query-algebra.md",
    "schemas/cxxlens_ng_logical_query_contract.schema.yaml",
    "schemas/cxxlens_ng_logical_query_contract.yaml",
    "schemas/cxxlens_ng_logical_query_ir.schema.yaml",
    "schemas/cxxlens_ng_query_conformance_report.schema.yaml",
    "schemas/cxxlens_ng_query_conformance_vectors.schema.yaml",
    "schemas/cxxlens_ng_query_conformance_vectors.yaml",
    "tests/quality/test_ng_query_contract.py",
    "tools/quality/check_ng_query_contract.py",
}


GENERATED_PATHS = {
    "docs/design/api_catalog_inventory.md": "generator.catalog",
    "schemas/cxxlens_contract_ownership.yaml": "generator.catalog",
    "schemas/cxxlens.agent-task-packet-corpus.v1.json": "steward.task-packet",
    "schemas/cxxlens.agent-task-packet-validation-report.v1.json": "steward.task-packet",
    "schemas/cxxlens.agent-ownership.v1.json": "steward.ownership",
    "schemas/cxxlens.dependency-request.examples.v1.json": "steward.ownership",
    "schemas/cxxlens_asset_migration_ledger.json": "steward.ng-documentation",
}

RESERVED_PATHS = [
    {
        "prefix": ".github/workflows/api-unit",
        "owner_role": "steward.runner",
        "access": "exclusive_write",
        "purpose": "API and atomic-unit reusable CI workflows owned by issue #29.",
    },
    {
        "prefix": "docs/agent_runner",
        "owner_role": "steward.runner",
        "access": "exclusive_write",
        "purpose": "Agent runner operational documentation owned by issue #29.",
    },
    {
        "prefix": "docs/readiness_audit",
        "owner_role": "steward.readiness",
        "access": "exclusive_write",
        "purpose": "Readiness authorization documentation owned by issue #30.",
    },
    {
        "prefix": "schemas/cxxlens.api-ready.",
        "owner_role": "steward.runner",
        "access": "exclusive_write",
        "purpose": "Ready DAG, shard, and run manifests owned by issue #29.",
    },
    {
        "prefix": "schemas/cxxlens.readiness.",
        "owner_role": "steward.readiness",
        "access": "exclusive_write",
        "purpose": "Readiness audit and authorization manifests owned by issue #30.",
    },
    {
        "prefix": "tests/agent/readiness/",
        "owner_role": "steward.readiness",
        "access": "exclusive_write",
        "purpose": "Readiness corruption and authorization fixtures owned by issue #30.",
    },
    {
        "prefix": "tests/agent/runner/",
        "owner_role": "steward.runner",
        "access": "exclusive_write",
        "purpose": "Ready evaluator, prompt, shard, and runner fixtures owned by issue #29.",
    },
    {
        "prefix": "tools/agent/api_task",
        "owner_role": "steward.runner",
        "access": "exclusive_write",
        "purpose": "API task prompt, preflight, and runner tooling owned by issue #29.",
    },
    {
        "prefix": "tools/agent/unit_local",
        "owner_role": "steward.runner",
        "access": "exclusive_write",
        "purpose": "Atomic-unit evidence-to-CTest resolution owned by issue #29.",
    },
    {
        "prefix": "tools/agent/readiness_audit",
        "owner_role": "steward.readiness",
        "access": "exclusive_write",
        "purpose": "Independent readiness audit tooling owned by issue #30.",
    },
    {
        "prefix": "tools/agent/ready_evaluator",
        "owner_role": "steward.runner",
        "access": "exclusive_write",
        "purpose": "Dependency DAG and ready predicate tooling owned by issue #29.",
    },
]


def reserved_policy(path: str) -> dict[str, str] | None:
    matches = [item for item in RESERVED_PATHS if path.startswith(item["prefix"])]
    if len(matches) > 1:
        fail("ownership.overlap", f"{path}: matches multiple reserved ownership prefixes")
    return matches[0] if matches else None


def repository_paths(root: pathlib.Path) -> list[str]:
    completed = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        check=True,
        capture_output=True,
    )
    paths = [path for path in completed.stdout.decode("utf-8").split("\0") if path]
    if len(paths) != len(set(paths)):
        fail("ownership.duplicate-path", "git path inventory contains duplicates")
    return sorted(paths)


def package_ids(corpus: dict[str, Any]) -> set[str]:
    return {packet["package"]["id"] for packet in corpus["packets"]}


def classify_domain(path: str) -> str:
    lowered = path.lower()
    ordered_markers = [
        (("canonical_encoding", "identity"), "steward.identity"),
        (("failure", "error.schema", "unresolved"), "steward.failure"),
        (("evidence", "coverage"), "steward.evidence"),
        (("finding", "diagnostic"), "steward.finding"),
        (("configuration", "config/"), "steward.configuration"),
        (("scheduler",), "steward.scheduler"),
        (("provision", "workspace_doctor", "coverage_delta"), "steward.provisioning"),
        (("reducer",), "steward.reducer"),
        (("store", "sqlite", "binary_codec"), "steward.store"),
        (("preprocessor", "macro", "include_relation"), "steward.preprocessor"),
        (("semantic_extractor", "symbol_type", "call_relation"), "steward.semantic-extractor"),
        (("clang22", "frontend", "interop"), "steward.clang-adapter"),
        (("selector", "select/"), "steward.selector"),
        (("query", "virtual_candidate"), "steward.query"),
        (("search", "explain/"), "steward.search"),
        (("workspace", "compile_command", "analysis_scope"), "steward.workspace"),
        (("fact", "observation"), "steward.facts"),
        (("source",), "steward.source"),
        (("testing", "fixture"), "steward.testing"),
        (("runtime",), "steward.runtime"),
    ]
    for markers, role in ordered_markers:
        if any(marker in lowered for marker in markers):
            return role
    return "steward.quality"


def owner_for_path(
    path: str, packages: set[str], units: list[dict[str, Any]]
) -> str:
    reserved = reserved_policy(path)
    if reserved is not None:
        return reserved["owner_role"]
    unit_owners = [
        unit["unit_owner_role"]
        for unit in units
        if any(path.startswith(prefix) for prefix in unit["allowed_write_prefixes"])
    ]
    if len(unit_owners) > 1:
        fail("ownership.overlap", f"{path}: matches multiple atomic units")
    if unit_owners:
        return unit_owners[0]
    if path in GENERATED_PATHS:
        return GENERATED_PATHS[path]
    if path in NG_AUTHORITY_PATHS:
        return "steward.ng-authority"
    if path in NG_DOCUMENTATION_PATHS:
        return "steward.ng-documentation"
    if path in NG_RELEASE_PATHS:
        return "steward.ng-release"
    if path in NG_RELATION_PATHS:
        return "steward.ng-relations"
    if path in NG_QUERY_PATHS:
        return "steward.ng-query"
    if path.startswith(("tools/agent/ownership_", "tests/agent/ownership/")) or path in {
        "docs/agent_ownership_reference.md",
        "schemas/cxxlens.agent-ownership.v1.schema.yaml",
        "schemas/cxxlens.dependency-request.v1.schema.yaml",
    }:
        return "steward.ownership"
    if path.startswith(("tools/agent/task_packet_", "tests/agent/task_packet/")) or path in {
        "docs/agent_task_packet_reference.md",
        "schemas/cxxlens.agent-task-packet.v1.schema.yaml",
        "schemas/cxxlens.agent-task-packet-validation-report.v1.schema.yaml",
    }:
        return "steward.task-packet"
    if path in {
        "schemas/cxxlens_public_api_contract.yaml",
        "schemas/cxxlens_api_catalog.schema.yaml",
        "docs/design/api_catalog_change_policy.md",
        "tools/quality/generate_api_catalog_inventory.py",
        "tools/quality/migrate_api_contract_v2.py",
        "tools/quality/validate_api_contract.py",
        "tests/quality/test_api_contract.py",
    }:
        return "generator.catalog"
    if path in {"CMakeLists.txt", "CMakePresets.json", "Doxyfile.in"} or path.startswith(
        (".github/", "cmake/", "examples/m2-flagship/")
    ):
        return "integration.global"
    if path.startswith("include/cxxlens/"):
        relative = path.removeprefix("include/cxxlens/")
        if "/" not in relative and relative.endswith(".hpp"):
            package = pathlib.PurePosixPath(relative).stem
            if package in packages:
                return f"integration.package.{package}"
            return "integration.global"
        return classify_domain(path)
    if path.startswith("schemas/"):
        domain = classify_domain(path)
        return domain if domain != "steward.quality" else "steward.schema"
    if path.startswith("src/"):
        return classify_domain(path)
    if path.startswith(("tools/quality/", "tests/")):
        return classify_domain(path)
    if path.startswith("docs/design/"):
        return "steward.design"
    if path.startswith("docs/") or path in {"README.md", "CONTRIBUTING.md", "SECURITY.md"}:
        return "steward.documentation"
    return "steward.repository"


def roles_for(packages: set[str], units: list[dict[str, Any]]) -> list[dict[str, str]]:
    definitions = dict(ROLE_DEFINITIONS)
    for package in packages:
        definitions[f"integration.package.{package}"] = (
            "package_integration",
            "#28",
            f"Sole integration writer for the {package} public aggregator and package handoff.",
        )
    for unit in units:
        definitions[unit["unit_owner_role"]] = (
            "atomic_unit",
            "#28",
            f"Exclusive implementation and fixture prefixes for {unit['atomic_unit_id']}.",
        )
    return [
        {
            "id": role_id,
            "kind": values[0],
            "owner_issue": values[1],
            "responsibility": values[2],
        }
        for role_id, values in sorted(definitions.items())
    ]


def make_units(corpus: dict[str, Any]) -> list[dict[str, Any]]:
    packet_by_api = {packet["api_id"]: packet for packet in corpus["packets"]}
    units = []
    for atomic_unit in corpus["atomic_units"]:
        member_packets = [packet_by_api[item] for item in atomic_unit["member_api_ids"]]
        package = member_packets[0]["package"]["id"]
        if any(packet["package"]["id"] != package for packet in member_packets):
            fail("ownership.cross-package-unit", f"{atomic_unit['id']}: spans packages")
        slug = atomic_unit["id"].lower()
        read_only = sorted(
            {
                "schemas/cxxlens.agent-task-packet-corpus.v1.json",
                *(packet["declaration"]["source"] for packet in member_packets),
                *(
                    packet["package"]["header"].strip("<>")
                    for packet in member_packets
                    if packet["package"]["header"].startswith("<cxxlens/")
                ),
            }
        )
        read_only = [
            f"include/{path}" if path.startswith("cxxlens/") else path for path in read_only
        ]
        units.append(
            {
                "atomic_unit_id": atomic_unit["id"],
                "package_id": package,
                "member_api_ids": atomic_unit["member_api_ids"],
                "unit_owner_role": f"unit.{slug}",
                "allowed_write_prefixes": [
                    f"src/{package}/agent_units/{slug}/",
                    f"tests/fixtures/{package}/agent_units/{slug}/",
                    f"tests/unit/{package}/agent_units/{slug}/",
                ],
                "read_only_shared_inputs": sorted(set(read_only)),
                "package_integration_role": f"integration.package.{package}",
                "dependency_request_contract": REQUEST_SCHEMA,
            }
        )
    return units


def make_skeletons(corpus: dict[str, Any]) -> list[dict[str, Any]]:
    skeletons = []
    for packet in corpus["packets"]:
        declaration = packet["declaration"]
        state = "frozen" if declaration["status"] == "exact" else "blocked"
        skeleton = {
            "api_id": packet["api_id"],
            "atomic_unit_id": packet["atomic_unit_id"],
            "state": state,
            "contract_state": packet["contract"]["state"],
            "contract_owner_issue": packet["contract"]["owner_issue"],
            "contract_transition_issue": packet["contract"]["transition_issue"],
            "declaration_source": declaration["source"],
            "signature": declaration["signature"],
            "signature_fingerprint": declaration["signature_fingerprint"],
            "source_fingerprint": declaration["source_fingerprint"],
            "block_reasons": (
                [] if state == "frozen" else packet["generation"]["block_reasons"]
            ),
        }
        skeleton["skeleton_fingerprint"] = digest(skeleton)
        skeletons.append(skeleton)
    return skeletons


def generate_manifest(corpus: dict[str, Any], paths: list[str]) -> dict[str, Any]:
    packages = package_ids(corpus)
    units = make_units(corpus)
    roles = roles_for(packages, units)
    role_ids = {role["id"] for role in roles}
    baseline_paths = [path for path in sorted(paths) if reserved_policy(path) is None]
    tracked_paths = []
    for path in baseline_paths:
        owner = owner_for_path(path, packages, units)
        if owner not in role_ids:
            fail("ownership.missing-role", f"{path}: classified to unknown role {owner}")
        generator = GENERATED_PATHS.get(path)
        tracked_paths.append(
            {
                "path": path,
                "owner_role": owner,
                "access": "exclusive_write",
                "generated": generator is not None,
                "generator_role": generator,
            }
        )
    skeletons = make_skeletons(corpus)
    skeleton_counts = collections.Counter(item["state"] for item in skeletons)
    contract_counts = collections.Counter(item["contract_state"] for item in skeletons)
    manifest: dict[str, Any] = {
        "schema": OWNERSHIP_SCHEMA,
        "task_packet_digest": corpus["semantic_digest"],
        "global_contract_fingerprints": corpus["global_contract_fingerprints"],
        "repository_paths_digest": digest(
            {"baseline_paths": baseline_paths, "reserved_paths": RESERVED_PATHS}
        ),
        "roles": roles,
        "tracked_paths": tracked_paths,
        "reserved_paths": RESERVED_PATHS,
        "units": units,
        "skeletons": skeletons,
        "summary": {
            "role_count": len(roles),
            "tracked_path_count": len(tracked_paths),
            "reserved_path_count": len(RESERVED_PATHS),
            "unit_count": len(units),
            "skeleton_state_counts": {
                "blocked": skeleton_counts.get("blocked", 0),
                "frozen": skeleton_counts.get("frozen", 0),
            },
            "contract_state_counts": dict(sorted(contract_counts.items())),
            "generated_path_count": sum(item["generated"] for item in tracked_paths),
        },
    }
    manifest["semantic_digest"] = digest(manifest)
    return manifest


def make_dependency_request_example(
    manifest: dict[str, Any], corpus: dict[str, Any]
) -> dict[str, Any]:
    skeleton = next(
        (item for item in manifest["skeletons"] if item["state"] == "blocked"),
        None,
    )
    if skeleton is None:
        blocked_packet = next(
            (
                packet
                for packet in corpus["packets"]
                if packet["generation"]["state"] == "blocked"
            ),
            None,
        )
        if blocked_packet is None:
            blocked_packet = next(
                packet
                for packet in corpus["packets"]
                if packet["implementation_state"] == "unimplemented"
            )
        blocked_api_id = blocked_packet["api_id"]
        skeleton = next(
            item for item in manifest["skeletons"] if item["api_id"] == blocked_api_id
        )
        unit = next(
            item
            for item in manifest["units"]
            if item["atomic_unit_id"] == skeleton["atomic_unit_id"]
        )
        kind = "missing_fixture"
        steward_target = unit["package_integration_role"]
        evidence = [
            f"{skeleton['api_id']}:phase_c_implementation_fixture_pending",
            skeleton["skeleton_fingerprint"],
        ]
        requested_behavior = [
            "Publish the production-backed positive, negative and ambiguous Phase C implementation fixtures."
        ]
        acceptance_criteria = [
            "The frozen declaration is exercised through its public package boundary with regenerated readiness evidence."
        ]
    else:
        kind = "missing_contract"
        steward_target = "generator.catalog"
        evidence = [
            f"{skeleton['api_id']}:exact_declaration_unresolved",
            skeleton["skeleton_fingerprint"],
        ]
        requested_behavior = [
            "Publish an exact declaration through the authoritative API catalog change process."
        ]
        acceptance_criteria = [
            "The declaration has a source-bound signature fingerprint and regenerated task packet."
        ]
    request = {
        "schema": REQUEST_SCHEMA,
        "request_id": f"DR-{skeleton['atomic_unit_id']}-001",
        "state": "pending",
        "requesting_atomic_unit": skeleton["atomic_unit_id"],
        "blocked_api_ids": [skeleton["api_id"]],
        "kind": kind,
        "steward_target": steward_target,
        "evidence": evidence,
        "requested_behavior": requested_behavior,
        "acceptance_criteria": acceptance_criteria,
        "resolution": None,
    }
    request["semantic_digest"] = digest(request)
    return {"schema": "cxxlens.dependency-request-examples.v1", "requests": [request]}


def _schema_validate(value: Any, schema: dict[str, Any], context: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(value)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail("ownership.schema-invalid", f"{context}: {error.message}")


def _safe_path(path: str) -> None:
    pure = pathlib.PurePosixPath(path)
    if pure.is_absolute() or ".." in pure.parts or "\\" in path or not path:
        fail("ownership.unsafe-path", f"unsafe repository path: {path}")


def validate_manifest(
    manifest: dict[str, Any],
    corpus: dict[str, Any],
    paths: list[str],
    schema: dict[str, Any] | None = None,
) -> None:
    if not isinstance(manifest, dict) or manifest.get("schema") != OWNERSHIP_SCHEMA:
        fail("ownership.unknown-schema", "ownership schema is not supported")
    role_ids = [role["id"] for role in manifest.get("roles", [])]
    if len(role_ids) != len(set(role_ids)):
        fail("ownership.duplicate-role", "role identifiers must be unique")
    tracked = manifest.get("tracked_paths", [])
    tracked_names = [item["path"] for item in tracked]
    if len(tracked_names) != len(set(tracked_names)):
        fail("ownership.overlap", "a tracked path has multiple write owners")
    reserved = manifest.get("reserved_paths", [])
    for item in reserved:
        _safe_path(item["prefix"])
        if item["owner_role"] not in role_ids:
            fail("ownership.missing-role", f"{item['prefix']}: reserved owner is missing")
    reserved_prefixes = [item["prefix"] for item in reserved]
    for index, prefix in enumerate(reserved_prefixes):
        if any(
            prefix.startswith(other) or other.startswith(prefix)
            for other in reserved_prefixes[index + 1 :]
        ):
            fail("ownership.overlap", f"reserved prefix overlaps: {prefix}")
    baseline_paths = [path for path in paths if reserved_policy(path) is None]
    missing = sorted(set(baseline_paths) - set(tracked_names))
    extra = sorted(set(tracked_names) - set(paths))
    if missing or extra:
        fail("ownership.path-coverage", f"missing={missing}, extra={extra}")
    for item in tracked:
        _safe_path(item["path"])
        if item["owner_role"] not in role_ids:
            fail("ownership.missing-role", f"{item['path']}: owner role is missing")
        if item["generated"] and item["generator_role"] not in role_ids:
            fail("ownership.generated-owner-missing", f"{item['path']}: generator is missing")
        if not item["generated"] and item["generator_role"] is not None:
            fail("ownership.generated-policy", f"{item['path']}: unexpected generator")
    units = manifest.get("units", [])
    unit_ids = [unit["atomic_unit_id"] for unit in units]
    expected_units = [unit["id"] for unit in corpus["atomic_units"]]
    if sorted(unit_ids) != sorted(expected_units):
        fail("ownership.unit-coverage", "ownership units differ from task packets")
    prefixes: list[tuple[str, str]] = []
    for unit in units:
        if unit["unit_owner_role"] not in role_ids:
            fail("ownership.missing-role", f"{unit['atomic_unit_id']}: unit owner is missing")
        for prefix in unit["allowed_write_prefixes"]:
            _safe_path(prefix)
            for existing, existing_unit in prefixes:
                if prefix.startswith(existing) or existing.startswith(prefix):
                    fail(
                        "ownership.overlap",
                        f"{unit['atomic_unit_id']} and {existing_unit} overlap at {prefix}",
                    )
            prefixes.append((prefix, unit["atomic_unit_id"]))
            conflicts = [
                item["path"]
                for item in tracked
                if item["path"].startswith(prefix)
                and item["owner_role"] != unit["unit_owner_role"]
            ]
            if conflicts:
                fail(
                    "ownership.overlap",
                    f"{unit['atomic_unit_id']}: tracked prefix conflicts {conflicts}",
                )
    skeletons = manifest.get("skeletons", [])
    skeleton_ids = [item["api_id"] for item in skeletons]
    expected_apis = [packet["api_id"] for packet in corpus["packets"]]
    if sorted(skeleton_ids) != sorted(expected_apis) or len(skeleton_ids) != len(set(skeleton_ids)):
        fail("ownership.skeleton-coverage", "every API requires exactly one skeleton record")
    packet_by_api = {packet["api_id"]: packet for packet in corpus["packets"]}
    for skeleton in skeletons:
        packet = packet_by_api[skeleton["api_id"]]
        declaration = packet["declaration"]
        expected_state = "frozen" if declaration["status"] == "exact" else "blocked"
        if skeleton["state"] != expected_state:
            fail("ownership.skeleton-state", f"{skeleton['api_id']}: invalid skeleton state")
        if (
            skeleton["contract_state"] != packet["contract"]["state"]
            or skeleton["contract_owner_issue"] != packet["contract"]["owner_issue"]
            or skeleton["contract_transition_issue"] != packet["contract"]["transition_issue"]
        ):
            fail("ownership.contract-state-drift", f"{skeleton['api_id']}: contract state differs")
        if (
            skeleton["signature"] != declaration["signature"]
            or skeleton["signature_fingerprint"] != declaration["signature_fingerprint"]
            or skeleton["source_fingerprint"] != declaration["source_fingerprint"]
        ):
            fail("ownership.skeleton-drift", f"{skeleton['api_id']}: frozen declaration drift")
        unsigned = copy.deepcopy(skeleton)
        fingerprint = unsigned.pop("skeleton_fingerprint")
        if fingerprint != digest(unsigned):
            fail("ownership.skeleton-drift", f"{skeleton['api_id']}: skeleton digest mismatch")
    unsigned_manifest = copy.deepcopy(manifest)
    semantic_digest = unsigned_manifest.pop("semantic_digest", None)
    if semantic_digest != digest(unsigned_manifest):
        fail("ownership.digest-mismatch", "ownership semantic digest mismatch")
    if schema is not None:
        _schema_validate(manifest, schema, "ownership manifest")
    expected = generate_manifest(corpus, paths)
    if manifest != expected:
        fail("ownership.stale-manifest", "ownership manifest is stale")


def validate_changed_paths(
    manifest: dict[str, Any], requester: str, changed_paths: list[str]
) -> dict[str, Any]:
    unit_by_id = {unit["atomic_unit_id"]: unit for unit in manifest["units"]}
    roles = {role["id"]: role for role in manifest["roles"]}
    tracked = {item["path"]: item for item in manifest["tracked_paths"]}
    if requester not in unit_by_id and requester not in roles:
        fail("ownership.unknown-requester", f"unknown requester: {requester}")
    checked = []
    for path in sorted(set(changed_paths)):
        _safe_path(path)
        policy = tracked.get(path)
        if policy is None:
            reserved = next(
                (
                    item
                    for item in manifest["reserved_paths"]
                    if path.startswith(item["prefix"])
                ),
                None,
            )
            if reserved is not None:
                policy = {
                    "path": path,
                    "owner_role": reserved["owner_role"],
                    "access": reserved["access"],
                    "generated": False,
                    "generator_role": None,
                }
        if requester in unit_by_id:
            unit = unit_by_id[requester]
            if any(path.startswith(prefix) for prefix in unit["allowed_write_prefixes"]):
                checked.append(path)
                continue
            if policy and policy["generated"]:
                fail(
                    "ownership.generated-direct-edit",
                    f"path={path} generator={policy['generator_role']} requester={requester}",
                )
            owner = policy["owner_role"] if policy else "unowned"
            fail(
                "ownership.unauthorized-path",
                f"path={path} owner={owner} requester={requester} "
                "alternative=dependency_request",
            )
        if policy is None:
            fail("ownership.path-coverage", f"path={path} has no declared owner")
        if policy["generated"] and policy["generator_role"] != requester:
            fail(
                "ownership.generated-direct-edit",
                f"path={path} generator={policy['generator_role']} requester={requester}",
            )
        if policy["owner_role"] != requester:
            fail(
                "ownership.unauthorized-path",
                f"path={path} owner={policy['owner_role']} requester={requester}",
            )
        checked.append(path)
    return {
        "schema": "cxxlens.agent-ownership-audit.v1",
        "requester": requester,
        "status": "passed",
        "changed_paths": checked,
        "manifest_digest": manifest["semantic_digest"],
    }


def validate_dependency_request(
    request: dict[str, Any],
    manifest: dict[str, Any],
    schema: dict[str, Any],
) -> None:
    if not isinstance(request, dict) or request.get("schema") != REQUEST_SCHEMA:
        fail("dependency_request.unknown-schema", "dependency request schema is not supported")
    _schema_validate(request, schema, "dependency request")
    unit_by_id = {unit["atomic_unit_id"]: unit for unit in manifest["units"]}
    unit = unit_by_id.get(request["requesting_atomic_unit"])
    if unit is None:
        fail("dependency_request.unknown-unit", request["requesting_atomic_unit"])
    if not set(request["blocked_api_ids"]).issubset(unit["member_api_ids"]):
        fail("dependency_request.api-unit-mismatch", "blocked APIs are outside the unit")
    role_ids = {role["id"] for role in manifest["roles"]}
    if request["steward_target"] not in role_ids:
        fail("dependency_request.unknown-steward", request["steward_target"])
    unsigned = copy.deepcopy(request)
    semantic_digest = unsigned.pop("semantic_digest", None)
    if semantic_digest != digest(unsigned):
        fail("dependency_request.digest-mismatch", request["request_id"])
    state = request["state"]
    resolution = request["resolution"]
    if state in {"resolved", "rejected"} and resolution["outcome"] != state:
        fail("dependency_request.invalid-transition", f"{state} outcome mismatch")


def transition_request(
    request: dict[str, Any],
    new_state: str,
    manifest: dict[str, Any],
    evidence: list[str] | None = None,
) -> dict[str, Any]:
    transitions = {
        "pending": {"accepted", "rejected"},
        "accepted": {"resolved", "rejected"},
        "resolved": set(),
        "rejected": set(),
    }
    if new_state not in transitions[request["state"]]:
        fail(
            "dependency_request.invalid-transition",
            f"{request['state']} -> {new_state} is not permitted",
        )
    updated = copy.deepcopy(request)
    updated["state"] = new_state
    if new_state in {"resolved", "rejected"}:
        affected = sorted(request["blocked_api_ids"])
        skeletons = {
            item["api_id"]: item["skeleton_fingerprint"]
            for item in manifest["skeletons"]
        }
        updated["resolution"] = {
            "outcome": new_state,
            "evidence": sorted(evidence or ["steward decision recorded"]),
            "affected_packet_ids": affected,
            "reissue_fingerprint": digest(
                {
                    "request_id": request["request_id"],
                    "outcome": new_state,
                    "skeletons": {api_id: skeletons[api_id] for api_id in affected},
                    "task_packet_digest": manifest["task_packet_digest"],
                }
            ),
        }
    else:
        updated["resolution"] = None
    unsigned = copy.deepcopy(updated)
    unsigned.pop("semantic_digest", None)
    updated["semantic_digest"] = digest(unsigned)
    return updated


def load_json(path: pathlib.Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def load_yaml(path: pathlib.Path) -> Any:
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def resolve(root: pathlib.Path, path: pathlib.Path) -> pathlib.Path:
    return path if path.is_absolute() else root / path


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("generate", "check", "preflight", "request-check"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument(
        "--task-packets",
        type=pathlib.Path,
        default=pathlib.Path("schemas/cxxlens.agent-task-packet-corpus.v1.json"),
    )
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=pathlib.Path("schemas/cxxlens.agent-ownership.v1.json"),
    )
    parser.add_argument(
        "--schema",
        type=pathlib.Path,
        default=pathlib.Path("schemas/cxxlens.agent-ownership.v1.schema.yaml"),
    )
    parser.add_argument(
        "--request-schema",
        type=pathlib.Path,
        default=pathlib.Path("schemas/cxxlens.dependency-request.v1.schema.yaml"),
    )
    parser.add_argument(
        "--request-examples",
        type=pathlib.Path,
        default=pathlib.Path("schemas/cxxlens.dependency-request.examples.v1.json"),
    )
    parser.add_argument("--requester")
    parser.add_argument("--changed-path", action="append", default=[])
    parser.add_argument("--request", type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    corpus = load_json(resolve(root, args.task_packets))
    manifest_path = resolve(root, args.manifest)
    schema = load_yaml(resolve(root, args.schema))
    request_schema = load_yaml(resolve(root, args.request_schema))
    examples_path = resolve(root, args.request_examples)
    paths = repository_paths(root)
    generated = generate_manifest(corpus, paths)
    examples = make_dependency_request_example(generated, corpus)
    _schema_validate(generated, schema, "generated ownership manifest")
    for request in examples["requests"]:
        validate_dependency_request(request, generated, request_schema)
    if args.mode == "generate":
        manifest_path.write_text(pretty_json(generated), encoding="utf-8")
        examples_path.write_text(pretty_json(examples), encoding="utf-8")
    else:
        manifest = load_json(manifest_path)
        validate_manifest(manifest, corpus, paths, schema)
        checked_examples = load_json(examples_path)
        if checked_examples != examples:
            fail("dependency_request.stale-examples", "dependency request examples are stale")
        for request in checked_examples["requests"]:
            validate_dependency_request(request, manifest, request_schema)
        if args.mode == "preflight":
            if not args.requester or not args.changed_path:
                fail("ownership.preflight-arguments", "requester and changed paths are required")
            print(pretty_json(validate_changed_paths(manifest, args.requester, args.changed_path)))
        if args.mode == "request-check":
            if args.request is None:
                fail("dependency_request.arguments", "--request is required")
            validate_dependency_request(
                load_json(resolve(root, args.request)), manifest, request_schema
            )
    print(
        f"ownership {args.mode} passed: {generated['summary']['tracked_path_count']} paths, "
        f"{generated['summary']['unit_count']} units, "
        f"digest {generated['semantic_digest']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        json.JSONDecodeError,
        OSError,
        OwnershipError,
        subprocess.CalledProcessError,
        yaml.YAMLError,
    ) as error:
        print(f"ownership generation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
