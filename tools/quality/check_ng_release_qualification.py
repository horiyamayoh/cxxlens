#!/usr/bin/env python3
"""Validate and materialize the exact-SHA distribution 1.0 GR report."""

from __future__ import annotations

import argparse
import contextlib
import datetime
import hashlib
import importlib
import json
import os
import pathlib
import platform
import re
import stat
import subprocess
import sys
import xml.etree.ElementTree as ET
from typing import Any

import jsonschema
import yaml

import check_ng_clang22_materialization as materialization
import check_ng_sqlite_store_v3_qualification as sqlite_qualification
import public_callable_inventory as callable_inventory


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = pathlib.Path("schemas/cxxlens_ng_release_qualification.yaml")
MANIFEST_SCHEMA = pathlib.Path("schemas/cxxlens_ng_release_qualification.schema.yaml")
REPORT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_release_qualification_report.schema.yaml")
EVALUATION_REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_release_qualification_evaluation_report.schema.yaml"
)
INSTALL_SCHEMA = pathlib.Path("schemas/cxxlens_ng_install_artifact_manifest.schema.yaml")
G5_REPORT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_g5_qualification_report.schema.yaml")
FOUNDATION_REPORT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_foundation_completion_report.schema.yaml")
READINESS_REPORT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_api_development_readiness_report.schema.yaml")
CALLABLE_INVENTORY = pathlib.Path("schemas/cxxlens_ng_public_callable_inventory.yaml")
CALLABLE_INVENTORY_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_public_callable_inventory.schema.yaml"
)
CALLABLE_REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_public_callable_inventory_report.schema.yaml"
)
SECURITY_REPORT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_security_conformance_report.schema.yaml")
ACCEPTANCE = pathlib.Path("schemas/cxxlens_ng_acceptance_manifest.yaml")
RELEASE = pathlib.Path("schemas/cxxlens_ng_release_bundle.yaml")
SUPPORT = pathlib.Path("schemas/cxxlens_ng_provider_support_matrix.yaml")
SECURITY = pathlib.Path("schemas/cxxlens_ng_security_profile.yaml")
REGISTRY = pathlib.Path("schemas/cxxlens_ng_relation_registry.yaml")
MATERIALIZATION_CONTRACT = pathlib.Path(
    "schemas/cxxlens_ng_clang22_materialization_contract.yaml"
)
MATERIALIZATION_CONTRACT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml"
)
MATERIALIZATION_REQUEST_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_clang22_materialization_request.schema.yaml"
)
MATERIALIZATION_REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_clang22_materialization_report.schema.yaml"
)
MATERIALIZATION_EXECUTION_RECEIPT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_clang22_materialization_execution_receipt.schema.yaml"
)
MATERIALIZATION_OCCURRENCE_MANIFEST_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_clang22_materializer_occurrence_manifest.schema.yaml"
)
SQLITE_STORE_CONTRACT = pathlib.Path("schemas/cxxlens_ng_sqlite_store_contract.yaml")
SQLITE_STORE_V3_QUALIFICATION_REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_sqlite_store_v3_qualification_report.schema.yaml"
)
MATERIALIZATION_OCCURRENCE_MANIFEST_PATH = (
    "share/cxxlens/materialization/clang22/occurrence-v1.json"
)
MATERIALIZATION_REQUEST_VERSION = "2.1.0"
MATERIALIZATION_PROTOCOL_MINOR = 1
MATERIALIZATION_REQUIRED_FEATURES = ["task-input-chunks-v1"]
RELEASE_AUTHORITY_PATHS = (
    MANIFEST,
    MATERIALIZATION_CONTRACT,
    MATERIALIZATION_CONTRACT_SCHEMA,
    MATERIALIZATION_REQUEST_SCHEMA,
    MATERIALIZATION_REPORT_SCHEMA,
    MATERIALIZATION_EXECUTION_RECEIPT_SCHEMA,
    MATERIALIZATION_OCCURRENCE_MANIFEST_SCHEMA,
    SQLITE_STORE_CONTRACT,
    SQLITE_STORE_V3_QUALIFICATION_REPORT_SCHEMA,
    RELEASE,
    ACCEPTANCE,
    SUPPORT,
    SECURITY,
    REGISTRY,
    CALLABLE_INVENTORY,
    CALLABLE_INVENTORY_SCHEMA,
    CALLABLE_REPORT_SCHEMA,
)
MATERIALIZATION_ARTIFACT_MAX_BYTES = 1073741824
MATERIALIZATION_RECEIPT_MAX_BYTES = 65536
MATERIALIZATION_MATRIX = (
    ("static", "memory"),
    ("static", "sqlite"),
    ("shared", "memory"),
    ("shared", "sqlite"),
)
MATERIALIZATION_TOOL_FILE = "bin/cxxlens-clang22-materialize"
MATERIALIZATION_ASSIGNMENT_ID = "scope.clang22-installed-adoption-gap"
MATERIALIZATION_ASSIGNMENT_OWNER = "#181"
MATERIALIZATION_ASSIGNMENT_FEEDBACK = (
    "DF-0182",
    "DF-0187",
    "DF-0191",
    "DF-0192",
    "DF-0195",
    "DF-0196",
    "DF-0197",
    "DF-0198",
    "DF-0199",
    "DF-0200",
)
MATERIALIZATION_ASSIGNMENT_SURFACES = (
    ("distribution.consumer-configuration", "shared/real-project"),
    ("distribution.consumer-configuration", "static/real-project"),
    ("distribution.installed-tool", "cxxlens-clang22-materialize"),
    ("distribution.native-package", "cxxlens-clang22-materialize"),
    ("provider.production-tuple-template", "shared/cc.call_direct_target@1"),
    ("provider.production-tuple-template", "shared/cc.call_site@1"),
    ("provider.production-tuple-template", "shared/cc.entity@1"),
    ("provider.production-tuple-template", "static/cc.call_direct_target@1"),
    ("provider.production-tuple-template", "static/cc.call_site@1"),
    ("provider.production-tuple-template", "static/cc.entity@1"),
    ("provider.profile-feature", "NG0/task-input-chunks-v1"),
    ("relation.descriptor", "frontend.clang22.call_observation.v2"),
    ("relation.descriptor", "frontend.clang22.entity_observation.v2"),
    ("relation.descriptor", "frontend.clang22.type_observation.v2"),
)
MATERIALIZATION_ASSIGNMENT_EVIDENCE = (
    "quality.ng-clang22_materialization",
    "quality.ng-relation_contract",
    "quality.ng-release_qualification",
)
MATERIALIZATION_BASE_CLAIM_DESCRIPTORS = (
    "build.project.v1",
    "build.toolchain_context.v1",
    "build.variant.v1",
    "source.file.v1",
    "build.compile_unit.v1",
    "source.span.v1",
)
MATERIALIZATION_TASK_EXECUTION_KEY_FIELDS = (
    "provider_task_id",
    "task_input_digest",
    "provider_execution_id",
)
SQLITE_STORE_V3_QUALIFICATION_REPORT_FILENAME = (
    "cxxlens-ng-sqlite-store-v3-qualification-report.json"
)
SQLITE_STORE_V3_QUALIFICATION_REPORT_MAX_BYTES = 16_777_216


class ReleaseQualificationError(ValueError):
    """A release qualification invariant is not satisfied."""


def fail(message: str) -> None:
    raise ReleaseQualificationError(message)


def load(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8")) if path.suffix == ".json" else yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, yaml.YAMLError) as error:
        fail(f"cannot load {path}: {error}")
    if not isinstance(value, dict):
        fail(f"expected mapping: {path}")
    return value


def read_bounded_artifact(
    path: pathlib.Path, label: str, maximum_bytes: int
) -> bytes:
    """Open once, fstat before reading, and retain only one bounded byte occurrence."""

    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(
        os, "O_NOFOLLOW", 0
    )
    descriptor = -1
    try:
        descriptor = os.open(path, flags)
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode):
            fail(f"{label} is not a regular artifact: {path}")
        expected_size = before.st_size
        if expected_size > maximum_bytes:
            fail(
                f"{label} exceeds the {maximum_bytes}-byte qualification limit: "
                f"{path}: {expected_size}"
            )
        with os.fdopen(descriptor, "rb", closefd=True) as stream:
            descriptor = -1
            raw = stream.read(maximum_bytes + 1)
            after = os.fstat(stream.fileno())
    except OSError as error:
        fail(f"cannot read {label} {path}: {error}")
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    if len(raw) > maximum_bytes:
        fail(f"{label} grew beyond the {maximum_bytes}-byte qualification limit: {path}")
    if len(raw) != expected_size:
        fail(
            f"{label} changed while qualification was reading it: "
            f"{path}: stat={expected_size}, read={len(raw)}"
        )
    before_identity = (
        before.st_dev,
        before.st_ino,
        before.st_size,
        before.st_mtime_ns,
        before.st_ctime_ns,
    )
    after_identity = (
        after.st_dev,
        after.st_ino,
        after.st_size,
        after.st_mtime_ns,
        after.st_ctime_ns,
    )
    if after_identity != before_identity:
        fail(f"{label} changed while qualification was reading it: {path}")
    try:
        current = os.stat(path, follow_symlinks=False)
    except OSError as error:
        fail(f"cannot restat {label} {path}: {error}")
    if not stat.S_ISREG(current.st_mode) or (
        current.st_dev,
        current.st_ino,
        current.st_size,
        current.st_mtime_ns,
        current.st_ctime_ns,
    ) != before_identity:
        fail(f"{label} path changed while qualification was reading it: {path}")
    return raw


def load_materialization_json(
    path: pathlib.Path, label: str
) -> tuple[dict[str, Any], bytes]:
    """Load one request/report with the specialization's strict byte policy."""

    raw = read_bounded_artifact(path, label, MATERIALIZATION_ARTIFACT_MAX_BYTES)
    try:
        value = materialization.load_strict_json_bytes(raw, f"{label} {path}")
    except materialization.MaterializationError as error:
        fail(f"{label} is not strict materialization JSON: {error}")
    return value, raw


def load_execution_receipt(
    path: pathlib.Path,
    schema: dict[str, Any],
) -> tuple[dict[str, Any], bytes]:
    """Load and validate one external materializer process receipt."""

    raw = read_bounded_artifact(
        path, "materialization execution receipt", MATERIALIZATION_RECEIPT_MAX_BYTES
    )
    try:
        value = materialization.load_strict_json_bytes(
            raw, f"materialization execution receipt {path}"
        )
    except materialization.MaterializationError as error:
        fail(f"materialization execution receipt is not strict JSON: {error}")
    validate_schema(
        value,
        schema,
        f"materialization execution receipt {path}",
    )
    return value, raw


def validate_schema(value: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(
            schema, format_checker=jsonschema.Draft202012Validator.FORMAT_CHECKER
        ).validate(value)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"{label} schema validation failed: {error.message}")


def validate_release_materialization_request_machine(request: dict[str, Any]) -> None:
    tool = request.get("tool")
    worker = request.get("worker")
    trust = request.get("trust_policy")
    if not all(isinstance(value, dict) for value in (tool, worker, trust)):
        fail("release materialization request machine projection is malformed")
    legacy_fields = {"prefix_manifest_digest", "relocated_prefix_digest"}
    if legacy_fields.intersection(tool):
        fail("release materialization request retains legacy prefix digest authority")
    if (
        request.get("schema") != "cxxlens.clang22-materialization-request.v2"
        or request.get("request_version") != MATERIALIZATION_REQUEST_VERSION
        or tool.get("interface_version") != MATERIALIZATION_REQUEST_VERSION
        or "occurrence_manifest_digest" not in tool
        or worker.get("protocol_major") != 1
        or worker.get("protocol_minor") != MATERIALIZATION_PROTOCOL_MINOR
        or worker.get("required_features") != MATERIALIZATION_REQUIRED_FEATURES
        or trust.get("protocol_major") != 1
        or trust.get("protocol_minor") != MATERIALIZATION_PROTOCOL_MINOR
        or trust.get("required_features") != MATERIALIZATION_REQUIRED_FEATURES
    ):
        fail("release materialization request machine 2.1 projection differs")


def validate_release_occurrence_binding(
    request: dict[str, Any],
    report: dict[str, Any],
    install: dict[str, Any],
    git: dict[str, Any],
    label: str,
) -> None:
    installation = report["installation"]
    if {"prefix_manifest_digest", "relocated_prefix_digest"}.intersection(
        installation
    ):
        fail("materialization report retains legacy prefix digest authority")
    file_rows = [
        row
        for row in install["files"]
        if row["path"] == MATERIALIZATION_OCCURRENCE_MANIFEST_PATH
    ]
    if len(file_rows) != 1:
        fail(f"installed occurrence manifest census differs: {label}")
    occurrence_manifest_digest = file_rows[0]["digest"]
    files = {row["path"]: row["digest"] for row in install["files"]}
    tool_digest = files.get("bin/cxxlens-clang22-materialize")
    worker_digest = files.get("bin/cxxlens-clang-worker-22")
    requested = installation["requested"]
    measured = installation["measured"]
    configuration = request["tool"]["package_configuration"]
    if (
        request["tool"]["occurrence_manifest_digest"]
        != occurrence_manifest_digest
        or request["tool"]["installed_executable_digest"] != tool_digest
        or request["worker"]["installed_binary_digest"] != worker_digest
        or requested
        != {"occurrence_manifest_digest": occurrence_manifest_digest}
        or measured["manifest_path"] != MATERIALIZATION_OCCURRENCE_MANIFEST_PATH
        or measured["manifest_file_digest"] != occurrence_manifest_digest
        or measured["source_revision"] != git["revision"]
        or measured["source_tree"] != git["tree"]
        or measured["configuration"] != configuration
        or measured["tool"]
        != {"path": "bin/cxxlens-clang22-materialize", "digest": tool_digest}
        or measured["worker"]
        != {"path": "bin/cxxlens-clang-worker-22", "digest": worker_digest}
    ):
        fail(f"materialization installed occurrence binding differs: {label}")


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest(path: pathlib.Path) -> str:
    return digest_bytes(path.read_bytes())


