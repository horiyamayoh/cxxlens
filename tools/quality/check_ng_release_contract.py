#!/usr/bin/env python3
"""Validate and evaluate the next-generation release compatibility contract."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from collections import defaultdict
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
BUNDLE = pathlib.Path("schemas/cxxlens_ng_release_bundle.yaml")
BUNDLE_SCHEMA = pathlib.Path("schemas/cxxlens_ng_release_bundle.schema.yaml")
REQUEST_SCHEMA = pathlib.Path("schemas/cxxlens_ng_compatibility_request.schema.yaml")
REPORT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_compatibility_report.schema.yaml")

EXPECTED_COMPONENTS = {
    "relation-kernel",
    "logical-query",
    "cc-cpp-semantics",
    "portable-provider-sdk",
    "native-provider",
    "recipes",
}
EXPECTED_TARGETS = {
    "cxxlens::base",
    "cxxlens::kernel",
    "cxxlens::query",
    "cxxlens::cpp",
    "cxxlens::provider_sdk",
    "cxxlens::recipes",
    "cxxlens::cxxlens",
}
EXPECTED_AXES = {
    "distribution",
    "kernel-semantics",
    "relation-descriptor",
    "identity-contract",
    "condition-semantics",
    "snapshot-format",
    "provider-protocol",
    "provider-implementation",
    "native-sdk",
    "logical-query-ir",
    "recipe-semantics",
    "model-assumption-pack",
    "patch-artifact-plan",
}
EXPECTED_REASONS = {
    "compat.exact",
    "compat.same-major",
    "compat.optional-feature-unavailable",
    "compat.migration-required",
    "compat.release-not-qualified",
    "compat.release-unknown",
    "compat.axis-missing",
    "compat.axis-duplicate",
    "compat.axis-major-mismatch",
    "compat.axis-version-mismatch",
    "compat.axis-version-too-old",
    "compat.contract-digest-mismatch",
    "compat.required-feature-missing",
    "compat.request-invalid",
}


class ReleaseContractError(ValueError):
    """A stable release-contract or compatibility-request violation."""


def fail(message: str) -> None:
    raise ReleaseContractError(message)


def load_document(path: pathlib.Path) -> dict[str, Any]:
    text = path.read_text(encoding="utf-8")
    value = json.loads(text) if path.suffix == ".json" else yaml.safe_load(text)
    if not isinstance(value, dict):
        fail(f"expected mapping: {path}")
    return value


def schema_validate(document: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"{label} schema validation failed: {error.message}")


def rows_by_id(rows: list[dict[str, Any]], label: str) -> dict[str, dict[str, Any]]:
    grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        grouped[row["id"]].append(row)
    duplicates = sorted(identifier for identifier, values in grouped.items() if len(values) != 1)
    if duplicates:
        fail(f"{label} contains duplicate IDs: {duplicates}")
    return {identifier: values[0] for identifier, values in grouped.items()}


def assert_acyclic(graph: dict[str, list[str]], label: str) -> None:
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(node: str) -> None:
        if node in visiting:
            fail(f"{label} dependency cycle includes {node}")
        if node in visited:
            return
        visiting.add(node)
        for dependency in graph[node]:
            if dependency not in graph:
                fail(f"{label} has dangling dependency: {node} -> {dependency}")
            visit(dependency)
        visiting.remove(node)
        visited.add(node)

    for node in sorted(graph):
        visit(node)


def dependency_closure(graph: dict[str, list[str]], start: str) -> set[str]:
    result: set[str] = set()
    pending = list(graph[start])
    while pending:
        node = pending.pop()
        if node in result:
            continue
        result.add(node)
        pending.extend(graph[node])
    return result


def validate_boundary(bundle: dict[str, Any]) -> None:
    components = rows_by_id(bundle["product_boundary"]["components"], "components")
    if set(components) != EXPECTED_COMPONENTS:
        fail("product component set differs from the accepted boundary")
    graph = {identifier: row["depends_on"] for identifier, row in components.items()}
    assert_acyclic(graph, "product component")
    for identifier, row in components.items():
        closure = dependency_closure(graph, identifier)
        forbidden = set(row["forbidden_dependencies"])
        if closure & forbidden:
            fail(f"{identifier} reaches a forbidden dependency: {sorted(closure & forbidden)}")
        if row["domain"] == "language-neutral":
            foreign = sorted(
                dependency
                for dependency in closure
                if components[dependency]["domain"] != "language-neutral"
            )
            if foreign:
                fail(f"language-neutral component {identifier} reaches {foreign}")
            native_values = [
                value
                for value in row["exported_values"]
                if value.startswith(("cc-", "cxx-", "clang-", "llvm-", "native-"))
            ]
            if native_values:
                fail(f"language-neutral component {identifier} exports native values")

    targets = {
        row["name"]: row for row in bundle["distribution_surface"]["public_targets"]
    }
    if len(targets) != len(bundle["distribution_surface"]["public_targets"]):
        fail("public target list contains duplicate names")
    if set(targets) != EXPECTED_TARGETS:
        fail("public CMake target set differs from the accepted surface")
    target_graph = {
        name: row["direct_dependencies"] for name, row in targets.items()
    }
    assert_acyclic(target_graph, "public target")
    aggregate = targets[bundle["distribution_surface"]["aggregate_target"]]
    if aggregate["direct_dependencies"] != [
        "cxxlens::base",
        "cxxlens::kernel",
        "cxxlens::query",
        "cxxlens::cpp",
    ]:
        fail("aggregate target dependency surface differs")
    if not {"cxxlens::provider_sdk", "cxxlens::recipes", "native-provider", "native-sdk"}.issubset(
        set(bundle["distribution_surface"]["aggregate_excludes"])
    ):
        fail("aggregate target does not exclude optional/native surfaces")


def validate_release_mapping(bundle: dict[str, Any], root: pathlib.Path) -> None:
    profiles = rows_by_id(bundle["profiles"], "profiles")
    if set(profiles) != {"NG0", "NG1", "NG2", "NG3"}:
        fail("release profile set differs")
    if profiles["NG0"]["release_role"] != "1.0-candidate-minimum":
        fail("NG0 must be the 1.0 candidate minimum")
    if profiles["NG1"]["release_role"] != "1.0-production-hardening":
        fail("NG1 must be required production hardening for 1.0")
    for profile in ("NG2", "NG3"):
        if profiles[profile]["distribution_requirement"] != "not-a-1.0-blocker":
            fail(f"{profile} must not block 1.0")

    releases = rows_by_id(bundle["releases"], "releases")
    if set(releases) != {
        "ng0-contract-candidate",
        "distribution-1.0",
        "distribution-1.x",
        "future-major",
    }:
        fail("distribution release set differs")
    one = releases["distribution-1.0"]
    if one["required_profiles"] != ["NG0", "NG1"]:
        fail("distribution 1.0 must require NG0 and NG1")
    if one["blocker_migrations"] != ["R0", "R1", "R2", "R3", "R4"]:
        fail("distribution 1.0 blocker migration set differs")
    expected_gates = [
        "gate.g0",
        "gate.g1",
        "gate.g2",
        "gate.g3",
        "gate.g4",
        "gate.g5",
        "gate.release",
    ]
    if one["blocker_gates"] != expected_gates:
        fail("distribution 1.0 blocker gate set differs")
    if bundle["non_blocking_migrations"]["distribution-1.0"] != ["R5", "R6", "R7"]:
        fail("distribution 1.0 optional migration branches differ")
    if releases["future-major"].get("activation_rule") != (
        "breaking-change-to-an-accepted-stable-version-axis"
    ):
        fail("future major must be triggered only by a stable-axis break")

    acceptance = load_document(root / "schemas/cxxlens_ng_acceptance_manifest.yaml")
    gates = rows_by_id(acceptance["entries"], "acceptance gates")
    if set(gates["gate.g5"].get("depends_on", [])) != {
        "gate.g2",
        "gate.g3",
        "gate.g4",
    }:
        fail("gate.g5 must include provider hardening gate.g4")
    if set(gates["gate.release"].get("depends_on", [])) != set(expected_gates[:-1]):
        fail("gate.release must depend on G0 through G5")


def validate_version_contract(bundle: dict[str, Any], root: pathlib.Path) -> None:
    axes = rows_by_id(bundle["version_axes"], "version axes")
    if set(axes) != EXPECTED_AXES:
        fail("independent version axis set differs")
    contexts = rows_by_id(bundle["compatibility_contexts"], "compatibility contexts")
    if set(contexts) != {"provider-handshake", "snapshot-open", "query-load", "release-startup"}:
        fail("compatibility context set differs")
    for context in contexts.values():
        if not set(context["required_axes"]).issubset(axes):
            fail(f"compatibility context has an unknown axis: {context['id']}")
    if set(contexts["release-startup"]["required_axes"]) != EXPECTED_AXES:
        fail("release-startup must decide every independent version axis")
    reasons = set(bundle["inspection"]["report_reason_codes"])
    if reasons != EXPECTED_REASONS:
        fail("structured compatibility reason code set differs")
    migrations = rows_by_id(bundle["migrations"], "compatibility migrations")
    migration_keys: set[tuple[str, str, str]] = set()
    for migration in migrations.values():
        if migration["axis"] not in axes:
            fail(f"migration uses unknown axis: {migration['axis']}")
        key = (migration["axis"], migration["from"], migration["to"])
        if key in migration_keys:
            fail(f"migration path is ambiguous: {key}")
        migration_keys.add(key)
        if semver(migration["from"])[0] != semver(migration["to"])[0]:
            fail(f"runtime migration must not cross an axis major: {migration['id']}")
        if semver(migration["from"]) >= semver(migration["to"]):
            fail(f"migration must advance an axis version: {migration['id']}")
    for relative in (BUNDLE_SCHEMA, REQUEST_SCHEMA, REPORT_SCHEMA):
        if not (root / relative).is_file():
            fail(f"release contract schema is missing: {relative}")


def validate_design_markers(root: pathlib.Path) -> None:
    design = (root / "docs/design/cxxlens_next_generation_integrated_design_ja.md").read_text(
        encoding="utf-8"
    )
    for marker in (
        "NG0 は 1.0 候補の最小垂直スライス",
        "NG1 は 1.0 release に必須の production hardening",
        "G0 から G5、GR",
        "R5、R6、R7 は 1.0 blocker ではない",
        "C++ module は 1.0 の installed",
    ):
        if marker not in design:
            fail(f"normative release marker is missing: {marker}")


def validate_bundle(root: pathlib.Path) -> dict[str, Any]:
    bundle = load_document(root / BUNDLE)
    schema_validate(bundle, load_document(root / BUNDLE_SCHEMA), "NG release bundle")
    validate_boundary(bundle)
    validate_release_mapping(bundle, root)
    validate_version_contract(bundle, root)
    validate_design_markers(root)
    return bundle


def semver(value: str) -> tuple[int, int, int]:
    try:
        parts = tuple(int(part) for part in value.split("."))
    except ValueError as error:
        fail(f"invalid semantic version: {value}")
    if len(parts) != 3:
        fail(f"invalid semantic version: {value}")
    return parts


def grouped_axes(rows: list[dict[str, Any]]) -> dict[str, list[dict[str, Any]]]:
    result: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        result[row["axis"]].append(row)
    return result


def make_axis_result(
    bundle: dict[str, Any], required: dict[str, Any], offered_rows: list[dict[str, Any]]
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    axis = required["axis"]
    base = {
        "axis": axis,
        "required_version": required["version"],
        "offered_version": None,
        "status": "unsupported",
        "reason_codes": [],
        "missing_required_features": [],
        "unavailable_optional_features": [],
    }
    if len(offered_rows) == 0:
        base["reason_codes"] = ["compat.axis-missing"]
        return base, []
    if len(offered_rows) > 1:
        base["reason_codes"] = ["compat.axis-duplicate"]
        return base, []
    offered = offered_rows[0]
    policy = next(row for row in bundle["version_axes"] if row["id"] == axis)[
        "compatibility"
    ]
    base["offered_version"] = offered["version"]
    required_features = {
        row["id"] for row in required["features"] if row["requirement"] == "required"
    }
    optional_features = {
        row["id"] for row in required["features"] if row["requirement"] == "optional"
    }
    offered_features = {row["id"] for row in offered["features"]}
    base["missing_required_features"] = sorted(required_features - offered_features)
    base["unavailable_optional_features"] = sorted(optional_features - offered_features)
    if base["missing_required_features"]:
        base["reason_codes"] = ["compat.required-feature-missing"]
        return base, []
    digest_required = policy in {
        "declared-major-minor-and-exact-digest",
        "certified-tuple",
        "exact-provider-major-and-toolchain",
        "exact-digest",
    }
    if (
        digest_required
        and (
            required["contract_digest"] is None
            or offered["contract_digest"] is None
            or required["contract_digest"] != offered["contract_digest"]
        )
    ) or (
        required["contract_digest"] is not None
        and required["contract_digest"] != offered["contract_digest"]
    ):
        base["reason_codes"] = ["compat.contract-digest-mismatch"]
        return base, []
    required_version = semver(required["version"])
    offered_version = semver(offered["version"])
    if required_version[0] != offered_version[0]:
        base["reason_codes"] = ["compat.axis-major-mismatch"]
        return base, []
    exact_version = policy in {
        "exact-selected-release",
        "certified-tuple",
        "exact-provider-major-and-toolchain",
    }
    if exact_version and offered_version != required_version:
        base["reason_codes"] = ["compat.axis-version-mismatch"]
        return base, []
    if offered_version < required_version:
        migrations = [
            row
            for row in bundle["migrations"]
            if row["axis"] == axis
            and row["from"] == offered["version"]
            and row["to"] == required["version"]
        ]
        if len(migrations) == 1:
            migration = migrations[0]
            base["status"] = "migration-required"
            base["reason_codes"] = ["compat.migration-required"]
            return base, [
                {
                    "axis": axis,
                    "migration_id": migration["id"],
                    "handler": migration["handler"],
                    "state": migration["state"],
                }
            ]
        base["reason_codes"] = ["compat.axis-version-too-old"]
        return base, []
    base["status"] = "supported"
    base["reason_codes"] = [
        "compat.exact" if offered_version == required_version else "compat.same-major"
    ]
    if base["unavailable_optional_features"]:
        base["reason_codes"].append("compat.optional-feature-unavailable")
    return base, []


def decide(bundle: dict[str, Any], request: dict[str, Any], root: pathlib.Path) -> dict[str, Any]:
    schema_validate(
        request,
        load_document(root / REQUEST_SCHEMA),
        "compatibility request",
    )
    releases = rows_by_id(bundle["releases"], "releases")
    contexts = rows_by_id(bundle["compatibility_contexts"], "compatibility contexts")
    release = releases.get(request["release_id"])
    if release is None:
        report = {
            "schema": "cxxlens.ng-compatibility-report.v1",
            "request_id": request["request_id"],
            "operation": request["operation"],
            "decision": "unsupported",
            "release_id": request["release_id"],
            "selected_bundle": None,
            "context": request["context"],
            "qualification_state": "unknown",
            "axis_results": [],
            "reason_codes": ["compat.release-unknown"],
            "migration_steps": [],
            "environment_findings": [],
            "fallback_used": False,
        }
        schema_validate(report, load_document(root / REPORT_SCHEMA), "compatibility report")
        return report

    expected_axes = set(contexts[request["context"]]["required_axes"])
    required_grouped = grouped_axes(request["required_axes"])
    offered_grouped = grouped_axes(request["offered_axes"])
    invalid_reasons: set[str] = set()
    if set(required_grouped) != expected_axes:
        invalid_reasons.update({"compat.axis-missing", "compat.request-invalid"})
    if any(len(rows) != 1 for rows in required_grouped.values()):
        invalid_reasons.update({"compat.axis-duplicate", "compat.request-invalid"})
    if set(required_grouped) - EXPECTED_AXES or set(offered_grouped) - expected_axes:
        invalid_reasons.add("compat.request-invalid")
    for rows in (*required_grouped.values(), *offered_grouped.values()):
        for row in rows:
            feature_ids = [feature["id"] for feature in row["features"]]
            if len(feature_ids) != len(set(feature_ids)):
                invalid_reasons.add("compat.request-invalid")

    axis_results: list[dict[str, Any]] = []
    migration_steps: list[dict[str, Any]] = []
    if not invalid_reasons:
        for axis in sorted(expected_axes):
            result, steps = make_axis_result(
                bundle, required_grouped[axis][0], offered_grouped.get(axis, [])
            )
            axis_results.append(result)
            migration_steps.extend(steps)
    reason_codes = set(invalid_reasons)
    for result in axis_results:
        reason_codes.update(result["reason_codes"])
    statuses = {result["status"] for result in axis_results}
    if invalid_reasons or "unsupported" in statuses:
        decision = "unsupported"
    elif "migration-required" in statuses:
        decision = "migration-required"
    else:
        decision = "supported"

    findings: list[dict[str, str]] = []
    if request["operation"] == "doctor" and (
        not request["environment"]["runtime_qualified"]
        or not release["production_supported"]
    ):
        decision = "unsupported"
        reason_codes.add("compat.release-not-qualified")
        findings.append(
            {
                "code": "compat.release-not-qualified",
                "severity": "blocker",
                "message": "The requested release has no commit-bound production qualification evidence.",
            }
        )
    report = {
        "schema": "cxxlens.ng-compatibility-report.v1",
        "request_id": request["request_id"],
        "operation": request["operation"],
        "decision": decision,
        "release_id": request["release_id"],
        "selected_bundle": request["release_id"],
        "context": request["context"],
        "qualification_state": release["state"],
        "axis_results": axis_results,
        "reason_codes": sorted(reason_codes),
        "migration_steps": sorted(migration_steps, key=lambda row: row["axis"]),
        "environment_findings": findings,
        "fallback_used": False,
    }
    schema_validate(report, load_document(root / REPORT_SCHEMA), "compatibility report")
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "inspect", "doctor"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--request", type=pathlib.Path)
    arguments = parser.parse_args()
    try:
        root = arguments.root.resolve()
        bundle = validate_bundle(root)
        if arguments.command == "check":
            if arguments.request is not None:
                fail("--request is valid only for inspect or doctor")
            print("validated NG product boundary, release profiles, target DAG and compatibility bundle")
            return 0
        if arguments.request is None:
            fail("inspect and doctor require --request")
        request = load_document(arguments.request.resolve())
        if request.get("operation") != arguments.command:
            fail("request operation differs from the invoked command")
        print(json.dumps(decide(bundle, request, root), ensure_ascii=False, indent=2, sort_keys=True))
        return 0
    except (OSError, ReleaseContractError) as error:
        print(f"NG release contract quality check failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
