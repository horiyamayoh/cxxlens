#!/usr/bin/env python3
"""Validate and report the distribution-1.0 typed production-scope closure."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import subprocess
import sys
import urllib.parse
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

import jsonschema
import yaml


MANIFEST = Path("schemas/cxxlens_ng_production_scope_closure.yaml")
MANIFEST_SCHEMA = Path("schemas/cxxlens_ng_production_scope_closure.schema.yaml")
REPORT_SCHEMA = Path("schemas/cxxlens_ng_production_scope_closure_report.schema.yaml")
EVALUATION_SCHEMA = Path("schemas/cxxlens_ng_release_qualification_evaluation_report.schema.yaml")
GR_REPORT_SCHEMA = Path("schemas/cxxlens_ng_release_qualification_report.schema.yaml")
CHECKER = Path("tools/quality/check_ng_production_scope_closure.py")
EVALUATION_SCHEMA_ID = "cxxlens.ng-release-qualification-evaluation-report.v1"
GR_SCHEMA_ID = "cxxlens.ng-release-qualification-report.v1"

SOURCE_CONTRACTS = (
    "schemas/cxxlens_ng_acceptance_manifest.yaml",
    "schemas/cxxlens_ng_g5_qualification.yaml",
    "schemas/cxxlens_ng_logical_query_contract.yaml",
    "schemas/cxxlens_ng_namespace_registry.yaml",
    "schemas/cxxlens_ng_provider_protocol.yaml",
    "schemas/cxxlens_ng_provider_runtime_contract.yaml",
    "schemas/cxxlens_ng_provider_support_matrix.yaml",
    "schemas/cxxlens_ng_public_api_catalog.yaml",
    "schemas/cxxlens_ng_public_callable_inventory.yaml",
    "schemas/cxxlens_ng_quality_ownership.yaml",
    "schemas/cxxlens_ng_relation_registry.yaml",
    "schemas/cxxlens_ng_release_bundle.yaml",
    "schemas/cxxlens_ng_release_qualification.yaml",
    "schemas/cxxlens_ng_security_profile.yaml",
)
EVIDENCE_CONTRACTS = (
    "schemas/cxxlens_ng_provider_conformance_vectors.yaml",
    "schemas/cxxlens_ng_query_conformance_vectors.yaml",
    "schemas/cxxlens_ng_security_conformance_vectors.yaml",
)

DOMAINS = (
    "release.profile",
    "release.gate",
    "release.migration",
    "distribution.target",
    "distribution.native-package",
    "distribution.installed-tool",
    "distribution.consumer-configuration",
    "distribution.source-surface",
    "public.package",
    "public.author-path",
    "public.catalog-entry",
    "public.header",
    "public.callable",
    "relation.descriptor",
    "relation.static-admission",
    "provider.profile-feature",
    "provider.message",
    "provider.execution-surface",
    "provider.support-tuple",
    "provider.production-tuple-template",
    "query.operator",
    "query.result-status",
    "query.backend",
    "g5.closure-kind",
    "security.profile",
    "security.namespace",
    "security.sandbox-profile",
    "security.sandbox-policy",
    "compatibility.axis",
    "quality.check",
)
AGGREGATE_DOMAINS = {"release.profile", "release.gate"}

EVIDENCE_TEST_PATHS = {
    "quality.ng-foundation_completion": "tests/quality/test_ng_foundation_completion.py",
    "quality.ng-g5_qualification": "tests/quality/test_ng_g5_qualification.py",
    "quality.ng-provider_protocol": "tests/quality/test_ng_provider_protocol.py",
    "quality.ng-provider_runtime": "tests/quality/test_ng_provider_runtime.py",
    "quality.ng-public_callable_inventory": "tests/quality/test_ng_public_callable_inventory.py",
    "quality.ng-query_contract": "tests/quality/test_ng_query_contract.py",
    "quality.ng-relation_contract": "tests/quality/test_ng_relation_contract.py",
    "quality.ng-release_contract": "tests/quality/test_ng_release_contract.py",
    "quality.ng-release_qualification": "tests/quality/test_ng_release_qualification.py",
    "quality.ng-sdk_contract": "tests/quality/test_ng_sdk_contract.py",
    "quality.ng-security_contract": "tests/quality/test_ng_security_contract.py",
    "quality.ownership": "tests/quality/test_quality_ownership.py",
}

DOMAIN_EVIDENCE_TESTS = {
    "release.migration": ("quality.ng-foundation_completion",),
    "distribution.target": ("quality.ng-sdk_contract",),
    "distribution.native-package": ("quality.ng-release_qualification",),
    "distribution.installed-tool": ("quality.ng-release_qualification",),
    "distribution.consumer-configuration": ("quality.ng-release_qualification",),
    "distribution.source-surface": ("quality.ng-sdk_contract",),
    "public.package": ("quality.ng-sdk_contract",),
    "public.author-path": ("quality.ng-sdk_contract",),
    "public.catalog-entry": (
        "quality.ng-public_callable_inventory",
        "quality.ng-sdk_contract",
    ),
    "public.header": (
        "quality.ng-public_callable_inventory",
        "quality.ng-sdk_contract",
    ),
    "public.callable": (
        "quality.ng-public_callable_inventory",
        "quality.ng-sdk_contract",
    ),
    "relation.descriptor": ("quality.ng-relation_contract",),
    "relation.static-admission": (
        "quality.ng-relation_contract",
        "quality.ng-sdk_contract",
    ),
    "provider.profile-feature": ("quality.ng-provider_protocol",),
    "provider.message": ("quality.ng-provider_protocol",),
    "provider.execution-surface": (
        "quality.ng-provider_protocol",
        "quality.ng-provider_runtime",
    ),
    "provider.production-tuple-template": ("quality.ng-release_qualification",),
    "query.operator": ("quality.ng-query_contract",),
    "query.result-status": ("quality.ng-query_contract",),
    "query.backend": ("quality.ng-query_contract",),
    "g5.closure-kind": ("quality.ng-g5_qualification",),
    "security.profile": ("quality.ng-security_contract",),
    "security.namespace": ("quality.ng-security_contract",),
    "security.sandbox-profile": ("quality.ng-security_contract",),
    "security.sandbox-policy": ("quality.ng-security_contract",),
    "compatibility.axis": ("quality.ng-release_contract",),
    "quality.check": ("quality.ownership",),
}

VALID_PAIRS = {
    ("included", "qualified"),
    ("included", "tracked-gap"),
    ("excluded", "not-applicable"),
    ("unresolved", "blocked"),
}
ACTIVE_BLOCKING_STATUSES = {"observed", "investigating", "proposed"}
DISPOSITIONS = {
    "production-required",
    "qualification-evidence",
    "explicit-non-1.0",
    "unresolved-authority",
}


class ContractError(RuntimeError):
    """A fail-closed scope-contract violation."""


@dataclass(frozen=True, order=True)
class SurfaceKey:
    domain: str
    id: str


SURFACE_EVIDENCE_TESTS = {
    SurfaceKey("release.migration", "R4"): ("quality.ng-g5_qualification",),
}
NIGHTLY_EVIDENCE_SURFACES = {
    SurfaceKey("quality.check", "analysis.clang-tidy"),
    SurfaceKey("quality.check", "quality.production-contracts"),
    SurfaceKey("quality.check", "sanitizer.asan-ubsan"),
    SurfaceKey("quality.check", "sanitizer.tsan"),
}


@dataclass(frozen=True)
class SourceNode:
    disposition: str
    cross_links: tuple[SurfaceKey, ...] = ()
    inherited_from: SurfaceKey | None = None


@dataclass
class ValidatedModel:
    root: Path
    manifest: dict[str, Any]
    nodes: dict[SurfaceKey, SourceNode]
    assignments: dict[SurfaceKey, dict[str, Any]]
    expanded: list[dict[str, Any]]
    blocking_feedback: tuple[str, ...]
    authority_census_digest: str
    evidence_census_digest: str
    classification_digest: str
    summary: dict[str, int]
    closure_status: str
    evidence_censuses: list[dict[str, Any]]
    evidence_tests: tuple[str, ...]


def fail(message: str) -> None:
    raise ContractError(message)


def load_yaml(path: Path) -> Any:
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as error:
        fail(f"cannot load {path}: {error}")


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot load {path}: {error}")


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()


def digest_value(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_bytes(value)).hexdigest()


def digest_file(path: Path) -> str:
    try:
        payload = path.read_bytes()
    except OSError as error:
        fail(f"cannot digest {path}: {error}")
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def validate_schema(instance: Any, schema_path: Path) -> None:
    schema = load_yaml(schema_path)
    try:
        jsonschema.Draft202012Validator(
            schema,
            format_checker=jsonschema.Draft202012Validator.FORMAT_CHECKER,
        ).validate(instance)
    except jsonschema.ValidationError as error:
        location = "/".join(str(part) for part in error.absolute_path) or "<root>"
        fail(f"{schema_path}: validation failed at {location}: {error.message}")


def require_repo_path(root: Path, raw: str, *, file_required: bool = True) -> Path:
    path = PurePosixPath(raw)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        fail(f"repository path is not normalized: {raw!r}")
    target = root.joinpath(*path.parts)
    if file_required and not target.is_file():
        fail(f"referenced repository file does not exist: {raw}")
    return target


def add_node(
    nodes: dict[SurfaceKey, SourceNode],
    domain: str,
    item_id: str,
    disposition: str,
    links: Iterable[SurfaceKey] = (),
    inherited_from: SurfaceKey | None = None,
) -> None:
    if domain not in DOMAINS:
        fail(f"unknown inventory domain: {domain}")
    if disposition not in DISPOSITIONS:
        fail(f"unknown disposition for {domain}/{item_id}: {disposition}")
    key = SurfaceKey(domain, str(item_id))
    if key in nodes:
        fail(f"duplicate canonical node: {domain}/{item_id}")
    nodes[key] = SourceNode(
        disposition,
        tuple(sorted(set(links))),
        inherited_from,
    )


def derive_inventory(root: Path) -> dict[SurfaceKey, SourceNode]:
    """Derive the closed 30-domain census solely from accepted source contracts."""

    sources = {path: load_yaml(root / path) for path in SOURCE_CONTRACTS}
    release = sources["schemas/cxxlens_ng_release_bundle.yaml"]
    acceptance = sources["schemas/cxxlens_ng_acceptance_manifest.yaml"]
    release_qualification = sources["schemas/cxxlens_ng_release_qualification.yaml"]
    catalog = sources["schemas/cxxlens_ng_public_api_catalog.yaml"]
    callables = sources["schemas/cxxlens_ng_public_callable_inventory.yaml"]
    relations = sources["schemas/cxxlens_ng_relation_registry.yaml"]
    protocol = sources["schemas/cxxlens_ng_provider_protocol.yaml"]
    support = sources["schemas/cxxlens_ng_provider_support_matrix.yaml"]
    query = sources["schemas/cxxlens_ng_logical_query_contract.yaml"]
    g5 = sources["schemas/cxxlens_ng_g5_qualification.yaml"]
    security = sources["schemas/cxxlens_ng_security_profile.yaml"]
    namespaces = sources["schemas/cxxlens_ng_namespace_registry.yaml"]
    quality = sources["schemas/cxxlens_ng_quality_ownership.yaml"]

    nodes: dict[SurfaceKey, SourceNode] = {}

    for profile in release["profiles"]:
        disposition = (
            "explicit-non-1.0"
            if profile["distribution_requirement"] == "not-a-1.0-blocker"
            else "production-required"
        )
        add_node(nodes, "release.profile", profile["id"], disposition)

    for gate in acceptance["entries"]:
        add_node(nodes, "release.gate", gate["id"], "qualification-evidence")
    for migration in ("R0", "R1", "R2", "R3", "R4"):
        add_node(nodes, "release.migration", migration, "qualification-evidence")
    for migration in release["non_blocking_migrations"]["distribution-1.0"]:
        add_node(nodes, "release.migration", migration, "explicit-non-1.0")

    surface = release["distribution_surface"]
    for target in surface["public_targets"]:
        add_node(nodes, "distribution.target", target["name"], "production-required")
    for package in surface["native_packages"]:
        add_node(nodes, "distribution.native-package", package, "production-required")
    package_qualification = surface["package_qualification"]
    for tool in package_qualification["direct_installed_tools"]:
        add_node(nodes, "distribution.installed-tool", tool, "production-required")
    for configuration in package_qualification["configurations"]:
        for consumer in package_qualification["installed_consumers"]:
            add_node(
                nodes,
                "distribution.consumer-configuration",
                f"{configuration['id']}/{consumer}",
                "qualification-evidence",
            )
    add_node(nodes, "distribution.source-surface", "headers", "production-required")
    add_node(nodes, "distribution.source-surface", "cxx-modules", "explicit-non-1.0")

    for package in catalog["packages"]:
        add_node(nodes, "public.package", package["id"], "production-required")
    for path in catalog["author_paths"]:
        add_node(
            nodes,
            "public.author-path",
            path["id"],
            "qualification-evidence",
            [SurfaceKey("public.catalog-entry", path["entry"])],
        )
    entries = {entry["id"]: entry for entry in catalog["entries"]}
    for entry in catalog["entries"]:
        add_node(nodes, "public.catalog-entry", entry["id"], "production-required")
    public_headers = sorted(
        {
            header
            for record in [*catalog["packages"], *catalog["entries"]]
            for header in record["headers"]
        }
    )
    for header in public_headers:
        owners = [
            SurfaceKey("public.catalog-entry", entry["id"])
            for entry in catalog["entries"]
            if header in entry["headers"]
        ]
        add_node(nodes, "public.header", header, "production-required", owners)

    for callable_row in callables["callables"]:
        owner_id = callable_row["catalog_entry"]
        if owner_id not in entries:
            fail(f"callable {callable_row['id']} has unknown catalog owner {owner_id}")
        owner = SurfaceKey("public.catalog-entry", owner_id)
        add_node(
            nodes,
            "public.callable",
            callable_row["id"],
            "production-required",
            [owner, SurfaceKey("public.header", callable_row["declaring_header"])],
            inherited_from=owner,
        )

    static_headers = {
        PurePosixPath(header).stem
        for entry in catalog["entries"]
        if entry["id"] == "public.relation-static"
        for header in entry["headers"]
    }
    for relation in relations["relations"]:
        descriptor = relation["descriptor_id"]
        relation_key = SurfaceKey("relation.descriptor", descriptor)
        add_node(nodes, "relation.descriptor", descriptor, "production-required")
        stem = relation["name"].replace(".", "_")
        links = [relation_key]
        if stem in static_headers:
            header = f"include/cxxlens/relations/{stem}.hpp"
            links.append(SurfaceKey("public.header", header))
            disposition = "qualification-evidence"
        else:
            disposition = "unresolved-authority"
        add_node(nodes, "relation.static-admission", descriptor, disposition, links)

    for profile, profile_contract in protocol["profiles"].items():
        for feature in profile_contract.get("required", []):
            add_node(
                nodes,
                "provider.profile-feature",
                f"{profile}/{feature}",
                "production-required",
                [SurfaceKey("release.profile", profile)],
            )
    for message in protocol["message_types"]["registry"]:
        add_node(
            nodes,
            "provider.message",
            f"{message['id']}:{message['name']}",
            "production-required",
            [SurfaceKey("release.profile", message["profile"])],
        )
    for execution_id in ("in_process", "out_of_process"):
        if execution_id not in protocol["execution_surfaces"]:
            fail(f"missing provider execution surface {execution_id}")
        add_node(nodes, "provider.execution-surface", execution_id, "qualification-evidence")
    for entry in support["entries"]:
        descriptor = entry["relation"].replace("@", ".v")
        disposition = (
            "explicit-non-1.0"
            if entry["status"] in {"conformance-only", "unsupported"}
            else "qualification-evidence"
        )
        add_node(
            nodes,
            "provider.support-tuple",
            entry["id"],
            disposition,
            [SurfaceKey("relation.descriptor", descriptor)],
        )
    for configuration in release_qualification["package"]["configurations"]:
        for relation in release_qualification["provider"]["relations"]:
            descriptor = relation.replace("@", ".v")
            add_node(
                nodes,
                "provider.production-tuple-template",
                f"{configuration['id']}/{relation}",
                "qualification-evidence",
                [SurfaceKey("relation.descriptor", descriptor)],
            )

    for operator in query["operator_profiles"]:
        add_node(nodes, "query.operator", operator["id"], "production-required")
    for status in query["execution_result"]["statuses"]:
        add_node(nodes, "query.result-status", status, "production-required")
    for backend in query["reference_evaluator"]["source_backends"]:
        add_node(nodes, "query.backend", backend, "qualification-evidence")
    for kind in g5["closure"]["registered_kinds"]:
        add_node(nodes, "g5.closure-kind", kind, "qualification-evidence")

    add_node(nodes, "security.profile", security["schema"], "qualification-evidence")
    for namespace in namespaces["entries"]:
        disposition = (
            "explicit-non-1.0" if namespace["status"] == "conformance-only" else "production-required"
        )
        add_node(nodes, "security.namespace", namespace["id"], disposition)
    for profile in security["sandbox"]["profiles"]:
        add_node(nodes, "security.sandbox-profile", profile["id"], "qualification-evidence")
    for policy in security["sandbox"]["policy_registry"]["policies"]:
        add_node(nodes, "security.sandbox-policy", policy["id"], "qualification-evidence")
    for axis in release["version_axes"]:
        add_node(nodes, "compatibility.axis", axis["id"], "production-required")
    for check in quality["checks"]:
        add_node(nodes, "quality.check", check["id"], "qualification-evidence")

    actual_domains = tuple(sorted({key.domain for key in nodes}))
    if actual_domains != tuple(sorted(DOMAINS)):
        missing = sorted(set(DOMAINS) - set(actual_domains))
        extra = sorted(set(actual_domains) - set(DOMAINS))
        fail(f"closed domain census mismatch: missing={missing}, extra={extra}")
    for key, node in nodes.items():
        for link in node.cross_links:
            if link not in nodes:
                fail(f"dangling typed edge {key.domain}/{key.id} -> {link.domain}/{link.id}")
    return nodes


def census_rows(nodes: dict[SurfaceKey, SourceNode]) -> list[dict[str, Any]]:
    return [
        {
            "domain": key.domain,
            "id": key.id,
            "disposition": node.disposition,
            "cross_links": [
                {"domain": link.domain, "id": link.id} for link in node.cross_links
            ],
            **(
                {
                    "inherited_from": {
                        "domain": node.inherited_from.domain,
                        "id": node.inherited_from.id,
                    }
                }
                if node.inherited_from
                else {}
            ),
        }
        for key, node in sorted(nodes.items())
    ]


def evidence_for_surface(key: SurfaceKey) -> tuple[str, ...]:
    evidence = SURFACE_EVIDENCE_TESTS.get(key, DOMAIN_EVIDENCE_TESTS.get(key.domain))
    if not evidence:
        fail(f"qualified surface has no typed evidence binding: {key.domain}/{key.id}")
    unknown = sorted(set(evidence) - set(EVIDENCE_TEST_PATHS))
    if unknown:
        fail(f"typed evidence binding references unknown CTest IDs: {unknown}")
    return tuple(sorted(evidence))


def validate_evidence_test_registry(root: Path) -> None:
    cmake_path = root / "tests/CMakeLists.txt"
    try:
        cmake = cmake_path.read_text(encoding="utf-8")
    except OSError as error:
        fail(f"cannot read typed CTest registry {cmake_path}: {error}")
    contract_block = re.search(
        r"foreach\(\s*contract\s+IN\s+ITEMS(?P<items>.*?)\)\s*"
        r"add_test\(\s*NAME\s+\"quality\.ng-\$\{contract\}\".*?"
        r"test_ng_\$\{contract\}\.py.*?endforeach\(\)",
        cmake,
        re.DOTALL,
    )
    if contract_block is None:
        fail("tests/CMakeLists.txt omits the canonical quality.ng contract registry")
    registered_contracts = set(
        re.findall(r"\b[a-z][a-z0-9_]*\b", contract_block.group("items"))
    )
    for test_id, path in EVIDENCE_TEST_PATHS.items():
        if test_id == "quality.ownership":
            continue
        prefix = "quality.ng-"
        if not test_id.startswith(prefix):
            fail(f"typed evidence CTest ID has an unsupported registry form: {test_id}")
        contract = test_id.removeprefix(prefix)
        expected_path = f"tests/quality/test_ng_{contract}.py"
        if path != expected_path or contract not in registered_contracts:
            fail(f"typed evidence CTest registration differs: {test_id} -> {path}")
    if re.search(
        r"add_test\(\s*NAME\s+quality\.ownership\s+COMMAND.*?"
        r"tests/quality/test_quality_ownership\.py\"?\s*\)",
        cmake,
        re.DOTALL,
    ) is None:
        fail("tests/CMakeLists.txt omits the exact quality.ownership registration")


def evidence_census_rows(root: Path) -> list[dict[str, Any]]:
    """Return the complete semantic evidence census bound by the static inventory."""

    protocol = load_yaml(root / "schemas/cxxlens_ng_provider_protocol.yaml")
    runtime = load_yaml(root / "schemas/cxxlens_ng_provider_runtime_contract.yaml")
    support = load_yaml(root / "schemas/cxxlens_ng_provider_support_matrix.yaml")
    query = load_yaml(root / "schemas/cxxlens_ng_logical_query_contract.yaml")
    security = load_yaml(root / "schemas/cxxlens_ng_security_profile.yaml")
    g5 = load_yaml(root / "schemas/cxxlens_ng_g5_qualification.yaml")
    release = load_yaml(root / "schemas/cxxlens_ng_release_qualification.yaml")
    acceptance = load_yaml(root / "schemas/cxxlens_ng_acceptance_manifest.yaml")
    callables = load_yaml(root / "schemas/cxxlens_ng_public_callable_inventory.yaml")

    vector_references = {
        "provider": protocol["backend_authority"]["conformance_vectors"],
        "query": query["reference_evaluator"]["conformance_vectors"],
        "security": security["backend_authority"]["vectors"],
    }
    expected_references = dict(
        zip(("provider", "query", "security"), EVIDENCE_CONTRACTS, strict=True)
    )
    vectors: dict[str, list[dict[str, Any]]] = {}
    for name, expected in expected_references.items():
        actual = vector_references[name]
        if actual != expected:
            fail(
                f"{name} conformance-vector authority drift: "
                f"expected {expected}, got {actual}"
            )
        require_repo_path(root, actual)
        document = load_yaml(root / actual)
        payload = document.get("vectors")
        if not isinstance(payload, list):
            fail(f"{actual} omits the vectors census")
        vectors[name] = payload

    validate_evidence_test_registry(root)
    for test_id, path in sorted(EVIDENCE_TEST_PATHS.items()):
        require_repo_path(root, path)
    acceptance_evidence = [
        {"gate": entry["id"], "path": path}
        for entry in acceptance["entries"]
        for path in entry["evidence"]
    ]
    provider_reasons = {
        "state_machine_validation.deterministic_rejections": protocol[
            "state_machine_validation"
        ]["deterministic_rejections"],
        "handshake.failure_reason_codes": protocol["handshake"][
            "failure_reason_codes"
        ],
        "failures.terminal_reasons": protocol["failures"]["terminal_reasons"],
        "provider_runtime.terminal.stable": runtime["terminal"]["stable"],
    }
    typed_evidence = {
        "tests": [
            {"id": test_id, "path": path}
            for test_id, path in sorted(EVIDENCE_TEST_PATHS.items())
        ],
        "domains": [
            {"domain": domain, "tests": list(tests)}
            for domain, tests in sorted(DOMAIN_EVIDENCE_TESTS.items())
        ],
        "surfaces": [
            {
                "domain": key.domain,
                "id": key.id,
                "tests": list(tests),
            }
            for key, tests in sorted(SURFACE_EVIDENCE_TESTS.items())
        ],
        "unconsumed_exact-main_surfaces": [
            {"domain": key.domain, "id": key.id}
            for key in sorted(NIGHTLY_EVIDENCE_SURFACES)
        ],
    }

    def census(census_id: str, payload: Any, count: int | None = None) -> dict[str, Any]:
        if count is None:
            if not isinstance(payload, list):
                fail(f"evidence census {census_id} requires an explicit count")
            count = len(payload)
        return {"id": census_id, "count": count, "digest": digest_value(payload)}

    return [
        census("public-callable-inventory", callables["callables"]),
        census("provider-support-entries", support["entries"]),
        census(
            "provider-reason-codes",
            provider_reasons,
            sum(len(values) for values in provider_reasons.values()),
        ),
        census("provider-conformance-vectors", vectors["provider"]),
        census("query-conformance-vectors", vectors["query"]),
        census("security-qualification-levels", security["qualification"]["levels"]),
        census("security-reason-codes", security["reason_codes"]),
        census("security-conformance-vectors", vectors["security"]),
        census("g5-exact-input-axes", g5["incrementality"]["exact_inputs"]),
        census("g5-required-artifacts", g5["required_artifacts"]),
        census("release-required-artifacts", release["required_artifacts"]),
        census("acceptance-gate-evidence", acceptance_evidence),
        census(
            "typed-evidence-tests",
            typed_evidence,
            len(typed_evidence["tests"])
            + len(typed_evidence["domains"])
            + len(typed_evidence["surfaces"])
            + len(typed_evidence["unconsumed_exact-main_surfaces"]),
        ),
    ]


def load_feedback_records(root: Path) -> dict[str, dict[str, Any]]:
    records: dict[str, dict[str, Any]] = {}
    records_dir = root / "docs/development/implementation-learning/records"
    for path in sorted(records_dir.glob("df-*.md")):
        text = path.read_text(encoding="utf-8")
        if not text.startswith("---\n"):
            continue
        end = text.find("\n---\n", 4)
        if end < 0:
            fail(f"unterminated design-feedback front matter: {path}")
        record = yaml.safe_load(text[4:end])
        if not isinstance(record, dict) or "id" not in record:
            fail(f"invalid design-feedback front matter: {path}")
        feedback_id = record["id"]
        if feedback_id in records:
            fail(f"duplicate design-feedback ID: {feedback_id}")
        records[feedback_id] = record
    return records


def applicable_feedback(root: Path) -> dict[str, dict[str, Any]]:
    return {
        feedback_id: record
        for feedback_id, record in load_feedback_records(root).items()
        if record.get("status") in ACTIVE_BLOCKING_STATUSES
        and record.get("implementation_disposition") == "blocked"
    }


def derived_state(rows: list[dict[str, Any]]) -> tuple[str, str]:
    if not rows:
        fail("derived aggregate has no typed dependency")
    qualifications = {
        row.get("qualification", row.get("derived_qualification")) for row in rows
    }
    if None in qualifications:
        fail("derived aggregate dependency has no qualification state")
    if qualifications & {"tracked-gap", "blocked"}:
        return "included", "tracked-gap"
    if qualifications == {"not-applicable"}:
        return "excluded", "not-applicable"
    if qualifications <= {"qualified", "not-applicable"} and "qualified" in qualifications:
        return "included", "qualified"
    fail(f"derived aggregate has an unsupported child partition: {sorted(qualifications)}")


def derive_aggregate_rows(
    root: Path,
    nodes: dict[SurfaceKey, SourceNode],
    assignments: dict[SurfaceKey, dict[str, Any]],
    rows: dict[SurfaceKey, dict[str, Any]],
    assignable_keys: set[SurfaceKey],
) -> dict[SurfaceKey, dict[str, Any]]:
    """Derive release profiles and gates from their leaf classifications."""

    release = load_yaml(root / "schemas/cxxlens_ng_release_bundle.yaml")
    acceptance = load_yaml(root / "schemas/cxxlens_ng_acceptance_manifest.yaml")

    def make_row(
        key: SurfaceKey,
        dependencies: list[SurfaceKey],
        *,
        forced_state: tuple[str, str] | None = None,
    ) -> dict[str, Any]:
        dependency_rows = [rows[dependency] for dependency in dependencies]
        scope, qualification = forced_state or derived_state(dependency_rows)
        node = nodes[key]
        return {
            "domain": key.domain,
            "id": key.id,
            "kind": "aggregate",
            "disposition": node.disposition,
            "derived_scope": scope,
            "derived_qualification": qualification,
            "cross_links": [
                {"domain": link.domain, "id": link.id} for link in node.cross_links
            ],
            "derived_from": [
                {"domain": dependency.domain, "id": dependency.id}
                for dependency in dependencies
            ],
        }

    for profile in sorted(release["profiles"], key=lambda row: row["id"]):
        key = SurfaceKey("release.profile", profile["id"])
        dependencies = sorted(
            candidate
            for candidate, node in nodes.items()
            if candidate.domain not in AGGREGATE_DOMAINS and key in node.cross_links
        )
        if profile["distribution_requirement"] == "not-a-1.0-blocker":
            rows[key] = make_row(
                key,
                dependencies,
                forced_state=("excluded", "not-applicable"),
            )
        else:
            rows[key] = make_row(key, dependencies)

    gates = {entry["id"]: entry for entry in acceptance["entries"]}

    def derive_gate(gate_id: str, stack: tuple[str, ...] = ()) -> None:
        key = SurfaceKey("release.gate", gate_id)
        if key in rows:
            return
        if gate_id in stack:
            fail(f"acceptance gate dependency cycle: {' -> '.join((*stack, gate_id))}")
        entry = gates.get(gate_id)
        if entry is None:
            fail(f"unknown acceptance gate dependency: {gate_id}")
        dependencies = {
            SurfaceKey("release.gate", dependency)
            for dependency in entry.get("depends_on", [])
        }
        profile_key = SurfaceKey("release.profile", entry["profile"])
        if profile_key in nodes:
            dependencies.add(profile_key)
        if gate_id == "gate.quality-evidence":
            dependencies.update(
                candidate for candidate in rows if candidate.domain == "quality.check"
            )
        elif gate_id == "gate.g5":
            dependencies.update(
                candidate for candidate in rows if candidate.domain == "g5.closure-kind"
            )
            dependencies.update(
                {
                    SurfaceKey("public.catalog-entry", "public.incremental"),
                    SurfaceKey("release.migration", "R4"),
                }
            )
        elif gate_id == "gate.release":
            dependencies.update(
                candidate
                for candidate in assignable_keys
                if assignments[candidate]["scope"] != "excluded"
            )
        for dependency in sorted(dependencies):
            if dependency.domain == "release.gate":
                derive_gate(dependency.id, (*stack, gate_id))
            elif dependency not in rows:
                fail(
                    f"derived gate dependency is outside the classified inventory: "
                    f"{dependency.domain}/{dependency.id}"
                )
        ordered = sorted(dependencies)
        rows[key] = make_row(key, ordered)

    for gate_id in sorted(gates):
        derive_gate(gate_id)
    return rows


def validate_repository(root: Path | str) -> ValidatedModel:
    """Validate the repository contract and return the stable expanded model."""

    root = Path(root).resolve()
    manifest = load_yaml(root / MANIFEST)
    validate_schema(manifest, root / MANIFEST_SCHEMA)
    if tuple(manifest["source_contracts"]) != SOURCE_CONTRACTS:
        fail("manifest source_contracts does not equal the closed source set")
    for source in SOURCE_CONTRACTS:
        require_repo_path(root, source)

    nodes = derive_inventory(root)
    evidence_censuses = evidence_census_rows(root)
    evidence_census_digest = digest_value(evidence_censuses)
    census_digest = digest_value(
        {"nodes": census_rows(nodes), "evidence_censuses": evidence_censuses}
    )
    if manifest["authority_census_digest"] != census_digest:
        fail(
            "stale authority census digest: "
            f"expected {census_digest}, got {manifest['authority_census_digest']}"
        )

    explicit_nodes = {
        key
        for key, node in nodes.items()
        if node.inherited_from is None and key.domain not in AGGREGATE_DOMAINS
    }
    assignments: dict[SurfaceKey, dict[str, Any]] = {}
    assignment_ids: set[str] = set()
    for assignment in manifest["assignments"]:
        if assignment["id"] in assignment_ids:
            fail(f"duplicate assignment ID: {assignment['id']}")
        assignment_ids.add(assignment["id"])
        pair = (assignment["scope"], assignment["qualification"])
        if pair not in VALID_PAIRS:
            fail(f"invalid scope/qualification pair in {assignment['id']}: {pair}")
        if "exclusion" in assignment:
            require_repo_path(root, assignment["exclusion"]["authority"])
        surfaces = [SurfaceKey(row["domain"], row["id"]) for row in assignment["surfaces"]]
        if surfaces != sorted(surfaces):
            fail(f"surfaces are not in typed canonical order: {assignment['id']}")
        if any(key.domain in AGGREGATE_DOMAINS for key in surfaces):
            fail(f"aggregate surface must be derived, not assigned: {assignment['id']}")
        if assignment["qualification"] == "qualified":
            unconsumed = sorted(set(surfaces) & NIGHTLY_EVIDENCE_SURFACES)
            if unconsumed:
                fail(
                    "qualified assignment uses evidence not consumed by the exact-main "
                    "release evaluator: "
                    + ", ".join(f"{key.domain}/{key.id}" for key in unconsumed)
                )
            expected_evidence = sorted(
                {
                    test_id
                    for key in surfaces
                    for test_id in evidence_for_surface(key)
                }
            )
            if assignment.get("evidence") != expected_evidence:
                fail(
                    f"typed evidence mismatch in {assignment['id']}: "
                    f"expected={expected_evidence}, actual={assignment.get('evidence')}"
                )
        for key in surfaces:
            if key.domain == "public.callable":
                fail("public.callable assignment must be inherited, not copied into the manifest")
            if key not in nodes:
                fail(f"assignment references unknown surface: {key.domain}/{key.id}")
            if key in assignments:
                fail(f"duplicate surface ownership: {key.domain}/{key.id}")
            node = nodes[key]
            qualification = assignment["qualification"]
            if node.disposition in {"production-required", "qualification-evidence"}:
                if assignment["scope"] != "included" or qualification not in {"qualified", "tracked-gap"}:
                    fail(f"unsupported classification for included surface {key.domain}/{key.id}")
            elif node.disposition == "explicit-non-1.0":
                if pair != ("excluded", "not-applicable"):
                    fail(f"unsupported exclusion state for {key.domain}/{key.id}")
            elif node.disposition == "unresolved-authority":
                if pair != ("unresolved", "blocked"):
                    fail(f"unresolved authority must remain blocked: {key.domain}/{key.id}")
            assignments[key] = assignment
    missing = sorted(explicit_nodes - set(assignments))
    extra = sorted(set(assignments) - explicit_nodes)
    if missing or extra:
        fail(
            "assignable partition is not exact: "
            f"missing={[f'{x.domain}/{x.id}' for x in missing]}, "
            f"extra={[f'{x.domain}/{x.id}' for x in extra]}"
        )

    records = applicable_feedback(root)
    referenced_feedback = {
        feedback
        for assignment in manifest["assignments"]
        if assignment["qualification"] in {"tracked-gap", "blocked"}
        for feedback in assignment.get("feedback", [])
    }
    exclusions: set[str] = set()
    if manifest["feedback_exclusions"]:
        fail(
            "v1 has no independent typed affected-surface authority for feedback exclusions; "
            "feedback_exclusions must remain empty"
        )
    for exclusion in manifest["feedback_exclusions"]:
        feedback = exclusion["feedback"]
        require_repo_path(root, exclusion["authority"])
        for row in exclusion["surfaces"]:
            key = SurfaceKey(row["domain"], row["id"])
            if key not in nodes or nodes[key].disposition != "explicit-non-1.0":
                fail(f"feedback exclusion is not proved by explicit-non-1.0: {key.domain}/{key.id}")
            if assignments[key]["qualification"] != "not-applicable":
                fail(f"feedback exclusion surface is not excluded: {key.domain}/{key.id}")
        exclusions.add(feedback)
    unknown_feedback = sorted((referenced_feedback | exclusions) - set(records))
    if unknown_feedback:
        fail(f"manifest references feedback outside applicable blocking census: {unknown_feedback}")
    unmapped = sorted(set(records) - referenced_feedback - exclusions)
    if unmapped:
        fail(f"applicable blocking design feedback is not mapped: {unmapped}")

    classified: dict[SurfaceKey, dict[str, Any]] = {}
    for key, node in sorted(nodes.items()):
        if key.domain in AGGREGATE_DOMAINS:
            continue
        assignment = assignments[node.inherited_from or key]
        row = {
            "domain": key.domain,
            "id": key.id,
            "kind": "leaf",
            "disposition": node.disposition,
            "assignment": assignment["id"],
            "owner_issue": assignment["owner_issue"],
            "scope": assignment["scope"],
            "qualification": assignment["qualification"],
            "cross_links": [
                {"domain": link.domain, "id": link.id} for link in node.cross_links
            ],
        }
        if node.inherited_from:
            row["inherited_from"] = {
                "domain": node.inherited_from.domain,
                "id": node.inherited_from.id,
            }
        if assignment["qualification"] == "qualified":
            row["evidence"] = list(evidence_for_surface(key))
        classified[key] = row

    derive_aggregate_rows(root, nodes, assignments, classified, explicit_nodes)
    if set(classified) != set(nodes):
        missing_rows = sorted(set(nodes) - set(classified))
        extra_rows = sorted(set(classified) - set(nodes))
        fail(
            "expanded inventory is not exact after aggregate derivation: "
            f"missing={missing_rows}, extra={extra_rows}"
        )
    expanded = [classified[key] for key in sorted(classified)]
    classification_rows = [
        {
            field: row[field]
            for field in (
                "domain",
                "id",
                "kind",
                "assignment",
                "owner_issue",
                "scope",
                "qualification",
                "evidence",
                "derived_scope",
                "derived_qualification",
                "derived_from",
            )
            if field in row
        }
        for row in expanded
    ]

    counts = {name: 0 for name in ("qualified", "tracked_gap", "blocked", "not_applicable")}
    for row in expanded:
        if row["kind"] == "aggregate":
            continue
        counts[row["qualification"].replace("-", "_")] += 1
    summary = {
        "domain_count": len(DOMAINS),
        "assignable_count": len(explicit_nodes),
        "expanded_count": len(expanded),
        "aggregate_count": sum(row["kind"] == "aggregate" for row in expanded),
        **counts,
    }
    closure_status = (
        "qualified" if counts["tracked_gap"] == 0 and counts["blocked"] == 0 else "classified-with-gaps"
    )
    evidence_tests = tuple(
        sorted(
            {
                test_id
                for row in expanded
                for test_id in row.get("evidence", [])
            }
        )
    )
    return ValidatedModel(
        root=root,
        manifest=manifest,
        nodes=nodes,
        assignments=assignments,
        expanded=expanded,
        blocking_feedback=tuple(sorted(records)),
        authority_census_digest=census_digest,
        evidence_census_digest=evidence_census_digest,
        classification_digest=digest_value(classification_rows),
        summary=summary,
        closure_status=closure_status,
        evidence_censuses=evidence_censuses,
        evidence_tests=evidence_tests,
    )


def binding_from_model(model: ValidatedModel) -> dict[str, Any]:
    return {
        "manifest_path": MANIFEST.as_posix(),
        "manifest_digest": digest_file(model.root / MANIFEST),
        "authority_census_digest": model.authority_census_digest,
        "evidence_census_digest": model.evidence_census_digest,
        "classification_digest": model.classification_digest,
        "evidence_tests": list(model.evidence_tests),
        "summary": model.summary,
        "closure_status": model.closure_status,
    }


def inventory_binding(root: Path | str) -> dict[str, Any]:
    """Return the static Wave-0/readiness binding; never consumes G5, evaluation, or GR."""

    return binding_from_model(validate_repository(root))


def git_output(root: Path, *args: str) -> str:
    completed = subprocess.run(
        ["git", *args], cwd=root, text=True, capture_output=True, check=False
    )
    if completed.returncode:
        fail(f"git {' '.join(args)} failed: {completed.stderr.strip()}")
    return completed.stdout.strip()


def exact_main_state(root: Path) -> dict[str, Any]:
    revision = git_output(root, "rev-parse", "HEAD")
    tree = git_output(root, "rev-parse", "HEAD^{tree}")
    branch = git_output(root, "branch", "--show-current")
    clean = not git_output(root, "status", "--porcelain=v1", "--untracked-files=all")
    if branch != "main" or not clean:
        fail(f"report requires clean exact main; branch={branch!r}, clean={clean}")
    return {"revision": revision, "tree": tree, "branch": branch, "clean": clean}


def require_expected_revision(git: dict[str, Any], expected_revision: str) -> None:
    if git.get("revision") != expected_revision:
        fail(
            "checked-out revision does not match --expected-revision: "
            f"expected {expected_revision}, got {git.get('revision')}"
        )


def require_out_of_tree(root: Path, output: Path) -> None:
    try:
        output.resolve().relative_to(root.resolve())
    except ValueError:
        return
    fail("terminal production-scope report must be written out of tree")


def validate_upstream_git(report: dict[str, Any], git: dict[str, Any], name: str) -> None:
    upstream_git = report.get("git")
    if not isinstance(upstream_git, dict):
        fail(f"{name} has no typed git binding")
    for field in ("revision", "tree", "branch", "clean"):
        if upstream_git.get(field) != git[field]:
            fail(f"{name} {field} does not match exact main")


def evaluation_qualification(evaluation: dict[str, Any]) -> str:
    qualification = evaluation.get("qualification")
    if qualification not in {"qualified", "not-qualified"}:
        fail("release evaluation qualification is not qualified/not-qualified")
    if evaluation.get("qualified") is not (qualification == "qualified"):
        fail("release evaluation qualification and qualified flag disagree")
    return qualification


def validate_gr_templates(model: ValidatedModel, gr: dict[str, Any]) -> None:
    expected = {
        key.id
        for key in model.nodes
        if key.domain == "provider.production-tuple-template"
    }
    actual: list[str] = []
    for row in gr.get("production_support", []):
        platform = row.get("platform", "")
        if platform.endswith("-static"):
            configuration = "static"
        elif platform.endswith("-shared"):
            configuration = "shared"
        else:
            fail(f"GR production platform does not identify static/shared template: {platform}")
        actual.append(f"{configuration}/{row.get('relation')}")
    if len(actual) != len(set(actual)):
        fail("GR contains duplicate production tuple templates")
    if set(actual) != expected:
        fail(f"GR production tuple templates mismatch: expected={sorted(expected)}, actual={sorted(actual)}")


def validate_report_exactness(model: ValidatedModel, report: dict[str, Any]) -> None:
    """Re-derive every static report section and reject key-level duplication or drift."""

    surfaces = report.get("surfaces")
    if not isinstance(surfaces, list):
        fail("production-scope report surfaces are not an array")
    keys = [(row.get("domain"), row.get("id")) for row in surfaces]
    if len(keys) != len(set(keys)):
        fail("production-scope report contains duplicate typed surface keys")
    expected_keys = [(row["domain"], row["id"]) for row in model.expanded]
    if keys != expected_keys or surfaces != model.expanded:
        fail("production-scope report surfaces differ from the derived canonical inventory")

    expected_domains = [
        {
            "id": domain,
            "count": len(rows := [row for row in model.expanded if row["domain"] == domain]),
            "digest": digest_value(rows),
        }
        for domain in DOMAINS
    ]
    if report.get("domains") != expected_domains:
        fail("production-scope report domain census differs from derived surfaces")
    if report.get("summary") != model.summary:
        fail("production-scope report summary differs from the derived leaf census")
    if report.get("closure_status") != model.closure_status:
        fail("production-scope report closure status differs from the leaf partition")
    if report.get("evidence_censuses") != model.evidence_censuses:
        fail("production-scope report evidence censuses differ from accepted authorities")

    expected_binding = {
        "manifest_path": MANIFEST.as_posix(),
        "manifest_digest": digest_file(model.root / MANIFEST),
        "manifest_schema_digest": digest_file(model.root / MANIFEST_SCHEMA),
        "report_schema_digest": digest_file(model.root / REPORT_SCHEMA),
        "checker_digest": digest_file(model.root / CHECKER),
        "source_digests": [
            {"path": path, "digest": digest_file(model.root / path)}
            for path in SOURCE_CONTRACTS
        ],
        "authority_census_digest": model.authority_census_digest,
        "evidence_census_digest": model.evidence_census_digest,
        "classification_digest": model.classification_digest,
    }
    if report.get("binding") != expected_binding:
        fail("production-scope report static binding differs from exact repository bytes")


def build_report(
    model: ValidatedModel,
    *,
    mode: str,
    evaluation: dict[str, Any],
    git: dict[str, Any],
    run_url: str,
    generated_at: str,
    evaluation_digest: str,
    gr: dict[str, Any] | None = None,
    gr_digest: str | None = None,
) -> dict[str, Any]:
    """Build the terminal report from upstream evidence without adding a reverse edge."""

    if mode not in {"normal", "final"}:
        fail(f"unsupported report mode: {mode}")
    if mode == "normal" and model.closure_status != "classified-with-gaps":
        fail("normal mode requires explicit gaps; qualified closure requires final mode")
    parsed_url = urllib.parse.urlsplit(run_url)
    if parsed_url.scheme not in {"http", "https"} or not parsed_url.netloc:
        fail("terminal report run_url must be an absolute HTTP(S) URI")
    try:
        timestamp = dt.datetime.fromisoformat(generated_at.replace("Z", "+00:00"))
    except ValueError:
        fail("terminal report generated_at must be an RFC 3339 date-time")
    if timestamp.tzinfo is None:
        fail("terminal report generated_at must include a timezone")
    validate_schema(evaluation, model.root / EVALUATION_SCHEMA)
    validate_upstream_git(evaluation, git, "release evaluation")
    if evaluation.get("scope_inventory") != binding_from_model(model):
        fail("release evaluation scope_inventory does not match terminal inventory binding")
    evaluation_state = evaluation_qualification(evaluation)
    production_support = evaluation.get("production_support", [])
    if production_support:
        fail("release evaluation must not own production tuples; exact tuples belong only to GR")
    if model.closure_status == "classified-with-gaps":
        if evaluation_state != "not-qualified" or production_support:
            fail("gap closure requires not-qualified evaluation with no production tuples")
    elif evaluation_state != "qualified":
        fail("qualified closure requires qualified release evaluation")

    if not isinstance(evaluation_digest, str) or not evaluation_digest.startswith("sha256:"):
        fail("release evaluation artifact byte digest is required")
    upstream = {"release_evaluation_digest": evaluation_digest}
    if mode == "final":
        if model.closure_status != "qualified" or model.blocking_feedback:
            fail("final mode forbids tracked gaps, unresolved authority, and blocking feedback")
        if evaluation_state != "qualified" or gr is None:
            fail("final mode requires qualified evaluation and exact GR")
        validate_schema(gr, model.root / GR_REPORT_SCHEMA)
        validate_upstream_git(gr, git, "release qualification")
        if gr["prerequisites"].get("release_evaluation_report_digest") != evaluation_digest:
            fail("release qualification does not bind the exact evaluation artifact")
        validate_gr_templates(model, gr)
        if not isinstance(gr_digest, str) or not gr_digest.startswith("sha256:"):
            fail("final GR artifact byte digest is required")
        upstream["release_qualification_digest"] = gr_digest
    elif gr is not None:
        fail("normal mode must not consume GR; GR exists only after qualified evaluation")

    source_digests = [
        {"path": path, "digest": digest_file(model.root / path)} for path in SOURCE_CONTRACTS
    ]
    domains = []
    for domain in DOMAINS:
        rows = [row for row in model.expanded if row["domain"] == domain]
        domains.append({"id": domain, "count": len(rows), "digest": digest_value(rows)})
    findings = sorted(
        {
            assignment["gap"]["finding"]
            for assignment in model.manifest["assignments"]
            if "gap" in assignment
        }
    )
    report = {
        "schema": "cxxlens.ng-production-scope-closure-report.v1",
        "result": "passed",
        "generated_at": generated_at,
        "run_url": run_url,
        "mode": mode,
        "closure_status": model.closure_status,
        "git": git,
        "binding": {
            "manifest_path": MANIFEST.as_posix(),
            "manifest_digest": digest_file(model.root / MANIFEST),
            "manifest_schema_digest": digest_file(model.root / MANIFEST_SCHEMA),
            "report_schema_digest": digest_file(model.root / REPORT_SCHEMA),
            "checker_digest": digest_file(model.root / CHECKER),
            "source_digests": source_digests,
            "authority_census_digest": model.authority_census_digest,
            "evidence_census_digest": model.evidence_census_digest,
            "classification_digest": model.classification_digest,
        },
        "summary": model.summary,
        "domains": domains,
        "surfaces": model.expanded,
        "blocking_feedback": list(model.blocking_feedback),
        "evidence_censuses": model.evidence_censuses,
        "upstream": upstream,
        "findings": findings,
    }
    validate_schema(report, model.root / REPORT_SCHEMA)
    validate_report_exactness(model, report)
    return report


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", dest="global_root", type=Path)
    subparsers = parser.add_subparsers(dest="command", required=True)
    check = subparsers.add_parser("check")
    check.add_argument("--root", dest="command_root", type=Path)
    report = subparsers.add_parser("report")
    report.add_argument("--root", dest="command_root", type=Path)
    report.add_argument("--mode", choices=("normal", "final"), required=True)
    report.add_argument("--evaluation", type=Path, required=True)
    report.add_argument("--gr", type=Path)
    report.add_argument("--expected-revision", required=True)
    report.add_argument("--output", type=Path, required=True)
    report.add_argument("--run-url", required=True)
    report.add_argument("--generated-at")
    arguments = parser.parse_args(argv)
    arguments.root = (
        arguments.command_root
        or arguments.global_root
        or Path(__file__).resolve().parents[2]
    )
    return arguments


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    try:
        model = validate_repository(args.root)
        if args.command == "check":
            print(
                "production scope closure: "
                f"{model.closure_status}; {model.summary['expanded_count']} nodes; "
                f"census {model.authority_census_digest}"
            )
            return 0
        require_out_of_tree(model.root, args.output)
        git = exact_main_state(model.root)
        require_expected_revision(git, args.expected_revision)
        evaluation = load_json(args.evaluation)
        gr = load_json(args.gr) if args.gr else None
        generated_at = args.generated_at or dt.datetime.now(dt.timezone.utc).isoformat()
        report = build_report(
            model,
            mode=args.mode,
            evaluation=evaluation,
            git=git,
            gr=gr,
            evaluation_digest=digest_file(args.evaluation),
            gr_digest=digest_file(args.gr) if args.gr else None,
            run_url=args.run_url,
            generated_at=generated_at,
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return 0
    except ContractError as error:
        print(f"production scope closure: ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