def canonical_digest(value: Any) -> str:
    return digest_bytes(json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode())


def git_output(root: pathlib.Path, *arguments: str) -> str:
    completed = subprocess.run(["git", "-C", str(root), *arguments], check=False, capture_output=True, text=True)
    if completed.returncode != 0:
        fail(f"git {' '.join(arguments)} failed: {completed.stderr.strip()}")
    return completed.stdout.strip()


def git_state(root: pathlib.Path) -> dict[str, Any]:
    return {
        "revision": git_output(root, "rev-parse", "HEAD"),
        "tree": git_output(root, "rev-parse", "HEAD^{tree}"),
        "branch": git_output(root, "branch", "--show-current"),
        "clean": git_output(root, "status", "--porcelain=v1") == "",
    }


def production_scope_inventory_binding(root: pathlib.Path) -> dict[str, Any]:
    """Recompute the static production-scope inventory/classification binding."""

    try:
        scope_checker = importlib.import_module("check_ng_production_scope_closure")
    except ImportError as error:
        fail(f"production scope checker is unavailable: {error}")
    helper = getattr(scope_checker, "inventory_binding", None)
    if not callable(helper):
        fail("production scope checker omits inventory_binding(root)")
    contract_error = getattr(scope_checker, "ContractError", None)
    if not isinstance(contract_error, type) or not issubclass(
        contract_error, Exception
    ):
        fail("production scope checker omits ContractError")
    try:
        value = helper(root)
    except contract_error as error:
        fail(f"production scope inventory evaluation failed: {error}")
    except (OSError, ValueError) as error:
        fail(f"production scope inventory evaluation failed: {error}")
    if not isinstance(value, dict):
        fail("production scope inventory binding is not a mapping")
    return value


def verify_production_scope_inventory(
    root: pathlib.Path, readiness: dict[str, Any]
) -> dict[str, Any]:
    """Require Wave 0 to bind the exact current static scope classification."""

    current = production_scope_inventory_binding(root)
    readiness_binding = readiness.get("production_scope_inventory")
    if readiness_binding != current:
        fail(
            "Wave 0 production scope inventory binding differs from the current "
            f"classification: expected={current}, actual={readiness_binding}"
        )
    readiness_tests = readiness.get("test_inventory", {}).get(
        "production_scope_tests"
    )
    if readiness_tests != current.get("evidence_tests"):
        fail(
            "Wave 0 JUnit production-scope evidence differs from the current "
            f"typed test binding: expected={current.get('evidence_tests')}, "
            f"actual={readiness_tests}"
        )
    if current.get("closure_status") not in {"classified-with-gaps", "qualified"}:
        fail("production scope inventory has an invalid closure status")
    return current


def materialization_assignment_shape(qualification: str) -> dict[str, Any]:
    """Return an exact accepted assignment shape for the materializer surface."""

    assignment: dict[str, Any] = {
        "id": MATERIALIZATION_ASSIGNMENT_ID,
        "owner_issue": MATERIALIZATION_ASSIGNMENT_OWNER,
        "scope": "included",
        "qualification": qualification,
        "surfaces": [
            {"domain": domain, "id": surface_id}
            for domain, surface_id in MATERIALIZATION_ASSIGNMENT_SURFACES
        ],
    }
    if qualification == "tracked-gap":
        assignment.update(
            {
                "gap": {
                    "finding": "scope.tracked-gap.clang22-installed-adoption",
                    "remediation": (
                        "Implement the accepted DF-0195 through DF-0199 sealed-evidence, "
                        "measured-occurrence, authenticated-streaming, head-observation, "
                        "and canonical-Base64 authority; resolve DF-0200 bounded claim/Store "
                        "staging; then complete installed actual-source worker output adoption "
                        "with exact publication and query evidence; do not claim generic "
                        "relation-row reference enforcement."
                    ),
                },
                "feedback": list(MATERIALIZATION_ASSIGNMENT_FEEDBACK),
            }
        )
    elif qualification == "qualified":
        assignment["evidence"] = list(MATERIALIZATION_ASSIGNMENT_EVIDENCE)
    else:
        fail(f"unsupported materialization assignment qualification: {qualification}")
    return assignment


def materialization_assignment_transition(root: pathlib.Path) -> dict[str, Any]:
    """Classify only the materializer assignment; unrelated scope gaps are irrelevant."""

    try:
        scope_checker = importlib.import_module("check_ng_production_scope_closure")
    except ImportError as error:
        fail(f"production scope checker is unavailable: {error}")
    contract_error = getattr(scope_checker, "ContractError", None)
    if not isinstance(contract_error, type) or not issubclass(
        contract_error, Exception
    ):
        fail("production scope checker omits ContractError")
    try:
        model = scope_checker.validate_repository(root)
        key = scope_checker.SurfaceKey(
            "distribution.installed-tool", "cxxlens-clang22-materialize"
        )
        assignment = model.assignments[key]
    except (KeyError, OSError, ValueError, contract_error) as error:
        fail(f"cannot resolve the Clang 22 materializer assignment: {error}")

    if assignment == materialization_assignment_shape("tracked-gap"):
        return {
            "assignment_state": "tracked-gap",
            "materialization_evidence": {
                "state": "tracked-gap-empty",
                "request_count": 0,
                "report_count": 0,
                "report_set_count": 0,
                "owner_issue": MATERIALIZATION_ASSIGNMENT_OWNER,
                "feedback": list(MATERIALIZATION_ASSIGNMENT_FEEDBACK),
            },
        }
    if assignment == materialization_assignment_shape("qualified"):
        return {
            "assignment_state": "qualified",
            "materialization_evidence": {
                "state": "exact-matrix",
                "request_count": 4,
                "report_count": 4,
                "report_set_count": 2,
                "owner_issue": None,
                "feedback": [],
            },
        }
    fail(
        "Clang 22 materializer assignment is neither the exact "
        "#181/DF-0182/DF-0187/DF-0191/DF-0192/DF-0195/DF-0196/DF-0197/"
        "DF-0198/DF-0199/DF-0200 "
        "tracked gap nor the exact included+qualified assignment"
    )


def materialization_required_install_files(
    manifest: dict[str, Any], transition: dict[str, Any]
) -> list[str]:
    """Apply the pre-#181 exemption to the tool binary and to no schema."""

    required = list(manifest["package"]["required_files"])
    state = transition.get("assignment_state")
    if state == "tracked-gap":
        if required.count(MATERIALIZATION_TOOL_FILE) != 1:
            fail("release package does not bind the materialization tool binary exactly")
        required.remove(MATERIALIZATION_TOOL_FILE)
    elif state != "qualified":
        fail(f"unsupported materialization assignment state: {state}")
    return required


def collect_materialization_evidence(
    root: pathlib.Path,
    manifest: dict[str, Any],
    evidence: pathlib.Path,
    install_values: dict[str, dict[str, Any]],
    git: dict[str, Any],
    transition: dict[str, Any],
) -> tuple[
    dict[tuple[str, str], dict[str, Any]],
    dict[str, str],
    dict[str, Any],
]:
    """Require exact zero for the tracked gap or the exact matrix when qualified."""

    report_paths = [
        path
        for path in evidence.rglob(manifest["materialization"]["report_filename"])
        if path.is_file()
    ]
    request_paths = [
        path
        for path in evidence.rglob(manifest["materialization"]["request_filename"])
        if path.is_file()
    ]
    receipt_paths = [
        path
        for path in evidence.rglob(
            manifest["materialization"]["execution_receipt_filename"]
        )
        if path.is_file()
    ]
    state = transition.get("assignment_state")
    if state == "tracked-gap":
        if report_paths or request_paths or receipt_paths:
            fail(
                "the exact #181/DF-0182/DF-0187/DF-0191/DF-0192/DF-0195/DF-0196/"
                "DF-0197/DF-0198/DF-0199/DF-0200 "
                "tracked gap requires zero materialization "
                f"requests and reports, found {len(request_paths)} requests and "
                f"{len(report_paths)} reports and {len(receipt_paths)} execution receipts"
            )
        return {}, {}, transition["materialization_evidence"]
    if state != "qualified":
        fail(f"unsupported materialization assignment state: {state}")
    reports, report_sets = verify_materialization_reports(
        root,
        manifest,
        evidence,
        install_values,
        git,
    )
    return reports, report_sets, transition["materialization_evidence"]


def require_exact_clean_main(
    root: pathlib.Path, expected_revision: str
) -> dict[str, Any]:
    git = git_state(root)
    if git != {
        "revision": expected_revision,
        "tree": git["tree"],
        "branch": "main",
        "clean": True,
    }:
        fail(f"GR evaluation requires exact clean main revision: {git}")
    return git


def validate_documents(
    root: pathlib.Path, *, require_qualified_release: bool = False
) -> dict[str, Any]:
    manifest = load(root / MANIFEST)
    validate_schema(manifest, load(root / MANIFEST_SCHEMA), "release qualification")
    validate_schema(
        {
            "schema": "cxxlens.clang22-materialization-execution-receipt.v1",
            "actual_exit_status": 0,
            "exact_stdout_byte_count": 1,
            "stdout_sha256": "sha256:" + "0" * 64,
            "parsed_response_count": 1,
            "stderr_sha256": "sha256:" + "0" * 64,
        },
        load(root / MATERIALIZATION_EXECUTION_RECEIPT_SCHEMA),
        "materialization execution receipt",
    )
    validate_schema(
        {
            "schema": "cxxlens.ng-release-qualification-report.v1",
            "result": "passed",
            "generated_at": "2026-07-18T00:00:00Z",
            "run_url": "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
            "git": {"revision": "1" * 40, "tree": "2" * 40, "branch": "main", "clean": True},
            "release": {"id": "distribution-1.0", "version": "1.0.0", "state": "qualified"},
            "prerequisites": {"gates": [f"gate.g{i}" for i in range(6)] + ["gate.release"], "migrations": [f"R{i}" for i in range(5)], "foundation_report_digest": "sha256:" + "1" * 64, "readiness_report_digest": "sha256:" + "2" * 64, "public_callable_report_digest": "sha256:" + "3" * 64, "g5_report_digest": "sha256:" + "4" * 64, "materialization_contract_digest": "sha256:" + "5" * 64, "sqlite_store_v3_qualification": {"revision": "1" * 40, "source_tree": "2" * 40, "report_digest": "sha256:" + "7" * 64, "report_set_digest": "sha256:" + "8" * 64, "report_schema_digest": "sha256:" + "9" * 64, "sqlite_contract_digest": "sha256:" + "a" * 64}, "release_evaluation_report_digest": "sha256:" + "6" * 64, "same_revision": True},
            "packages": [
                {"configuration": configuration, "prefix_digest": "sha256:" + digit * 64, "manifest_digest": "sha256:" + digit * 64, "toolchain_digest": "sha256:" + digit * 64, "materialization_report_set_digest": "sha256:" + digit * 64, "real_project": "passed", "storage_backends": ["memory", "sqlite"], "relocated": True, "license": "sha256:" + digit * 64, "notice": "sha256:" + digit * 64}
                for configuration, digit in (("static", "1"), ("shared", "2"))
            ],
            "production_support": [
                {"provider_id": "cxxlens.clang22.reference", "provider_version": "1.0.0", "binary_digest": "sha256:" + digit * 64, "relation": relation, "interpretation": "cc.clang22-canonical-1", "toolchain": "clang version 22.1.0", "platform": f"linux-x86_64-{configuration}", "status": "production-supported", "capabilities": ["canonical-entity", "call-site", "direct-target", "process-isolation"], "guarantee": "exact-claims-with-coverage-unresolved-and-provenance", "security_profile_digest": "sha256:" + "3" * 64, "evidence_digest": "sha256:" + "4" * 64}
                for configuration, digit in (("static", "1"), ("shared", "2"))
                for relation in ("cc.entity@1", "cc.call_site@1", "cc.call_direct_target@1")
            ],
            "security": {"status": "green", "contract_digest": "sha256:" + "3" * 64, "vector_count": 1},
            "performance": {"report_digest": "sha256:" + "4" * 64, "warm_zero_plan_median_us": 1, "bounded_closure_median_us": 1},
            "documentation": {
                "doxygen_contract": "passed",
                "doxygen_callable_count": 1,
                "doxygen_digest": "sha256:" + "5" * 64,
                "public_callable_report": {
                    "path": "cxxlens-public-callables/cxxlens-ng-public-callable-inventory-report.json",
                    "digest": "sha256:" + "3" * 64,
                },
                "public_callable_inventory": {
                    "path": CALLABLE_INVENTORY.as_posix(),
                    "file_digest": "sha256:" + "6" * 64,
                    "semantic_digest": "sha256:" + "7" * 64,
                    "callable_count": 1,
                },
                "public_callable_review": {
                    "path": "cxxlens-public-callables/cxxlens-ng-public-callable-inventory-review.md",
                    "digest": "sha256:" + "8" * 64,
                },
                "support_matrix": "exact-report-only",
            },
            "negative_evidence": manifest["security"]["required_negative_evidence"],
            "authority_digests": [{"path": f"authority-{index}", "digest": "sha256:" + str(index) * 64} for index in range(1, 7)],
        },
        load(root / REPORT_SCHEMA),
        "release report",
    )
    validate_schema(
        {
            "schema": "cxxlens.ng-release-qualification-evaluation-report.v1",
            "result": "passed",
            "generated_at": "2026-07-19T00:00:00Z",
            "run_url": "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
            "git": {
                "revision": "1" * 40,
                "tree": "2" * 40,
                "branch": "main",
                "clean": True,
            },
            "release": {"id": "distribution-1.0", "version": "1.0.0"},
            "qualification": "not-qualified",
            "qualified": False,
            "scope_inventory": {
                "manifest_path": "schemas/cxxlens_ng_production_scope_closure.yaml",
                "manifest_digest": "sha256:" + "1" * 64,
                "authority_census_digest": "sha256:" + "2" * 64,
                "evidence_census_digest": "sha256:" + "3" * 64,
                "classification_digest": "sha256:" + "3" * 64,
                "evidence_tests": ["quality.ownership"],
                "summary": {
                    "domain_count": 30,
                    "assignable_count": 1,
                    "expanded_count": 1,
                    "aggregate_count": 14,
                    "qualified": 0,
                    "tracked_gap": 1,
                    "blocked": 0,
                    "not_applicable": 0,
                },
                "closure_status": "classified-with-gaps",
            },
            "evidence": {
                "foundation_report_digest": "sha256:" + "4" * 64,
                "readiness_report_digest": "sha256:" + "5" * 64,
                "public_callable_report_digest": "sha256:" + "6" * 64,
                "g5_report_digest": "sha256:" + "7" * 64,
                "security_report_digest": "sha256:" + "8" * 64,
                "materialization_contract_digest": "sha256:" + "9" * 64,
                "sqlite_store_v3_qualification": {
                    "revision": "1" * 40,
                    "source_tree": "2" * 40,
                    "report_digest": "sha256:" + "a" * 64,
                    "report_set_digest": "sha256:" + "b" * 64,
                    "report_schema_digest": "sha256:" + "c" * 64,
                    "sqlite_contract_digest": "sha256:" + "d" * 64,
                },
                "materialization_evidence": {
                    "state": "exact-matrix",
                    "request_count": 4,
                    "report_count": 4,
                    "report_set_count": 2,
                    "owner_issue": None,
                    "feedback": [],
                },
                "install_manifests": [
                    {
                        "configuration": configuration,
                        "manifest_digest": "sha256:" + digit * 64,
                        "prefix_digest": "sha256:" + digit * 64,
                    }
                    for configuration, digit in (("static", "9"), ("shared", "a"))
                ],
                "materialization_reports": [
                    {
                        "configuration": configuration,
                        "backend": backend,
                        "request_digest": "sha256:" + digit * 64,
                        "request_byte_count": 1,
                        "report_digest": "sha256:" + digit * 64,
                        "report_byte_count": 1,
                        "execution_receipt_digest": "sha256:" + digit * 64,
                        "actual_exit_status": 0,
                        "exact_stdout_byte_count": 1,
                        "stdout_digest": "sha256:" + digit * 64,
                        "parsed_response_count": 1,
                        "stderr_digest": "sha256:" + "0" * 64,
                    }
                    for configuration, digit in (("static", "b"), ("shared", "c"))
                    for backend in ("memory", "sqlite")
                ],
                "materialization_report_sets": [
                    {
                        "configuration": configuration,
                        "report_set_digest": "sha256:" + digit * 64,
                    }
                    for configuration, digit in (("static", "d"), ("shared", "e"))
                ],
                "same_revision": True,
            },
            "production_support": [],
            "strict_report": {
                "schema": "cxxlens.ng-release-qualification-report.v1",
                "eligible": False,
                "emitted": False,
            },
            "reason_codes": ["release.scope-not-qualified"],
        },
        load(root / EVALUATION_REPORT_SCHEMA),
        "release evaluation report",
    )

    missing = [path for path in manifest["required_artifacts"] if not (root / path).is_file()]
    if missing:
        fail(f"required GR artifacts are missing: {missing}")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    if "VERSION 1.0.0" not in cmake or "LICENSE NOTICE" not in cmake:
        fail("distribution package is not version 1.0.0 with license/notice")
    if MATERIALIZATION_EXECUTION_RECEIPT_SCHEMA.name not in cmake:
        fail("CMake install omits the materialization execution receipt schema")
    if MATERIALIZATION_OCCURRENCE_MANIFEST_SCHEMA.name not in cmake:
        fail("CMake install omits the materializer occurrence manifest schema")
    if MATERIALIZATION_OCCURRENCE_MANIFEST_PATH not in manifest["package"][
        "required_files"
    ]:
        fail("release package omits the fixed materializer occurrence manifest")

    try:
        materialization.validate_documents(root)
    except materialization.MaterializationError as error:
        fail(f"Clang 22 materialization authority is invalid: {error}")
    try:
        sqlite_qualification.validate_documents(root)
    except sqlite_qualification.SQLiteStoreV3QualificationError as error:
        fail(f"SQLite Store v3 qualification authority is invalid: {error}")
    expected_materialization = {
        "tool": "cxxlens-clang22-materialize",
        "contract": MATERIALIZATION_CONTRACT.as_posix(),
        "contract_schema": MATERIALIZATION_CONTRACT_SCHEMA.as_posix(),
        "request_schema": MATERIALIZATION_REQUEST_SCHEMA.as_posix(),
        "report_schema": MATERIALIZATION_REPORT_SCHEMA.as_posix(),
        "occurrence_manifest_schema": (
            MATERIALIZATION_OCCURRENCE_MANIFEST_SCHEMA.as_posix()
        ),
        "occurrence_manifest_path": MATERIALIZATION_OCCURRENCE_MANIFEST_PATH,
        "request_filename": "cxxlens-clang22-materialization-request.json",
        "report_filename": "cxxlens-clang22-materialization-report.json",
        "execution_receipt_filename": (
            "cxxlens-clang22-materialization-execution-receipt.json"
        ),
        "execution_receipt_schema": (
            MATERIALIZATION_EXECUTION_RECEIPT_SCHEMA.as_posix()
        ),
        "execution_receipt": {
            "schema": "cxxlens.clang22-materialization-execution-receipt.v1",
            "encoding": "strict-json-single-object",
            "maximum_bytes": MATERIALIZATION_RECEIPT_MAX_BYTES,
            "fields": [
                "actual_exit_status",
                "exact_stdout_byte_count",
                "stdout_sha256",
                "parsed_response_count",
                "stderr_sha256",
            ],
            "report_cross_binding": "exact-stdout-bytes-equal-report-artifact",
            "qualified": {"actual_exit_status": 0, "parsed_response_count": 1},
            "schema_valid_failure": {
                "actual_exit_status": 1,
                "parsed_response_count": 1,
                "release_eligible": False,
            },
            "no_response_failure": {
                "actual_exit_status": 2,
                "parsed_response_count": 0,
                "release_eligible": False,
            },
        },
        "configurations": ["static", "shared"],
        "cardinality": "exactly-one-per-configuration-and-backend",
        "relocated_installed_execution": "required",
        "runtime_loader_environment": "unset",
        "storage_backends": ["memory", "sqlite"],
        "exact_observation_equivalence": (
            "required-zero-non-exact-per-descriptor"
        ),
        "tuple_evidence_binding": "exact-configuration-two-report-set-digest",
        "report_set_digest": {
            "algorithm": "sha256",
            "encoding": "canonical-json-sorted-keys-no-whitespace",
            "projection": ["configuration", "reports"],
            "backend_order": ["memory", "sqlite"],
            "report_entry_fields": [
                "backend",
                "report_digest",
                "execution_receipt_digest",
            ],
        },
    }
    if manifest.get("materialization") != expected_materialization:
        fail("release materialization matrix/set-digest contract differs")
    expected_sqlite_qualification = {
        "authority": "docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md",
        "report_schema": SQLITE_STORE_V3_QUALIFICATION_REPORT_SCHEMA.as_posix(),
        "checker": "tools/quality/check_ng_sqlite_store_v3_qualification.py",
        "report_filename": SQLITE_STORE_V3_QUALIFICATION_REPORT_FILENAME,
        "report_artifact": "cxxlens-ng-sqlite-store-v3-qualification-${revision}",
        "maximum_bytes": SQLITE_STORE_V3_QUALIFICATION_REPORT_MAX_BYTES,
        "status": "required",
        "binding": (
            "exact-revision-source-tree-schema-contract-artifact-and-"
            "report-set-digests"
        ),
        "configurations": ["static", "shared"],
        "required_cases": list(sqlite_qualification.CASE_IDS),
    }
    if manifest.get("sqlite_store_v3_qualification") != expected_sqlite_qualification:
        fail("release SQLite Store v3 qualification contract differs")
    if manifest["provider"].get("observation_relation_descriptors") != materialization.DESCRIPTOR_IDS[3:]:
        fail("release observation descriptor order differs from materialization authority")
    if manifest["claim_policy"].get("tuple_evidence_projection") != [
        "install_manifest",
        "installed_worker",
        "materialization_tool",
        "materialization_request_artifacts",
        "materialization_report_set",
        "materialization_contract",
        "sqlite_store_v3_qualification",
        "g5_report",
        "security_report",
        "public_callable_report",
        "configuration",
    ]:
        fail("production tuple evidence omits exact materialization artifacts")

    acceptance = load(root / ACCEPTANCE)
    gates = {row["id"]: row for row in acceptance["entries"]}
    expected_gates = manifest["prerequisites"]["gates"] + ["gate.release"]
    if any(gates.get(identifier, {}).get("status") != "implemented" for identifier in expected_gates):
        fail("G0-G5 and GR must all be implemented")
    if gates["gate.release"].get("owner_issue") != "#167" or gates["gate.release"].get("depends_on") != manifest["prerequisites"]["gates"]:
        fail("gate.release ownership or dependency closure differs")
    if not set(manifest["required_artifacts"]).issubset(gates["gate.release"].get("evidence", [])):
        fail("gate.release evidence does not enumerate every GR artifact")

    release = load(root / RELEASE)
    row = next(item for item in release["releases"] if item["id"] == "distribution-1.0")
    release_state = (row["state"], row["production_supported"])
    if release_state not in {
        ("qualification-in-progress", False),
        ("qualified", True),
    }:
        fail("distribution 1.0 has an invalid qualification state")
    if require_qualified_release and release_state != ("qualified", True):
        fail("distribution 1.0 is not qualified")
    expected_release_binding = {
        "gate": "gate.release",
        "authority": MANIFEST.as_posix(),
        "materialization_contract": MATERIALIZATION_CONTRACT.as_posix(),
        "materialization_report_matrix": [
            {"configuration": configuration, "backend": backend}
            for configuration, backend in MATERIALIZATION_MATRIX
        ],
        "materialization_report_set_binding": "exact-configuration-two-report-set-digest",
        "sqlite_store_v3_qualification": {
            "authority": (
                "docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md"
            ),
            "report_schema": SQLITE_STORE_V3_QUALIFICATION_REPORT_SCHEMA.as_posix(),
            "checker": "tools/quality/check_ng_sqlite_store_v3_qualification.py",
            "artifact": "cxxlens-ng-sqlite-store-v3-qualification-${revision}",
            "maximum_bytes": SQLITE_STORE_V3_QUALIFICATION_REPORT_MAX_BYTES,
            "binding": (
                "exact-revision-source-tree-schema-contract-artifact-and-"
                "report-set-digests"
            ),
        },
        "checker": "tools/quality/check_ng_release_qualification.py",
        "ci_job": "release-qualification",
        "status": "implemented",
        "claim_scope": "exact-gr-report-tuples-only",
        "report_artifact": "cxxlens-ng-release-qualification-${revision}",
        "required_evidence": [
            "same-sha-foundation-report",
            "same-sha-wave0-readiness-report",
            "same-sha-public-callable-report-and-review",
            "same-sha-g5-report",
            "same-sha-sqlite-store-v3-qualification-report",
            "static-relocated-install-artifact",
            "shared-relocated-install-artifact",
            "static-shared-runtime-junit",
            "static-shared-clang22-materialization-reports-and-execution-receipts",
            "real-project-memory-sqlite-and-major-rejection",
            "security-conformance-and-negative-paths",
            "doxygen-contract-and-support-matrix",
            "license-and-notice",
        ],
    }
    if release.get("release_qualification") != expected_release_binding:
        fail("release qualification binding differs")
    expected_sqlite_release_evidence = {
        "status": "required",
        "current_authority": "independently-validated-exact-2x4-release-evidence",
        "report_schema": SQLITE_STORE_V3_QUALIFICATION_REPORT_SCHEMA.as_posix(),
        "checker": "tools/quality/check_ng_sqlite_store_v3_qualification.py",
        "report_filename": SQLITE_STORE_V3_QUALIFICATION_REPORT_FILENAME,
        "artifact": "cxxlens-ng-sqlite-store-v3-qualification-${revision}",
        "maximum_bytes": SQLITE_STORE_V3_QUALIFICATION_REPORT_MAX_BYTES,
        "revision_binding": (
            "exact-revision-source-tree-schema-contract-artifact-and-"
            "report-set-digests"
        ),
        "configurations": ["static", "shared"],
        "required_cases": list(sqlite_qualification.CASE_IDS),
    }
    snapshot_binding = release.get("snapshot_format_binding", {})
    if (
        snapshot_binding.get("registered_migration", {}).get("authority_status")
        != "accepted"
        or snapshot_binding.get("qualification_evidence")
        != expected_sqlite_release_evidence
    ):
        fail("release SQLite Store v3 evidence activation differs")
    scope_evaluation = release.get("production_scope_closure", {}).get("evaluation")
    if scope_evaluation != {
        "schema": EVALUATION_REPORT_SCHEMA.as_posix(),
        "ci_job": "release-evaluation",
        "artifact": "cxxlens-ng-release-qualification-evaluation-${revision}",
        "not_qualified_satisfies_gate_release": False,
    }:
        fail("release evaluation binding differs")

    support = load(root / SUPPORT)
    policy = support.get("production_claim_policy", {})
    if policy.get("authority") != "exact-gr-report-tuple-only" or policy.get("pending_or_wildcard") != "forbidden" or policy.get("unlisted_surface") != "unsupported":
        fail("support matrix does not fail closed to exact GR report tuples")
    if any(row["status"] == "production-supported" for row in support["entries"]):
        fail("static support matrix must not grant production support")
    clang = [row for row in support["entries"] if row["provider_id"] == "cxxlens.clang22.reference"]
    if {row["relation"] for row in clang} != set(manifest["provider"]["relations"]) or any(
        row["interpretation"] != manifest["provider"]["interpretation"]
        or row["qualification"] != "canonical-semantic-qualified"
        for row in clang
    ):
        fail("Clang 22 conformance templates differ from production authority")
    registry = load(root / REGISTRY)
    descriptors = {row["descriptor_id"] for row in registry["relations"]}
    if not set(manifest["provider"]["relation_descriptors"]).issubset(descriptors):
        fail("release provider relation is absent from registry")
    worker_source = (root / "src/llvm/clang22/provider_worker.cpp").read_text(encoding="utf-8")
    task_decoder_source = (root / "src/llvm/clang22/provider_task_v3.cpp").read_text(
        encoding="utf-8"
    )
    validate_clang22_production_source_decomposition(
        worker_source, task_decoder_source
    )
    workflow = (root / ".github/workflows/quality.yml").read_text(encoding="utf-8")

    def workflow_job(name: str) -> re.Match[str] | None:
        return re.search(
            rf"(?ms)^  {re.escape(name)}:\n(?P<body>.*?)(?=^  [a-zA-Z0-9_-]+:|\Z)",
            workflow,
        )

    evaluation_job = workflow_job("release-evaluation")
    strict_job = workflow_job("release-qualification")
    if evaluation_job is None or strict_job is None:
        fail("release evaluation/qualification workflow jobs are missing")
    for marker in (
        "needs: [g5-qualification, sqlite-store-v3-qualification]",
        "check_ng_release_qualification.py evaluate",
        '--github-output "${GITHUB_OUTPUT}"',
        "qualification: ${{ steps.evaluate.outputs.qualification }}",
    ):
        if marker not in evaluation_job.group("body"):
            fail(f"release evaluation workflow marker is missing: {marker}")
    for marker in (
        "needs: [release-evaluation]",
        "needs.release-evaluation.outputs.qualification == 'qualified'",
        "check_ng_release_qualification.py report",
        "--evaluation ",
    ):
        if marker not in strict_job.group("body"):
            fail(f"strict release qualification workflow marker is missing: {marker}")
    if "-DCXXLENS_BUILD_DOCS=ON" not in workflow:
        fail("release qualification workflow omits Doxygen production build")
    install = (root / "tests/install/run_install_test.cmake.in").read_text(encoding="utf-8")
    for marker in ("real-project-consumer", "share/doc/cxxlens/LICENSE", "share/doc/cxxlens/NOTICE", "libcxxlens_base.so.1"):
        if marker not in install:
            fail(f"release install qualification marker is missing: {marker}")
    return manifest


def validate_clang22_production_source_decomposition(
    worker_source: str, task_decoder_source: str
) -> None:
    for marker in (
        "cc::relations::entity::descriptor()",
        "cc::relations::call_site::descriptor()",
        "cc::relations::call_direct_target::descriptor()",
        "decode_task_input(",
    ):
        if marker not in worker_source:
            fail(f"Clang 22 worker production surface marker is missing: {marker}")
    for marker in ('"cc.clang22-canonical-1"', "sdk::project_catalog::make("):
        if marker not in task_decoder_source:
            fail(f"Clang 22 task.v3 production surface marker is missing: {marker}")


def find_one(root: pathlib.Path, name: str) -> pathlib.Path:
    matches = sorted(path for path in root.rglob(name) if path.is_file())
    if len(matches) != 1:
        fail(f"expected exactly one {name}, found {len(matches)}")
    return matches[0]


def _artifact_identity(value: os.stat_result) -> tuple[int, int, int, int, int, int]:
    return (
        value.st_dev,
        value.st_ino,
        value.st_mode,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
    )


def _artifact_name_occurrences(
    evidence: pathlib.Path, filename: str
) -> list[pathlib.Path]:
    """Census one filename without traversing a symlinked directory."""

    matches: list[pathlib.Path] = []

    def raise_walk_error(error: OSError) -> None:
        raise error

    try:
        for current, directories, files in os.walk(
            evidence, topdown=True, onerror=raise_walk_error, followlinks=False
        ):
            current_path = pathlib.Path(current)
            if filename in files or filename in directories:
                matches.append(current_path / filename)
            directories[:] = sorted(
                name
                for name in directories
                if not (current_path / name).is_symlink()
            )
    except OSError as error:
        fail(f"cannot census SQLite Store v3 qualification artifacts: {error}")
    return sorted(matches)


@contextlib.contextmanager
def held_sqlite_store_v3_report(
    evidence: pathlib.Path,
    artifact_name: str,
    report_name: str,
    maximum_bytes: int,
):
    """Hold and bounded-read the exact no-follow artifact path once."""

    for label, component in (
        ("artifact", artifact_name),
        ("report", report_name),
    ):
        if component in {"", ".", ".."} or pathlib.PurePath(component).name != component:
            fail(f"SQLite Store v3 qualification {label} is not one path component")
    if not hasattr(os, "O_NOFOLLOW") or not hasattr(os, "O_DIRECTORY"):
        fail("SQLite Store v3 qualification requires no-follow directory opens")

    expected_path = evidence / artifact_name / report_name
    evidence_descriptor = -1
    artifact_descriptor = -1
    report_descriptor = -1
    try:
        try:
            evidence_lstat = os.stat(evidence, follow_symlinks=False)
        except OSError as error:
            fail(f"cannot lstat SQLite Store v3 evidence root {evidence}: {error}")
        if not stat.S_ISDIR(evidence_lstat.st_mode):
            fail(f"SQLite Store v3 evidence root is not a no-follow directory: {evidence}")
        flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
        try:
            evidence_descriptor = os.open(evidence, flags | os.O_DIRECTORY)
        except OSError as error:
            fail(f"cannot open SQLite Store v3 evidence root {evidence}: {error}")
        evidence_fstat = os.fstat(evidence_descriptor)
        if _artifact_identity(evidence_fstat) != _artifact_identity(evidence_lstat):
            fail("SQLite Store v3 evidence root changed before it was held")

        try:
            artifact_lstat = os.stat(
                artifact_name,
                dir_fd=evidence_descriptor,
                follow_symlinks=False,
            )
        except FileNotFoundError:
            matches = _artifact_name_occurrences(evidence, report_name)
            if len(matches) == 1:
                fail(
                    "SQLite Store v3 qualification report artifact root differs: "
                    f"expected={expected_path.parent}, actual={matches[0].parent}"
                )
            fail(f"expected exactly one {report_name}, found {len(matches)}")
        except OSError as error:
            fail(f"cannot lstat SQLite Store v3 artifact directory: {error}")
        if not stat.S_ISDIR(artifact_lstat.st_mode):
            fail(
                "SQLite Store v3 qualification artifact directory is not a "
                f"no-follow directory: {expected_path.parent}"
            )
        try:
            artifact_descriptor = os.open(
                artifact_name,
                flags | os.O_DIRECTORY,
                dir_fd=evidence_descriptor,
            )
        except OSError as error:
            fail(f"cannot open SQLite Store v3 artifact directory: {error}")
        artifact_fstat = os.fstat(artifact_descriptor)
        if _artifact_identity(artifact_fstat) != _artifact_identity(artifact_lstat):
            fail("SQLite Store v3 qualification artifact directory changed before open")

        matches = _artifact_name_occurrences(evidence, report_name)
        if len(matches) != 1:
            fail(f"expected exactly one {report_name}, found {len(matches)}")
        if pathlib.Path(os.path.abspath(matches[0])) != pathlib.Path(
            os.path.abspath(expected_path)
        ):
            fail(
                "SQLite Store v3 qualification report artifact root differs: "
                f"expected={expected_path.parent}, actual={matches[0].parent}"
            )

        try:
            report_lstat = os.stat(
                report_name,
                dir_fd=artifact_descriptor,
                follow_symlinks=False,
            )
        except OSError as error:
            fail(f"cannot lstat SQLite Store v3 qualification report: {error}")
        if not stat.S_ISREG(report_lstat.st_mode):
            fail(
                "SQLite Store v3 qualification report is not a no-follow regular file: "
                f"{expected_path}"
            )
        if report_lstat.st_size > maximum_bytes:
            fail(
                "SQLite Store v3 qualification report exceeds the bounded byte limit: "
                f"{report_lstat.st_size} > {maximum_bytes}"
            )
        try:
            report_descriptor = os.open(
                report_name,
                flags,
                dir_fd=artifact_descriptor,
            )
        except OSError as error:
            fail(f"cannot open SQLite Store v3 qualification report: {error}")
        report_fstat = os.fstat(report_descriptor)
        if _artifact_identity(report_fstat) != _artifact_identity(report_lstat):
            fail("SQLite Store v3 qualification report changed before open")

        chunks: list[bytes] = []
        remaining = maximum_bytes + 1
        while remaining > 0:
            chunk = os.read(report_descriptor, min(1_048_576, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        raw = b"".join(chunks)
        if len(raw) > maximum_bytes:
            fail("SQLite Store v3 qualification report grew beyond its bounded byte limit")
        after_read = os.fstat(report_descriptor)
        if (
            len(raw) != report_fstat.st_size
            or _artifact_identity(after_read) != _artifact_identity(report_fstat)
        ):
            fail("SQLite Store v3 qualification report changed while it was read")

        yield expected_path, raw

        current_evidence = os.stat(evidence, follow_symlinks=False)
        current_artifact = os.stat(
            artifact_name,
            dir_fd=evidence_descriptor,
            follow_symlinks=False,
        )
        current_report = os.stat(
            report_name,
            dir_fd=artifact_descriptor,
            follow_symlinks=False,
        )
        if _artifact_identity(current_evidence) != _artifact_identity(evidence_fstat):
            fail("SQLite Store v3 evidence root path changed while validating the report")
        if _artifact_identity(current_artifact) != _artifact_identity(artifact_fstat):
            fail("SQLite Store v3 qualification artifact directory path changed")
        if _artifact_identity(current_report) != _artifact_identity(report_fstat):
            fail("SQLite Store v3 qualification report path changed while validating")
        matches = _artifact_name_occurrences(evidence, report_name)
        if [pathlib.Path(os.path.abspath(path)) for path in matches] != [
            pathlib.Path(os.path.abspath(expected_path))
        ]:
            fail("SQLite Store v3 qualification report census changed while validating")
    except OSError as error:
        fail(f"SQLite Store v3 qualification artifact changed while held: {error}")
    finally:
        for descriptor in (
            report_descriptor,
            artifact_descriptor,
            evidence_descriptor,
        ):
            if descriptor >= 0:
                os.close(descriptor)


def verify_sqlite_store_v3_qualification(
    root: pathlib.Path,
    manifest: dict[str, Any],
    evidence: pathlib.Path,
    git: dict[str, Any],
) -> dict[str, Any]:
    """Independently validate and bind one exact-SHA SQLite v3 report artifact."""

    contract = manifest["sqlite_store_v3_qualification"]
    artifact_name = contract["report_artifact"].replace(
        "${revision}", git["revision"]
    )
    with held_sqlite_store_v3_report(
        evidence,
        artifact_name,
        contract["report_filename"],
        contract["maximum_bytes"],
    ) as (report_path, report_bytes):
        try:
            report = sqlite_qualification.load_report_bytes(
                report_bytes,
                "SQLite Store v3 qualification report",
            )
            sqlite_qualification.validate_report(
                root,
                report,
                expected_revision=git["revision"],
                expected_source_tree=git["tree"],
            )
        except sqlite_qualification.SQLiteStoreV3QualificationError as error:
            fail(f"SQLite Store v3 qualification report is invalid: {error}")

        binding = {
            "revision": report["revision"],
            "source_tree": report["source_tree"],
            "report_digest": digest_bytes(report_bytes),
            "report_set_digest": report["report_set_digest"],
            "report_schema_digest": report["report_schema_digest"],
            "sqlite_contract_digest": report["sqlite_contract_digest"],
        }
        if binding["revision"] != git["revision"]:
            fail("SQLite Store v3 qualification revision differs from release evidence")
        if binding["source_tree"] != git["tree"]:
            fail("SQLite Store v3 qualification source tree differs from release evidence")
    return {"path": report_path, "report": report, "binding": binding}


def verify_public_callable_evidence(
    root: pathlib.Path,
    manifest: dict[str, Any],
    evidence: pathlib.Path,
    git: dict[str, Any],
    readiness: dict[str, Any],
) -> dict[str, Any]:
    """Verify one exact-SHA callable JSON/Markdown pair and its Wave 0 binding."""

    contract = manifest["documentation"]
    report_path = find_one(evidence, contract["public_callable_report_filename"])
    review_path = find_one(evidence, contract["public_callable_review_filename"])
    if report_path.parent != review_path.parent:
        fail("public callable JSON/Markdown do not form one artifact pair")
    report = load(report_path)
    validate_schema(
        report,
        load(root / contract["public_callable_report_schema"]),
        "public callable inventory report",
    )
    if report["git"] != git:
        fail("public callable inventory report is not from the exact GR revision/tree")
    if report["result"] != "passed":
        fail("public callable inventory report did not pass")

    inventory_path = root / contract["public_callable_inventory"]
    inventory = load(inventory_path)
    try:
        callable_inventory.validate_inventory_document(root, inventory)
    except callable_inventory.CallableInventoryError as error:
        fail(f"current public callable inventory is invalid: {error}")
    inventory_binding = {
        "path": contract["public_callable_inventory"],
        "file_digest": digest(inventory_path),
        "semantic_digest": callable_inventory.inventory_digest(inventory),
        "callable_count": len(inventory["callables"]),
    }
    if report["inventory"] != inventory_binding:
        fail("public callable report differs from the current inventory digests/count")
    if report["doxygen"]["count"] != inventory_binding["callable_count"]:
        fail("public callable Doxygen count differs from the current inventory")

    if report["review"]["path"] != contract["public_callable_review_filename"]:
        fail("public callable report names a different Markdown review artifact")
    review_digest = digest(review_path)
    if report["review"]["digest"] != review_digest:
        fail("public callable Markdown review digest differs")
    expected_markdown = callable_inventory.review_markdown(
        inventory,
        git,
        report["run_url"],
        report["doxygen"]["digest"],
    )
    if review_path.read_text(encoding="utf-8") != expected_markdown:
        fail("public callable Markdown review differs from the current inventory")

    report_relative = report_path.relative_to(evidence).as_posix()
    review_relative = review_path.relative_to(evidence).as_posix()
    report_digest = digest(report_path)
    expected_readiness_binding = {
        "path": report_relative,
        "report_digest": report_digest,
        "inventory_file_digest": inventory_binding["file_digest"],
        "inventory_semantic_digest": inventory_binding["semantic_digest"],
        "review_path": review_relative,
        "review_digest": review_digest,
        "callable_count": inventory_binding["callable_count"],
        "doxygen_count": report["doxygen"]["count"],
        "result": report["result"],
        "revision": report["git"]["revision"],
        "tree": report["git"]["tree"],
    }
    readiness_binding = readiness.get("public_callable_inventory")
    if readiness_binding != expected_readiness_binding:
        fail(
            "Wave 0 public callable binding differs from the exact JSON artifact "
            f"digest: expected={expected_readiness_binding}, actual={readiness_binding}"
        )

    return {
        "report": {"path": report_relative, "digest": report_digest},
        "inventory": inventory_binding,
        "review": {"path": review_relative, "digest": review_digest},
        "doxygen": report["doxygen"],
    }


def junit_tests(path: pathlib.Path) -> set[str]:
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as error:
        fail(f"invalid JUnit {path}: {error}")
    if any(case.find("failure") is not None or case.find("error") is not None or case.find("skipped") is not None for case in root.iter("testcase")):
        fail(f"JUnit contains non-passing test: {path}")
    return {case.get("name", "") for case in root.iter("testcase")}


def verify_install_manifest(root: pathlib.Path, path: pathlib.Path, revision: str, tree: str, required_files: list[str]) -> tuple[str, dict[str, Any]]:
    value = load(path)
    validate_schema(value, load(root / INSTALL_SCHEMA), f"install manifest {path}")
    if value["source"] != {"revision": revision, "tree": tree}:
        fail(f"install artifact is not bound to exact source: {path}")
    if "clang version 22" not in value["toolchain"]["identity"].lower():
        fail(f"install artifact was not built by exact Clang 22: {path}")
    without_manifest = dict(value)
    actual_manifest_digest = without_manifest.pop("manifest_digest")
    if canonical_digest(without_manifest) != actual_manifest_digest:
        fail(f"install manifest digest mismatch: {path}")
    if canonical_digest(value["files"]) != value["prefix_digest"]:
        fail(f"install prefix digest mismatch: {path}")
    configuration = "shared" if "shared=ON" in value["configuration"] else "static" if "shared=OFF" in value["configuration"] else ""
    if not configuration:
        fail(f"unknown install configuration: {value['configuration']}")
    files = {row["path"]: row for row in value["files"]}
    missing = sorted(set(required_files) - files.keys())
    if missing:
        fail(f"installed {configuration} package omits required files: {missing}")
    prefix = path.parent / "relocated-prefix"
    if not prefix.is_dir():
        fail(f"relocated prefix is missing beside {path}")
    for relative in required_files:
        candidate = prefix / relative
        if not candidate.is_file():
            fail(f"installed file digest mismatch: {configuration}:{relative}")
        candidate_digest = digest(candidate)
        if candidate_digest != files[relative]["digest"]:
            fail(f"installed file digest mismatch: {configuration}:{relative}")
        schema_prefix = "share/cxxlens/schemas/"
        if relative.startswith(schema_prefix):
            source_schema = root / "schemas" / relative.removeprefix(schema_prefix)
            if not source_schema.is_file() or candidate_digest != digest(source_schema):
                fail(
                    "installed schema differs from the exact source authority: "
                    f"{configuration}:{relative}"
                )
    return configuration, value


def materialization_report_set_digest(
    reports: dict[tuple[str, str], dict[str, Any]], configuration: str
) -> str:
    """Digest one configuration's canonical memory/SQLite report set."""

    return canonical_digest(
        {
            "configuration": configuration,
            "reports": [
                {
                    "backend": backend,
                    "report_digest": reports[(configuration, backend)]["digest"],
                    "execution_receipt_digest": reports[(configuration, backend)][
                        "execution_receipt_digest"
                    ],
                }
                for backend in ("memory", "sqlite")
            ],
        }
    )


def validate_release_span_census(report: dict[str, Any]) -> None:
    """Independently close observation rows, span presence, and unresolved counts."""

    stages: dict[str, dict[str, Any]] = {}
    for stage in report["claim_stages"]:
        descriptor_id = stage["descriptor_id"]
        if descriptor_id in stages:
            fail(f"duplicate materialization claim stage: {descriptor_id}")
        stages[descriptor_id] = stage

    observation_rows = {
        descriptor_id: 0
        for descriptor_id in materialization.SPAN_OBSERVATION_DESCRIPTORS
    }
    for task in report["task_results"]:
        task_batches: dict[str, dict[str, Any]] = {}
        for batch in task["batches"]:
            descriptor_id = batch["descriptor_id"]
            if descriptor_id in task_batches:
                fail(
                    "duplicate materialization task batch while closing span census: "
                    f"{task['provider_task_id']}:{descriptor_id}"
                )
            task_batches[descriptor_id] = batch
        for descriptor_id in observation_rows:
            if descriptor_id not in task_batches:
                fail(
                    "materialization task omits a span-bearing observation batch: "
                    f"{task['provider_task_id']}:{descriptor_id}"
                )
            observation_rows[descriptor_id] += task_batches[descriptor_id]["row_count"]

    for descriptor_id, row_count in observation_rows.items():
        stage = stages.get(descriptor_id)
        if stage is None or stage["sdk_claim_occurrence_count"] != row_count:
            fail(
                "materialization span observation claim/row census differs: "
                f"{descriptor_id}"
            )

    spans = report["span_validation"]
    total_observations = sum(observation_rows.values())
    if total_observations != (
        spans["observed_bundle_count"] + spans["absent_bundle_count"]
    ):
        fail(
            "materialization entity/call observation census does not equal "
            "observed plus absent primary-span bundles"
        )
    if spans["unique_bundle_count"] > spans["observed_bundle_count"]:
        fail("materialization unique primary-span census exceeds observed bundles")
    if spans["absent_bundle_count"] != (
        spans["entity_absent_bundle_count"] + spans["call_absent_bundle_count"]
    ):
        fail("materialization entity/call absent-bundle census differs")

    unresolved = report["side_channels"]["unresolved"]
    category_counts = unresolved["category_counts"]
    if (
        sum(category_counts.values()) != unresolved["record_count"]
        or unresolved["categories"] != sorted(category_counts)
    ):
        fail("materialization unresolved category census is not canonical and exact")
    absence_category = materialization.PRIMARY_SPAN_ABSENCE_CATEGORY
    absence_unresolved_count = category_counts.get(absence_category, 0)
    if (
        spans["absent_bundle_unresolved_count"] != spans["absent_bundle_count"]
        or absence_unresolved_count != spans["absent_bundle_unresolved_count"]
        or unresolved["record_count"] < spans["absent_bundle_unresolved_count"]
    ):
        fail(
            "materialization absent primary-span bundles lack exact typed "
            "unresolved category accounting"
        )


def validate_release_observation_equivalence(report: dict[str, Any]) -> None:
    """Independently require a zero-non-exact census for every observation row."""

    descriptor_ids = materialization.DESCRIPTOR_IDS[3:]
    aggregate: dict[str, dict[str, Any]] = {
        descriptor: {
            "exact_equivalence_count": 0,
            "non_exact_equivalence_count": 0,
            "rows": [],
        }
        for descriptor in descriptor_ids
    }
    for task in report["task_results"]:
        batches = {batch["descriptor_id"]: batch for batch in task["batches"]}
        for descriptor in descriptor_ids:
            batch = batches.get(descriptor)
            if batch is None or "observation_equivalence_census" not in batch:
                fail(
                    "production materialization omits observation equivalence census: "
                    f"{descriptor}"
                )
            census = batch["observation_equivalence_census"]
            if (
                census["exact_equivalence_count"]
                + census["non_exact_equivalence_count"]
                != batch["row_count"]
                or len(census["rows"]) != batch["row_count"]
                or len(batch["row_bindings"]) != batch["row_count"]
            ):
                fail(
                    "production materialization observation equivalence batch census "
                    f"differs: {descriptor}"
                )
            census_bindings = sorted(
                (
                    {
                        "observation_row_digest": row["observation_row_digest"],
                        "final_relation_compile_unit_id": row[
                            "final_relation_compile_unit_id"
                        ],
                        "originating_task": row["originating_task"],
                        "exact_equivalence": row["exact_equivalence"],
                        "limitation_digest": row["limitation_digest"],
                    }
                    for row in census["rows"]
                ),
                key=lambda row: json.dumps(row, sort_keys=True),
            )
            row_bindings = sorted(
                (
                    {
                        "observation_row_digest": binding["row_digest"],
                        "final_relation_compile_unit_id": binding[
                            "final_relation_compile_unit_id"
                        ],
                        "originating_task": binding["originating_task"],
                        "exact_equivalence": binding["exact_equivalence"],
                        "limitation_digest": binding["limitation_digest"],
                    }
                    for binding in batch["row_bindings"]
                ),
                key=lambda row: json.dumps(row, sort_keys=True),
            )
            if census_bindings != row_bindings:
                fail(
                    "production materialization observation equivalence row binding "
                    f"differs: {descriptor}"
                )
            aggregate[descriptor]["exact_equivalence_count"] += census[
                "exact_equivalence_count"
            ]
            aggregate[descriptor]["non_exact_equivalence_count"] += census[
                "non_exact_equivalence_count"
            ]
            aggregate[descriptor]["rows"].extend(census["rows"])

    stages = {stage["descriptor_id"]: stage for stage in report["claim_stages"]}
    guarantee = report["side_channels"]["guarantee"]
    guarantee_censuses = guarantee["observation_descriptor_censuses"]
    if [row["descriptor_id"] for row in guarantee_censuses] != descriptor_ids:
        fail("production materialization observation guarantee census order differs")
    guarantee_by_descriptor = {
        row["descriptor_id"]: row for row in guarantee_censuses
    }
    for descriptor in descriptor_ids:
        expected = materialization.observation_equivalence_census(
            descriptor,
            aggregate[descriptor]["rows"],
        )
        stage = stages.get(descriptor)
        stage_census = None if stage is None else stage.get(
            "observation_equivalence_census"
        )
        guarantee_census = guarantee_by_descriptor[descriptor]
        if stage_census != expected or guarantee_census != {
            "descriptor_id": descriptor,
            "exact_equivalence_count": expected["exact_equivalence_count"],
            "non_exact_equivalence_count": expected[
                "non_exact_equivalence_count"
            ],
            "row_equivalence_set_digest": expected[
                "row_equivalence_set_digest"
            ],
        }:
            fail(
                "production materialization observation equivalence cross-layer "
                f"census differs: {descriptor}"
            )
        if expected["non_exact_equivalence_count"] != 0:
            fail(
                "production materialization contains a non-exact observation: "
                f"{descriptor}"
            )


def validate_release_base_claims(
    request: dict[str, Any], report: dict[str, Any]
) -> None:
    """Independently bind the six host-owned base-claim censuses."""

    base = report["base_claims"]
    if base["descriptor_ids"] != list(MATERIALIZATION_BASE_CLAIM_DESCRIPTORS):
        fail("materialization base-claim descriptor order differs")
    results = base["descriptor_results"]
    if [row["descriptor_id"] for row in results] != list(
        MATERIALIZATION_BASE_CLAIM_DESCRIPTORS
    ):
        fail("materialization base-claim result order differs")

    tasks = request["tasks"]
    spans = report["span_validation"]
    expected_counts = {
        "build.project.v1": 1,
        "build.toolchain_context.v1": len(
            {task["toolchain_context_id"] for task in tasks}
        ),
        "build.variant.v1": len({task["build_variant_id"] for task in tasks}),
        "source.file.v1": len(
            {
                (task["source"]["source_snapshot_id"], task["source"]["file_id"])
                for task in tasks
            }
        ),
        "build.compile_unit.v1": len(
            {task["compile_unit_id"] for task in tasks}
        ),
        "source.span.v1": spans["constructed_source_span_claim_count"],
    }
    for result in results:
        expected = expected_counts[result["descriptor_id"]]
        if result["row_count"] != expected or result["claim_count"] != expected:
            fail(
                "materialization base-claim descriptor census differs: "
                f"{result['descriptor_id']}"
            )
    if (
        base["total_row_count"] != sum(row["row_count"] for row in results)
        or base["total_claim_count"]
        != sum(row["claim_count"] for row in results)
        or base["total_row_count"] != base["total_claim_count"]
        or not base["validated_before_hard_references"]
    ):
        fail("materialization total base-claim census is incomplete")


def materialization_task_execution_key(row: dict[str, Any]) -> tuple[str, str, str]:
    """Return the exact semantic-task/input/physical-execution correlation key."""

    return tuple(  # type: ignore[return-value]
        row[field] for field in MATERIALIZATION_TASK_EXECUTION_KEY_FIELDS
    )


def validate_release_task_execution_census(
    request: dict[str, Any], report: dict[str, Any]
) -> None:
    """Bind catalog-local selections to exactly one composite execution result."""

    request_project = request["project"]
    report_project = report["project"]
    request_entries = request_project["catalog_compile_units"]
    report_entries = report_project["catalog_compile_units"]
    catalog_ids = [entry["catalog_compile_unit_id"] for entry in request_entries]
    if (
        report_entries != request_entries
        or report_project["catalog_compile_unit_census_digest"]
        != request_project["catalog_compile_unit_census_digest"]
        or report_project["catalog_compile_unit_census_digest"]
        != materialization.expected_catalog_compile_unit_census_digest(report_project)
    ):
        fail("materialization report catalog-local compile-unit census differs")

    request_tasks = request["tasks"]
    result_rows = report["task_results"]
    request_keys = [
        materialization_task_execution_key(task) for task in request_tasks
    ]
    result_keys = [
        materialization_task_execution_key(result) for result in result_rows
    ]
    if (
        len(request_keys) != len(set(request_keys))
        or len(result_keys) != len(set(result_keys))
        or set(result_keys) != set(request_keys)
    ):
        fail(
            "materialization composite task/input/execution result census differs"
        )

    selected_ids = [
        result["selected_catalog_compile_unit_id"] for result in result_rows
    ]
    if (
        len(selected_ids) != len(catalog_ids)
        or len(selected_ids) != len(set(selected_ids))
        or set(selected_ids) != set(catalog_ids)
    ):
        fail("materialization selected catalog compile-unit census differs")

    tasks_by_key = dict(zip(request_keys, request_tasks, strict=True))
    for key, result in zip(result_keys, result_rows, strict=True):
        task = tasks_by_key[key]
        if (
            result["selected_catalog_compile_unit_id"]
            != task["selected_catalog_compile_unit_id"]
            or result["compile_unit_id"] != task["compile_unit_id"]
        ):
            fail(
                "materialization catalog-local/final compile-unit execution "
                "mapping differs"
            )


def validate_release_base_claim_parity(
    reports: dict[tuple[str, str], dict[str, Any]],
) -> None:
    """Require every configuration/backend to expose the same full base set."""

    first = reports[MATERIALIZATION_MATRIX[0]]["value"]["base_claims"]
    for key in MATERIALIZATION_MATRIX[1:]:
        if reports[key]["value"]["base_claims"] != first:
            fail(
                "materialization memory/SQLite/static/shared base-claim "
                "count/set-digest parity differs"
            )


def verify_materialization_reports(
    root: pathlib.Path,
    manifest: dict[str, Any],
    evidence: pathlib.Path,
    install_values: dict[str, dict[str, Any]],
    git: dict[str, Any],
) -> tuple[
    dict[tuple[str, str], dict[str, Any]],
    dict[str, str],
]:
    """Validate the exact static/shared x memory/SQLite installed report matrix."""

    report_name = manifest["materialization"]["report_filename"]
    request_name = manifest["materialization"]["request_filename"]
    receipt_name = manifest["materialization"]["execution_receipt_filename"]
    paths = sorted(path for path in evidence.rglob(report_name) if path.is_file())
    if len(paths) != len(MATERIALIZATION_MATRIX):
        fail(
            "GR requires exactly four static/shared x memory/SQLite Clang 22 "
            f"materialization reports, found {len(paths)}"
        )
    request_paths = sorted(
        path for path in evidence.rglob(request_name) if path.is_file()
    )
    if len(request_paths) != len(MATERIALIZATION_MATRIX):
        fail(
            "GR requires exactly four co-located Clang 22 materialization "
            f"requests, found {len(request_paths)}"
        )
    receipt_paths = sorted(
        path for path in evidence.rglob(receipt_name) if path.is_file()
    )
    if len(receipt_paths) != len(MATERIALIZATION_MATRIX):
        fail(
            "GR requires exactly four co-located Clang 22 materialization "
            f"execution receipts, found {len(receipt_paths)}"
        )
    artifact_parents = {path.parent for path in paths}
    if (
        {path.parent for path in request_paths} != artifact_parents
        or {path.parent for path in receipt_paths} != artifact_parents
    ):
        fail(
            "materialization request/report/execution-receipt artifacts are not "
            "co-located exactly"
        )

    report_schema = load(root / MATERIALIZATION_REPORT_SCHEMA)
    receipt_schema = load(root / MATERIALIZATION_EXECUTION_RECEIPT_SCHEMA)
    expected_authorities = materialization.authority_bindings(root)
    expected_registry_digest, expected_descriptors = materialization.descriptor_bindings(
        root
    )
    expected_registry = {
        "authority_registry_digest": expected_registry_digest,
        "base_descriptors": materialization.base_descriptor_bindings(root),
        "descriptors": expected_descriptors,
    }
    reports: dict[tuple[str, str], dict[str, Any]] = {}

    for path in paths:
        request_path = path.with_name(request_name)
        request, request_bytes = load_materialization_json(
            request_path, "materialization request"
        )
        validate_release_materialization_request_machine(request)
        try:
            materialization.validate_request(root, request)
        except materialization.MaterializationError as error:
            fail(f"materialization request binding is invalid: {request_path}: {error}")
        report, report_bytes = load_materialization_json(
            path, "materialization report"
        )
        receipt_path = path.with_name(receipt_name)
        receipt, receipt_bytes = load_execution_receipt(receipt_path, receipt_schema)
        validate_schema(report, report_schema, f"Clang 22 materialization report {path}")
        if report_bytes != materialization.canonical_json(report):
            fail(f"materialization report artifact is not canonical JSON: {path}")
        if (
            report.get("response_kind") != "detailed"
            or report["result"] != "passed"
            or report["process_exit_status"] != 0
        ):
            fail(f"materialization qualification contains a failed report: {path}")
        report_digest = digest_bytes(report_bytes)
        if (
            receipt["actual_exit_status"] != 0
            or receipt["parsed_response_count"] != 1
            or receipt["exact_stdout_byte_count"] != len(report_bytes)
            or receipt["stdout_sha256"] != report_digest
        ):
            fail(
                "materialization execution receipt does not bind exact successful "
                f"stdout/report bytes: {path}"
            )
        if report["source"] != {
            "revision": git["revision"],
            "tree": git["tree"],
        }:
            fail(f"materialization report is not bound to exact source: {path}")
        if report["authority_digests"] != expected_authorities:
            fail(f"materialization authority digest binding differs: {path}")
        if report["registry"] != expected_registry:
            fail(f"materialization registry/descriptor binding differs: {path}")

        publication = report["publication"]
        configuration = request["tool"]["package_configuration"]
        backend = publication["backend"]
        key = (configuration, backend)
        if key not in MATERIALIZATION_MATRIX:
            fail(f"unexpected materialization matrix combination: {key}")
        if key in reports:
            fail(f"duplicate materialization matrix combination: {key}")
        if configuration not in install_values:
            fail(f"materialization report has no install manifest: {configuration}")

        install = install_values[configuration]
        validate_release_occurrence_binding(
            request,
            report,
            install,
            git,
            f"{configuration}/{backend}",
        )
        validate_release_task_execution_census(request, report)
        validate_release_span_census(report)
        validate_release_observation_equivalence(report)
        validate_release_base_claims(request, report)
        try:
            materialization.validate_report(
                root, request, report, request_bytes=request_bytes
            )
        except materialization.MaterializationError as error:
            fail(
                "materialization request/report binding is invalid: "
                f"{configuration}/{backend}: {error}"
            )
        task_results = report["task_results"]
        descriptor_bindings = {
            row["descriptor_id"]: row for row in expected_descriptors
        }
        for task in task_results:
            if task["terminal"] != "provider.success":
                fail(f"materialization task did not succeed: {task['provider_task_id']}")
            groups = {row["dependency_group_id"]: row for row in task["groups"]}
            if set(groups) != set(materialization.GROUP_DESCRIPTORS):
                fail(f"materialization task group set differs: {task['provider_task_id']}")
            for group, descriptors in materialization.GROUP_DESCRIPTORS.items():
                value = groups[group]
                if (
                    value["descriptor_ids"] != descriptors
                    or value["atomic_output_group_id"] != "clang22-atomic"
                    or not value["sealed"]
                ):
                    fail(
                        "materialization task group is not exact/sealed: "
                        f"{task['provider_task_id']}:{group}"
                    )
            batches = {row["descriptor_id"]: row for row in task["batches"]}
            if set(batches) != set(materialization.DESCRIPTOR_IDS):
                fail(f"materialization task batch set differs: {task['provider_task_id']}")
            for descriptor_id, binding in descriptor_bindings.items():
                batch = batches[descriptor_id]
                if (
                    batch["batch_id"] != binding["batch_id"]
                    or batch["runtime_descriptor_digest"]
                    != binding["runtime_descriptor_digest"]
                    or batch["dependency_group_id"]
                    != binding["dependency_group_id"]
                    or batch["atomic_output_group_id"] != "clang22-atomic"
                    or not batch["sealed"]
                ):
                    fail(
                        "materialization batch binding differs: "
                        f"{task['provider_task_id']}:{descriptor_id}"
                    )

        adoption = report["adoption"]
        if (
            adoption["state"] != "sealed"
            or not adoption["all_tasks_mandatory"]
            or not adoption["all_groups_mandatory"]
            or not adoption["all_batches_mandatory"]
            or adoption["raw_frames"]["authority"]
            != "diagnostic-only-non-authoritative"
            or adoption["raw_frames"]["retained"]
        ):
            fail(
                "production materialization report lacks sealed all-or-nothing adoption: "
                f"{configuration}/{backend}"
            )
        spans = report["span_validation"]
        if (
            spans["absent_bundle_count"] != 0
            or spans["absent_bundle_unresolved_count"] != 0
            or spans["source_dependent_canonical_omission_count"] != 0
            or spans["unique_bundle_count"]
            != spans["constructed_source_span_claim_count"]
            or spans["recomputed_id_mismatch_count"] != 0
            or spans["invalid_range_count"] != 0
            or spans["task_binding_mismatch_count"] != 0
            or not spans["hard_references_resolved"]
        ):
            fail(
                "production materialization report has incomplete source-span adoption: "
                f"{configuration}/{backend}"
            )
        coverage = report["side_channels"]["coverage"]
        if (
            sum(coverage["state_counts"].values()) != coverage["record_count"]
            or coverage["balance"] != "exact"
            or coverage["state_counts"]["covered"] != coverage["record_count"]
        ):
            fail(
                "production materialization coverage is not exact and complete: "
                f"{configuration}/{backend}"
            )
        unresolved = report["side_channels"]["unresolved"]
        if unresolved["record_count"] != 0 or unresolved["blocking_count"] != 0:
            fail(
                "production materialization report has unresolved work: "
                f"{configuration}/{backend}"
            )
        guarantee = report["side_channels"]["guarantee"]
        if guarantee["approximation"] != "exact" or any(
            census["non_exact_equivalence_count"] != 0
            for census in guarantee["observation_descriptor_censuses"]
        ):
            fail(
                "production materialization report lacks an exact guarantee: "
                f"{configuration}/{backend}"
            )
        stages = {row["descriptor_id"]: row for row in report["claim_stages"]}
        if set(stages) != set(materialization.DESCRIPTOR_IDS):
            fail("production materialization claim-stage set differs")
        for descriptor_id, expected_stage in materialization.DESCRIPTOR_STAGE.items():
            if stages[descriptor_id]["stage"] != expected_stage:
                fail(
                    "production materialization claim stage differs: "
                    f"{descriptor_id}"
                )
        canonical_count = sum(
            stages[descriptor_id]["sdk_claim_occurrence_count"]
            for descriptor_id in materialization.DESCRIPTOR_IDS[:3]
        )
        provenance = report["provenance"]
        if (
            provenance["canonical_claim_count"] != canonical_count
            or provenance["canonical_claims_with_exact_input_edges"]
            != canonical_count
            or provenance["orphan_count"] != 0
            or provenance["ambiguous_count"] != 0
        ):
            fail("production materialization provenance is incomplete")
        if (
            publication["observed_parent_publication"]
            != publication["expected_parent_publication"]
            or publication["outcome"] != "committed_verified"
            or publication["candidate_identity_state"] != "constructed"
            or publication["invocation_commit_state"] != "committed"
            or publication["committed_transaction_count"] != 1
            or publication["invocation_committed_record"] is None
            or publication["terminal_head"]["status"] != "present"
            or publication["candidate_visibility"] != "present_by_invocation"
            or publication["head_effect"] != "advanced_to_candidate"
            or publication["store_failure"] is not None
        ):
            fail(
                "production materialization report lacks one exact committed transaction: "
                f"{configuration}/{backend}"
            )
        if backend == "sqlite" and publication["sqlite_reopen_status"] != "opened":
            fail("SQLite materialization report was not close/reopen verified")
        if backend == "memory" and publication["sqlite_reopen_status"] != "not_applicable":
            fail("memory materialization report falsely claims SQLite reopen")
        semantic = report["semantic_verification"]
        reopened = semantic["reopened_store"]
        if (
            semantic["status"] != "passed"
            or reopened is None
            or semantic["reopen_attempt"] is not None
            or semantic["failure"] is not None
            or reopened["backend"] != backend
            or len(reopened["handle_receipts"]) != 3
            or any(
                receipt["status"] != "present"
                for receipt in reopened["handle_receipts"]
            )
        ):
            fail(
                "materialization semantic reopen/identity verification is incomplete: "
                f"{configuration}/{backend}"
            )
        reports[key] = {
            "request_path": request_path,
            "request": request,
            "request_bytes": request_bytes,
            "request_digest": digest_bytes(request_bytes),
            "request_byte_count": len(request_bytes),
            "path": path,
            "digest": report_digest,
            "byte_count": len(report_bytes),
            "execution_receipt_path": receipt_path,
            "execution_receipt": receipt,
            "execution_receipt_digest": digest_bytes(receipt_bytes),
            "value": report,
        }

    if set(reports) != set(MATERIALIZATION_MATRIX):
        fail(f"materialization matrix differs: {sorted(reports)}")
    validate_release_base_claim_parity(reports)
    try:
        materialization.validate_qualification_matrix(
            root,
            [
                (
                    reports[key]["request"],
                    reports[key]["value"],
                    reports[key]["request_bytes"],
                )
                for key in MATERIALIZATION_MATRIX
            ],
        )
    except materialization.MaterializationError as error:
        fail(f"materialization qualification matrix is invalid: {error}")

    first = reports[MATERIALIZATION_MATRIX[0]]["value"]
    for key in MATERIALIZATION_MATRIX[1:]:
        report = reports[key]["value"]
        if report["provider"] != first["provider"]:
            fail("materialization matrix provider semantics differ")
        if report["project"] != first["project"]:
            fail("materialization matrix project semantics differ")
        if report["registry"] != first["registry"]:
            fail("materialization matrix registry semantics differ")
        if report["engine"] != first["engine"]:
            fail("materialization matrix engine admission semantics differ")
        if report["interpretation_policy"] != first["interpretation_policy"]:
            fail("materialization matrix interpretation policy semantics differ")
        if report["trust_policy"] != first["trust_policy"]:
            fail("materialization matrix trust policy semantics differ")
        if report["store"]["selector"] != first["store"]["selector"]:
            fail("materialization matrix Store selector semantics differ")

    for configuration in ("static", "shared"):
        if (
            reports[(configuration, "memory")]["request"][
                "semantic_request_digest"
            ]
            != reports[(configuration, "sqlite")]["request"][
                "semantic_request_digest"
            ]
        ):
            fail(
                f"{configuration} memory/SQLite semantic request digests differ"
            )

    snapshots = {
        reports[key]["value"]["store"]["snapshot_manifest"]["snapshot_id"]
        for key in MATERIALIZATION_MATRIX
    }
    exports = {
        reports[key]["value"]["semantic_verification"]["reopened_store"][
            "canonical_export_digest"
        ]
        for key in MATERIALIZATION_MATRIX
    }
    cursors = {
        reports[key]["value"]["semantic_verification"]["reopened_store"][
            "cursor_projection"
        ]["digest"]
        for key in MATERIALIZATION_MATRIX
    }
    publication_semantics = {
        tuple(
            reports[key]["value"]["publication"]["invocation_committed_record"][
                field
            ]
            for field in (
                "publication_id",
                "series_id",
                "snapshot_id",
                "sequence",
                "parent_publication",
                "state",
                "corrupt",
            )
        )
        for key in MATERIALIZATION_MATRIX
    }
    partition_bindings = {
        canonical_digest(
            reports[key]["value"]["semantic_verification"]["reopened_store"][
                "partition_bindings"
            ]
        )
        for key in MATERIALIZATION_MATRIX
    }
    reopen_semantics = {
        backend: {
            receipt["projection"]["semantic_projection_digest"]
            for configuration in ("static", "shared")
            for receipt in reports[(configuration, backend)]["value"][
                "semantic_verification"
            ]["reopened_store"]["handle_receipts"]
        }
        for backend in ("memory", "sqlite")
    }
    if any(
        len(values) != 1
        for values in (
            snapshots,
            exports,
            cursors,
            publication_semantics,
            partition_bindings,
            *reopen_semantics.values(),
        )
    ):
        fail("materialization memory/SQLite/static/shared semantic parity differs")

    report_sets = {
        configuration: materialization_report_set_digest(reports, configuration)
        for configuration in ("static", "shared")
    }
    return reports, report_sets


def collect_release_evidence(
    root: pathlib.Path,
    manifest: dict[str, Any],
    evidence: pathlib.Path,
    security_path: pathlib.Path,
    expected_revision: str,
) -> dict[str, Any]:
    """Validate exact-SHA inputs without constructing either release report."""

    git = require_exact_clean_main(root, expected_revision)
    sqlite_store_v3_qualification = verify_sqlite_store_v3_qualification(
        root, manifest, evidence, git
    )
    foundation_path = find_one(evidence, "cxxlens-ng-foundation-completion-report.json")
    foundation = load(foundation_path)
    validate_schema(foundation, load(root / FOUNDATION_REPORT_SCHEMA), "foundation report")
    readiness_path = find_one(evidence, "cxxlens-ng-api-development-readiness-report.json")
    readiness = load(readiness_path)
    validate_schema(readiness, load(root / READINESS_REPORT_SCHEMA), "readiness report")
    for label, value in (("foundation", foundation), ("readiness", readiness)):
        if value["git"]["revision"] != git["revision"] or value["git"]["tree"] != git["tree"] or value["result"] != "passed":
            fail(f"{label} evidence is not from the exact GR revision")
    scope_inventory = verify_production_scope_inventory(root, readiness)
    materialization_transition = materialization_assignment_transition(root)

    install_paths = sorted(evidence.rglob("install-artifact-manifest.json"))
    if len(install_paths) != 2:
        fail(f"GR requires exactly static/shared install manifests, found {len(install_paths)}")
    packages: list[dict[str, Any]] = []
    install_values: dict[str, dict[str, Any]] = {}
    required_install_files = materialization_required_install_files(
        manifest, materialization_transition
    )
    for path in install_paths:
        configuration, value = verify_install_manifest(
            root,
            path,
            git["revision"],
            git["tree"],
            required_install_files,
        )
        if configuration in install_values:
            fail(f"duplicate install configuration: {configuration}")
        install_values[configuration] = value
        xml = find_one(evidence, f"ctest-install-{'ON' if configuration == 'shared' else 'OFF'}.xml")
        tests = junit_tests(xml)
        required = {
            manifest["package"]["real_project_consumer"],
            "install.relocation",
            *(
                test
                for test in manifest["security"]["required_negative_evidence"]
                if test.startswith("install.")
            ),
        }
        if not required.issubset(tests):
            fail(f"{configuration} install evidence omits tests: {sorted(required - tests)}")
        files = {row["path"]: row["digest"] for row in value["files"]}
        packages.append({"configuration": configuration, "prefix_digest": value["prefix_digest"], "manifest_digest": value["manifest_digest"], "toolchain_digest": value["toolchain"]["digest"], "real_project": "passed", "storage_backends": ["memory", "sqlite"], "relocated": True, "license": files["share/doc/cxxlens/LICENSE"], "notice": files["share/doc/cxxlens/NOTICE"]})
    packages.sort(key=lambda row: (row["configuration"] != "static", row["configuration"]))
    if set(install_values) != {"static", "shared"}:
        fail("static/shared package matrix is incomplete")
    (
        materialization_reports,
        materialization_report_sets,
        materialization_evidence,
    ) = collect_materialization_evidence(
        root,
        manifest,
        evidence,
        install_values,
        git,
        materialization_transition,
    )
    for package in packages:
        if materialization_report_sets:
            package["materialization_report_set_digest"] = (
                materialization_report_sets[package["configuration"]]
            )

    build_xml = sorted(evidence.rglob("ctest-build-*.xml"))
    if len(build_xml) != 2:
        fail(f"GR requires exactly static/shared runtime JUnit, found {len(build_xml)}")
    runtime_tests = set().union(*(junit_tests(path) for path in build_xml))
    required_runtime_tests = list(manifest["provider"]["required_positive_evidence"])
    if materialization_transition["assignment_state"] == "tracked-gap":
        required_runtime_tests.remove("install.clang22-materialization")
    required_runtime_tests.extend(
        manifest["security"]["required_negative_evidence"][:2]
    )
    for test in required_runtime_tests:
        if test not in runtime_tests:
            fail(f"provider runtime evidence is missing: {test}")

    callable_evidence = verify_public_callable_evidence(
        root, manifest, evidence, git, readiness
    )

    g5_path = find_one(evidence, "cxxlens-ng-g5-qualification-report.json")
    g5 = load(g5_path)
    validate_schema(g5, load(root / G5_REPORT_SCHEMA), "G5 report")
    if g5["git"]["revision"] != git["revision"] or g5["git"]["tree"] != git["tree"] or g5["result"] != "passed":
        fail("G5 evidence is not from the exact GR revision")
    security = load(security_path)
    validate_schema(security, load(root / SECURITY_REPORT_SCHEMA), "security report")
    if security["status"] != "green":
        fail("security conformance is not green")
    quality_log = find_one(evidence, "quality-production.log").read_text(encoding="utf-8")
    if "validated Doxygen contracts" not in quality_log:
        fail("Doxygen production contract evidence is missing")

    return {
        "root": root,
        "git": git,
        "packages": packages,
        "install_values": install_values,
        "materialization_reports": materialization_reports,
        "materialization_report_sets": materialization_report_sets,
        "materialization_evidence": materialization_evidence,
        "foundation_path": foundation_path,
        "readiness_path": readiness_path,
        "callable_evidence": callable_evidence,
        "scope_inventory": scope_inventory,
        "g5_path": g5_path,
        "g5": g5,
        "security_path": security_path,
        "security": security,
        "sqlite_store_v3_qualification": sqlite_store_v3_qualification,
    }


def production_support_tuples(
    root: pathlib.Path,
    manifest: dict[str, Any],
    evidence: dict[str, Any],
) -> list[dict[str, Any]]:
    """Derive candidate exact tuples only after all shared evidence is valid."""

    security_digest = digest(root / SECURITY)
    production_support: list[dict[str, Any]] = []
    for configuration in ("static", "shared"):
        value = evidence["install_values"][configuration]
        files = {row["path"]: row["digest"] for row in value["files"]}
        worker = files["bin/cxxlens-clang-worker-22"]
        evidence_digest = canonical_digest(
            {
                "install_manifest": value["manifest_digest"],
                "installed_worker": worker,
                "materialization_tool": files[
                    "bin/cxxlens-clang22-materialize"
                ],
                "materialization_request_artifacts": [
                    {
                        "backend": backend,
                        "request_digest": evidence["materialization_reports"][
                            (configuration, backend)
                        ]["request_digest"],
                    }
                    for backend in ("memory", "sqlite")
                ],
                "materialization_report_set": evidence[
                    "materialization_report_sets"
                ][configuration],
                "materialization_contract": digest(root / MATERIALIZATION_CONTRACT),
                "sqlite_store_v3_qualification": evidence[
                    "sqlite_store_v3_qualification"
                ]["binding"],
                "g5_report": digest(evidence["g5_path"]),
                "security_report": digest(evidence["security_path"]),
                "public_callable_report": evidence["callable_evidence"]["report"]["digest"],
                "configuration": configuration,
            }
        )
        for relation in manifest["provider"]["relations"]:
            production_support.append({"provider_id": manifest["provider"]["provider_id"], "provider_version": manifest["provider"]["provider_version"], "binary_digest": worker, "relation": relation, "interpretation": manifest["provider"]["interpretation"], "toolchain": value["toolchain"]["identity"], "platform": f"linux-{platform.machine().lower()}-{configuration}", "status": "production-supported", "capabilities": manifest["provider"]["capabilities"], "guarantee": manifest["provider"]["guarantee"], "security_profile_digest": security_digest, "evidence_digest": evidence_digest})
    return production_support


def evaluation_evidence_binding(evidence: dict[str, Any]) -> dict[str, Any]:
    sqlite_binding = evidence["sqlite_store_v3_qualification"]["binding"]
    if (
        sqlite_binding.get("revision") != evidence["git"]["revision"]
        or sqlite_binding.get("source_tree") != evidence["git"]["tree"]
    ):
        fail("SQLite Store v3 qualification binding differs from release revision/tree")
    install_manifests = [
        {
            "configuration": configuration,
            "manifest_digest": evidence["install_values"][configuration][
                "manifest_digest"
            ],
            "prefix_digest": evidence["install_values"][configuration]["prefix_digest"],
        }
        for configuration in ("static", "shared")
    ]
    if evidence["materialization_evidence"]["state"] == "exact-matrix":
        materialization_reports = [
            {
                "configuration": configuration,
                "backend": backend,
                "request_digest": evidence["materialization_reports"][
                    (configuration, backend)
                ]["request_digest"],
                "request_byte_count": evidence["materialization_reports"][
                    (configuration, backend)
                ]["request_byte_count"],
                "report_digest": evidence["materialization_reports"][
                    (configuration, backend)
                ]["digest"],
                "report_byte_count": evidence["materialization_reports"][
                    (configuration, backend)
                ]["byte_count"],
                "execution_receipt_digest": evidence["materialization_reports"][
                    (configuration, backend)
                ]["execution_receipt_digest"],
                "actual_exit_status": evidence["materialization_reports"][
                    (configuration, backend)
                ]["execution_receipt"]["actual_exit_status"],
                "exact_stdout_byte_count": evidence["materialization_reports"][
                    (configuration, backend)
                ]["execution_receipt"]["exact_stdout_byte_count"],
                "stdout_digest": evidence["materialization_reports"][
                    (configuration, backend)
                ]["execution_receipt"]["stdout_sha256"],
                "parsed_response_count": evidence["materialization_reports"][
                    (configuration, backend)
                ]["execution_receipt"]["parsed_response_count"],
                "stderr_digest": evidence["materialization_reports"][
                    (configuration, backend)
                ]["execution_receipt"]["stderr_sha256"],
            }
            for configuration in ("static", "shared")
            for backend in ("memory", "sqlite")
        ]
        materialization_report_sets = [
            {
                "configuration": configuration,
                "report_set_digest": evidence["materialization_report_sets"][
                    configuration
                ],
            }
            for configuration in ("static", "shared")
        ]
    else:
        materialization_reports = []
        materialization_report_sets = []
    return {
        "foundation_report_digest": digest(evidence["foundation_path"]),
        "readiness_report_digest": digest(evidence["readiness_path"]),
        "public_callable_report_digest": evidence["callable_evidence"]["report"][
            "digest"
        ],
        "g5_report_digest": digest(evidence["g5_path"]),
        "security_report_digest": digest(evidence["security_path"]),
        "materialization_contract_digest": digest(
            evidence["root"] / MATERIALIZATION_CONTRACT
        ),
        "sqlite_store_v3_qualification": sqlite_binding,
        "materialization_evidence": evidence["materialization_evidence"],
        "install_manifests": install_manifests,
        "materialization_reports": materialization_reports,
        "materialization_report_sets": materialization_report_sets,
        "same_revision": True,
    }


def verify_qualified_evaluation(
    root: pathlib.Path,
    evaluation_path: pathlib.Path,
    evidence: dict[str, Any],
) -> dict[str, Any]:
    evaluation = load(evaluation_path)
    validate_schema(
        evaluation,
        load(root / EVALUATION_REPORT_SCHEMA),
        "release evaluation report",
    )
    if evaluation["git"] != evidence["git"]:
        fail("release evaluation is not bound to the exact GR revision/tree")
    if evaluation["scope_inventory"] != evidence["scope_inventory"]:
        fail("release evaluation scope binding differs from current Wave 0")
    if evaluation["evidence"] != evaluation_evidence_binding(evidence):
        fail("release evaluation evidence binding differs from exact GR inputs")
    if evaluation["qualification"] != "qualified" or not evaluation["qualified"]:
        fail("strict GR report requires a qualified release evaluation")
    return evaluation


def verify_gr_evaluation_artifact_binding(
    root: pathlib.Path,
    evaluation_path: pathlib.Path,
    gr_path: pathlib.Path,
) -> str:
    """Verify that a strict GR names the exact evaluation artifact it consumed."""

    evaluation = load(evaluation_path)
    gr = load(gr_path)
    validate_schema(
        evaluation,
        load(root / EVALUATION_REPORT_SCHEMA),
        "release evaluation report",
    )
    validate_schema(gr, load(root / REPORT_SCHEMA), "release report")
    if evaluation["git"] != gr["git"]:
        fail("strict GR and release evaluation use different revision/tree bindings")
    if evaluation["qualification"] != "qualified" or not evaluation["qualified"]:
        fail("strict GR terminal binding requires a qualified release evaluation")
    sqlite_binding = evaluation["evidence"]["sqlite_store_v3_qualification"]
    if (
        sqlite_binding["revision"] != evaluation["git"]["revision"]
        or sqlite_binding["source_tree"] != evaluation["git"]["tree"]
    ):
        fail("release evaluation SQLite qualification revision/tree binding differs")
    if gr["prerequisites"]["sqlite_store_v3_qualification"] != sqlite_binding:
        fail("strict GR SQLite qualification report binding differs")
    expected_digest = digest(evaluation_path)
    actual_digest = gr["prerequisites"]["release_evaluation_report_digest"]
    if actual_digest != expected_digest:
        fail(
            "strict GR release evaluation artifact digest differs: "
            f"expected={expected_digest}, actual={actual_digest}"
        )
    return expected_digest


def make_report(
    root: pathlib.Path,
    manifest: dict[str, Any],
    evidence_dir: pathlib.Path,
    security_path: pathlib.Path,
    run_url: str,
    expected_revision: str,
    generated_at: str,
    evaluation_path: pathlib.Path | None = None,
) -> dict[str, Any]:
    evidence = collect_release_evidence(
        root, manifest, evidence_dir, security_path, expected_revision
    )
    if evidence["scope_inventory"]["closure_status"] != "qualified":
        fail("strict GR report requires production scope with no tracked or blocked gaps")
    if evaluation_path is None:
        fail("strict GR report requires a qualified release evaluation artifact")
    verify_qualified_evaluation(root, evaluation_path, evidence)
    production_support = production_support_tuples(root, manifest, evidence)

    metrics = evidence["g5"]["performance"]["metrics_us"]
    report = {
        "schema": "cxxlens.ng-release-qualification-report.v1",
        "result": "passed",
        "generated_at": generated_at,
        "run_url": run_url,
        "git": evidence["git"],
        "release": {"id": "distribution-1.0", "version": "1.0.0", "state": "qualified"},
        "prerequisites": {"gates": manifest["prerequisites"]["gates"] + ["gate.release"], "migrations": manifest["prerequisites"]["migrations"], "foundation_report_digest": digest(evidence["foundation_path"]), "readiness_report_digest": digest(evidence["readiness_path"]), "public_callable_report_digest": evidence["callable_evidence"]["report"]["digest"], "g5_report_digest": digest(evidence["g5_path"]), "materialization_contract_digest": digest(root / MATERIALIZATION_CONTRACT), "sqlite_store_v3_qualification": evidence["sqlite_store_v3_qualification"]["binding"], "release_evaluation_report_digest": digest(evaluation_path), "same_revision": True},
        "packages": evidence["packages"],
        "production_support": production_support,
        "security": {"status": evidence["security"]["status"], "contract_digest": evidence["security"]["contract_digest"], "vector_count": evidence["security"]["vector_count"]},
        "performance": {"report_digest": digest(evidence["g5_path"]), "warm_zero_plan_median_us": metrics["warm_zero_plan_median"], "bounded_closure_median_us": metrics["bounded_closure_median"]},
        "documentation": {
            "doxygen_contract": "passed",
            "doxygen_callable_count": evidence["callable_evidence"]["doxygen"]["count"],
            "doxygen_digest": evidence["callable_evidence"]["doxygen"]["digest"],
            "public_callable_report": evidence["callable_evidence"]["report"],
            "public_callable_inventory": evidence["callable_evidence"]["inventory"],
            "public_callable_review": evidence["callable_evidence"]["review"],
            "support_matrix": "exact-report-only",
        },
        "negative_evidence": manifest["security"]["required_negative_evidence"],
        "authority_digests": [{"path": path.as_posix(), "digest": digest(root / path)} for path in RELEASE_AUTHORITY_PATHS],
    }
    validate_schema(report, load(root / REPORT_SCHEMA), "release report")
    return report


def make_evaluation_report(
    root: pathlib.Path,
    manifest: dict[str, Any],
    evidence_dir: pathlib.Path,
    security_path: pathlib.Path,
    run_url: str,
    expected_revision: str,
    generated_at: str,
) -> dict[str, Any]:
    evidence = collect_release_evidence(
        root, manifest, evidence_dir, security_path, expected_revision
    )
    qualified = evidence["scope_inventory"]["closure_status"] == "qualified"
    report = {
        "schema": "cxxlens.ng-release-qualification-evaluation-report.v1",
        "result": "passed",
        "generated_at": generated_at,
        "run_url": run_url,
        "git": evidence["git"],
        "release": {"id": "distribution-1.0", "version": "1.0.0"},
        "qualification": "qualified" if qualified else "not-qualified",
        "qualified": qualified,
        "scope_inventory": evidence["scope_inventory"],
        "evidence": evaluation_evidence_binding(evidence),
        "production_support": [],
        "strict_report": {
            "schema": "cxxlens.ng-release-qualification-report.v1",
            "eligible": qualified,
            "emitted": False,
        },
        "reason_codes": [] if qualified else ["release.scope-not-qualified"],
    }
    validate_schema(
        report, load(root / EVALUATION_REPORT_SCHEMA), "release evaluation report"
    )
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "command",
        choices=("check", "evaluate", "report", "verify-evaluation-binding"),
    )
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--evidence-dir", type=pathlib.Path)
    parser.add_argument("--security-report", type=pathlib.Path)
    parser.add_argument("--evaluation", type=pathlib.Path)
    parser.add_argument("--gr", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--github-output", type=pathlib.Path)
    parser.add_argument("--run-url")
    parser.add_argument("--expected-revision")
    parser.add_argument("--generated-at")
    arguments = parser.parse_args()
    try:
        root = arguments.root.resolve()
        manifest = validate_documents(
            root, require_qualified_release=arguments.command == "report"
        )
        if arguments.github_output and arguments.command != "evaluate":
            fail("--github-output is valid only for evaluate")
        if arguments.command == "verify-evaluation-binding":
            if arguments.evaluation is None or arguments.gr is None:
                fail("verify-evaluation-binding requires evaluation and GR artifacts")
            verify_gr_evaluation_artifact_binding(
                root,
                arguments.evaluation.resolve(),
                arguments.gr.resolve(),
            )
            print("strict GR release evaluation artifact binding passed")
            return 0
        if arguments.command in {"evaluate", "report"}:
            if not all((arguments.evidence_dir, arguments.security_report, arguments.output, arguments.run_url, arguments.expected_revision)):
                fail(
                    f"{arguments.command} requires evidence, security report, output, "
                    "run URL, and expected revision"
                )
            if arguments.command == "report" and arguments.evaluation is None:
                fail("report requires a qualified evaluation artifact")
            generated_at = arguments.generated_at or datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")
            if arguments.command == "evaluate":
                report = make_evaluation_report(
                    root,
                    manifest,
                    arguments.evidence_dir.resolve(),
                    arguments.security_report.resolve(),
                    arguments.run_url,
                    arguments.expected_revision,
                    generated_at,
                )
            else:
                report = make_report(
                    root,
                    manifest,
                    arguments.evidence_dir.resolve(),
                    arguments.security_report.resolve(),
                    arguments.run_url,
                    arguments.expected_revision,
                    generated_at,
                    arguments.evaluation.resolve(),
                )
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            arguments.output.write_text(json.dumps(report, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
            if arguments.command == "evaluate":
                signal = f"qualification={report['qualification']}"
                if arguments.github_output:
                    arguments.github_output.parent.mkdir(parents=True, exist_ok=True)
                    with arguments.github_output.open("a", encoding="utf-8") as output:
                        output.write(signal + "\n")
                print(signal)
                return 0
        if arguments.command == "report":
            print("distribution 1.0 strict release qualification report passed")
        else:
            print("distribution 1.0 release qualification contract check passed")
        return 0
    except (ReleaseQualificationError, OSError) as error:
        print(f"release qualification failure: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
