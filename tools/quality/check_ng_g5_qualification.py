#!/usr/bin/env python3
"""Validate and report the exact G5 closure/incrementality qualification."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = pathlib.Path("schemas/cxxlens_ng_g5_qualification.yaml")
MANIFEST_SCHEMA = pathlib.Path("schemas/cxxlens_ng_g5_qualification.schema.yaml")
REPORT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_g5_qualification_report.schema.yaml")
MATERIALIZATION_REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_clang22_materialization_report.schema.yaml"
)
ACCEPTANCE = pathlib.Path("schemas/cxxlens_ng_acceptance_manifest.yaml")
PUBLIC_API = pathlib.Path("schemas/cxxlens_ng_public_api_catalog.yaml")
QUERY_CONTRACT = pathlib.Path("schemas/cxxlens_ng_logical_query_contract.yaml")
RUNTIME_CONTRACT = pathlib.Path("schemas/cxxlens_ng_query_runtime_contract.yaml")
STORE_CONTRACT = pathlib.Path("schemas/cxxlens_ng_snapshot_store_contract.yaml")
RELEASE_BUNDLE = pathlib.Path("schemas/cxxlens_ng_release_bundle.yaml")


class G5QualificationError(ValueError):
    """A fail-closed G5 qualification violation."""


def fail(message: str) -> None:
    raise G5QualificationError(message)


def load(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail(f"expected mapping: {path}")
    return value


def load_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"production coordinator evidence is not valid JSON: {path}: {error}")
    if not isinstance(value, dict):
        fail(f"production coordinator evidence is not an object: {path}")
    return value


def validate_schema(value: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(
            schema,
            format_checker=jsonschema.Draft202012Validator.FORMAT_CHECKER,
        ).validate(value)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"{label} schema validation failed: {error.message}")


def validate_schema_definition(schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
    except jsonschema.SchemaError as error:
        fail(f"{label} schema definition is invalid: {error.message}")


def sha256(path: pathlib.Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def validate_evidence_policy(manifest: dict[str, Any]) -> None:
    expected = {
        "schema": "cxxlens.ng-g5-production-coordinator-evidence.v1",
        "input": "required",
        "source": "production-coordinator-only",
        "synthetic_planner_evidence": "non-qualifying",
        "exact_binding": ["revision", "tree", "branch", "clean"],
        "required_observations": [
            "actual-provider-execution-census",
            "warm-zero",
            "affected-only",
            "publication-outcome",
            "reopen-outcome",
        ],
    }
    actual = manifest.get("provider_hardening", {}).get("evidence_ownership", {}).get(
        "production_coordinator"
    )
    if actual != expected:
        fail("G5 production-coordinator evidence ownership policy differs")


def validate_production_report_bindings(
    report: dict[str, Any],
    evidence_git: dict[str, Any],
    production_binary_digest: str,
) -> None:
    """Bind report-owned installation facts to the supplied evidence artifacts."""

    source = report.get("source")
    if not isinstance(source, dict) or source.get("revision") != evidence_git["revision"]:
        fail("production coordinator report source revision does not match evidence Git")
    if source.get("tree") != evidence_git["tree"]:
        fail("production coordinator report source tree does not match evidence Git")

    measured = report.get("installation", {}).get("measured")
    if not isinstance(measured, dict):
        fail("production coordinator report lacks measured installation binding")
    if measured.get("source_revision") != evidence_git["revision"]:
        fail("production coordinator report installation revision does not match evidence Git")
    if measured.get("source_tree") != evidence_git["tree"]:
        fail("production coordinator report installation tree does not match evidence Git")

    tool = measured.get("tool")
    if not isinstance(tool, dict) or tool.get("digest") != production_binary_digest:
        fail(
            "production coordinator report tool digest does not match the supplied "
            "production binary"
        )


def validate_production_coordinator_evidence(
    root: pathlib.Path,
    evidence_path: pathlib.Path | None,
    expected_revision: str | None = None,
    production_binary: pathlib.Path | None = None,
    production_report: pathlib.Path | None = None,
) -> dict[str, Any]:
    if evidence_path is None:
        fail("production-coordinator evidence input is required")
    if not evidence_path.is_file():
        fail(f"production-coordinator evidence input is missing: {evidence_path}")

    report_schema = load(root / REPORT_SCHEMA)
    evidence_schema = dict(report_schema.get("$defs", {}).get("production_coordinator_evidence", {}))
    if not evidence_schema:
        fail("G5 report schema omits production-coordinator evidence definition")
    evidence_schema["$defs"] = report_schema.get("$defs", {})
    validate_schema_definition(report_schema, "G5 report")
    evidence = load_json(evidence_path)
    validate_schema(evidence, evidence_schema, "production-coordinator evidence")

    actual_git = git_state(root)
    evidence_git = evidence["git"]
    if evidence_git != actual_git:
        fail(
            "production-coordinator evidence is not bound to the exact local SHA/tree/state: "
            f"evidence={evidence_git}, actual={actual_git}"
        )
    if expected_revision is not None and evidence_git["revision"] != expected_revision:
        fail(
            "production-coordinator evidence revision differs from expected SHA: "
            f"{evidence_git['revision']} != {expected_revision}"
        )

    if production_binary is None or production_report is None:
        fail(
            "production binary and production report inputs are required when "
            "production-coordinator evidence is supplied"
        )
    if production_binary is not None and production_report is not None:
        if (
            production_binary.is_symlink()
            or not production_binary.is_file()
            or production_report.is_symlink()
            or not production_report.is_file()
        ):
            fail("production binary/report inputs must be regular files")
        production_binary_digest = sha256(production_binary)
        if production_binary_digest != evidence["producer"]["binary_digest"]:
            fail("production coordinator binary digest does not match the evidence")
        if sha256(production_report) != evidence["producer"]["report_digest"]:
            fail("production coordinator report digest does not match the evidence")
        report = load_json(production_report)
        validate_schema(
            report,
            load(root / MATERIALIZATION_REPORT_SCHEMA),
            "production coordinator report",
        )
        if report.get("response_kind") != "detailed" or report.get("result") != "passed":
            fail("production coordinator report is not a detailed passed response")
        validate_production_report_bindings(
            report,
            evidence["git"],
            production_binary_digest,
        )
        report_census = report.get("incremental_execution")
        if not isinstance(report_census, dict):
            fail("production coordinator report lacks incremental execution census")
        affected_only = evidence["execution_census"]["affected_only"]
        for field in (
            "planned_provider_executions",
            "actual_provider_executions",
            "actual_recomputed_partition_count",
            "warm_zero",
            "executed_partition_ids",
            "executed_provider_task_ids",
            "executed_provider_execution_ids",
            "executed_artifact_digests",
            "executed_task_partition_set_digests",
        ):
            if report_census.get(field) != affected_only.get(field):
                fail(f"production coordinator report census differs for {field}")
        publication = report.get("publication", {})
        if (
            publication.get("backend") != "sqlite"
            or publication.get("outcome") != "committed_verified"
            or publication.get("committed_transaction_count")
            != evidence["publication"]["committed_transaction_count"]
            or publication.get("sqlite_reopen_status") != "opened"
        ):
            fail("production coordinator report publication does not match evidence")
        if report.get("semantic_verification", {}).get("status") != "passed":
            fail("production coordinator report semantic verification is not passed")

    census = evidence["execution_census"]
    warm_zero = census["warm_zero"]
    affected_only = census["affected_only"]
    if (
        not warm_zero["warm_zero"]
        or warm_zero["affected_only"]
        or not warm_zero["exact_inputs_unchanged"]
        or warm_zero["actual_provider_executions"] != 0
        or warm_zero["actual_recomputed_partition_count"] != 0
        or warm_zero["executed_partition_ids"]
        or warm_zero["executed_provider_task_ids"]
        or warm_zero["executed_provider_execution_ids"]
        or warm_zero["executed_artifact_digests"]
        or warm_zero["executed_task_partition_set_digests"]
    ):
        fail("production-coordinator warm-zero census is not zero and unchanged")
    if (
        affected_only["warm_zero"]
        or not affected_only["affected_only"]
        or affected_only["exact_inputs_unchanged"]
        or affected_only["actual_provider_executions"] < 1
        or affected_only["actual_recomputed_partition_count"] < 1
        or not affected_only["executed_partition_ids"]
        or not affected_only["executed_provider_task_ids"]
        or not affected_only["executed_provider_execution_ids"]
    ):
        fail("production-coordinator affected-only census is incomplete")
    if (
        census["total_actual_provider_executions"]
        != warm_zero["actual_provider_executions"] + affected_only["actual_provider_executions"]
        or census["total_actual_recomputed_partition_count"]
        != warm_zero["actual_recomputed_partition_count"]
        + affected_only["actual_recomputed_partition_count"]
        or census["total_planned_provider_executions"]
        < census["total_actual_provider_executions"]
    ):
        fail("production-coordinator execution census totals are inconsistent")

    if evidence["publication"]["outcome"] != "committed_verified":
        fail("production-coordinator publication outcome is not committed_verified")
    if evidence["reopen"]["outcome"] != "opened":
        fail("production-coordinator reopen outcome is not opened")
    if evidence["independent_recompute"]["canonical_parity"] != "passed":
        fail("production-coordinator independent recomputation parity is not passed")
    return evidence


def validate_documents(root: pathlib.Path) -> dict[str, Any]:
    manifest = load(root / MANIFEST)
    validate_schema(manifest, load(root / MANIFEST_SCHEMA), "G5 manifest")
    report_schema = load(root / REPORT_SCHEMA)
    validate_schema_definition(report_schema, "G5 report")
    validate_evidence_policy(manifest)
    missing = [path for path in manifest["required_artifacts"] if not (root / path).is_file()]
    if missing:
        fail(f"required G5 artifacts are missing: {missing}")

    kinds = manifest["closure"]["registered_kinds"]
    store = load(root / STORE_CONTRACT)
    if store["closure"]["candidate_binding"]["allowed_kinds"] != manifest["closure"]["persisted_partition_kinds"]:
        fail("snapshot closure kinds differ from G5 authority")
    store_source = (root / "src/sdk/store.cpp").read_text(encoding="utf-8")
    if '"relation-key-enumeration"' not in store_source:
        fail("store validator omits persisted relation closure kind")

    query = load(root / QUERY_CONTRACT)
    operators = {row["id"]: row for row in query["operator_profiles"]}
    anti = operators.get("query.anti_join.v1")
    if anti is None or anti["profile"] != "NG1" or anti["monotonicity"] != "non-monotone-boundary":
        fail("anti_join NG1 contract is absent")
    runtime = load(root / RUNTIME_CONTRACT)
    if "query.anti_join.v1" not in runtime["execution"]["operators"]:
        fail("anti_join runtime contract is absent")
    query_source = (root / "src/sdk/query_execution.cpp").read_text(encoding="utf-8")
    for marker in (
        "sdk.query-closure-missing",
        "right coverage incomplete",
        "closure-proven-absence-filter",
    ):
        if marker not in query_source:
            fail(f"anti_join fail-closed marker is missing: {marker}")

    incremental_header = (root / "include/cxxlens/sdk/incremental.hpp").read_text(encoding="utf-8")
    incremental_source = (root / "src/sdk/incremental.cpp").read_text(encoding="utf-8")
    for kind in manifest["closure"]["bounded_derived_kinds"]:
        if f'"{kind}"' not in incremental_source:
            fail(f"bounded closure validator omits derived kind: {kind}")
    for marker in (
        "source_digest",
        "provider_semantics_digest",
        "precision_profile",
        "frontend_provider_executions",
        "bounded_transitive_closure",
        "closure_certified",
    ):
        if marker not in incremental_header:
            fail(f"incremental public contract marker is missing: {marker}")
    for marker in (
        "sdk.incremental-exact-reuse",
        "sdk.incremental-corruption-detected",
        "sdk.closure-iteration-budget",
        "sdk.closure-edge-budget",
    ):
        if marker not in incremental_source:
            fail(f"incremental implementation marker is missing: {marker}")

    provider_authority = "\n".join(
        (root / path).read_text(encoding="utf-8")
        for path in (
            "schemas/cxxlens_ng_provider_protocol.yaml",
            "schemas/cxxlens_ng_provider_runtime_contract.yaml",
            "schemas/cxxlens_ng_security_profile.yaml",
        )
    )
    for marker in (
        "prior_published_snapshot",
        "variant",
        "structured",
        "budget",
        "process-isolation",
    ):
        if marker not in provider_authority:
            fail(f"provider hardening authority marker is missing: {marker}")
    if "refresh_policy_digest" not in incremental_header:
        fail("incremental refresh binding is absent")

    catalog = load(root / PUBLIC_API)
    entries = {row["id"]: row for row in catalog["entries"]}
    if "public.incremental" not in entries or entries["public.incremental"]["status"] != "implemented":
        fail("public incremental API catalog entry is absent")
    package = next(row for row in catalog["packages"] if row["id"] == "author-sdk")
    if "include/cxxlens/sdk/incremental.hpp" not in package["headers"]:
        fail("incremental public header is absent from author SDK package")

    acceptance = load(root / ACCEPTANCE)
    gate = next(row for row in acceptance["entries"] if row["id"] == "gate.g5")
    if gate["status"] != "implemented" or gate["owner_issue"] != "#166":
        fail("gate.g5 is not implemented under Issue #166")
    if not set(manifest["required_artifacts"]).issubset(gate["evidence"]):
        fail("gate.g5 evidence omits a required qualification artifact")

    release = load(root / RELEASE_BUNDLE)
    binding = release.get("g5_qualification")
    if binding != {
        "migration": "R4",
        "gate": "gate.g5",
        "authority": MANIFEST.as_posix(),
        "ci_job": "g5-qualification",
        "status": "implemented",
    }:
        fail("release R4/G5 binding differs")
    distribution = next(row for row in release["releases"] if row["id"] == "distribution-1.0")
    release_binding = release.get("release_qualification")
    expected_release_binding = {
        "gate": "gate.release",
        "authority": "schemas/cxxlens_ng_release_qualification.yaml",
        "materialization_contract": "schemas/cxxlens_ng_clang22_materialization_contract.yaml",
        "materialization_report_matrix": [
            {"configuration": "static", "backend": "memory"},
            {"configuration": "static", "backend": "sqlite"},
            {"configuration": "shared", "backend": "memory"},
            {"configuration": "shared", "backend": "sqlite"},
        ],
        "materialization_report_set_binding": "exact-configuration-two-report-set-digest",
        "sqlite_store_v3_qualification": {
            "authority": "docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md",
            "report_schema": "schemas/cxxlens_ng_sqlite_store_v3_qualification_report.schema.yaml",
            "checker": "tools/quality/check_ng_sqlite_store_v3_qualification.py",
            "artifact": "cxxlens-ng-sqlite-store-v3-qualification-${revision}",
            "maximum_bytes": 16_777_216,
            "binding": "exact-revision-source-tree-schema-contract-artifact-and-report-set-digests",
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
    if distribution["state"] == "planned":
        if distribution["production_supported"]:
            fail("planned distribution 1.0 cannot claim production support")
    elif distribution["state"] == "qualification-in-progress":
        if distribution["production_supported"] or release_binding != expected_release_binding:
            fail(
                "qualification-in-progress distribution 1.0 must retain strict GR "
                "binding without claiming production support"
            )
    elif distribution["state"] == "qualified":
        if (
            not distribution["production_supported"]
            or release_binding != expected_release_binding
        ):
            fail("qualified distribution 1.0 lacks independent GR binding")
    else:
        fail("distribution 1.0 has an invalid G5/GR state")
    return manifest


def run_runtime(runtime: pathlib.Path, benchmark: pathlib.Path) -> dict[str, Any]:
    completed = subprocess.run([str(runtime)], check=False, capture_output=True, text=True)
    if completed.returncode != 0:
        fail(f"G5 runtime test failed: {completed.stderr.strip()}")
    completed = subprocess.run(
        [str(runtime), "--benchmark", str(benchmark)],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        fail(f"G5 benchmark failed: {completed.stderr.strip()}")
    try:
        value = json.loads(benchmark.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"G5 benchmark report invalid: {error}")
    if not isinstance(value, dict):
        fail("G5 benchmark report is not an object")
    return value


def validate_performance(manifest: dict[str, Any], value: dict[str, Any]) -> None:
    expected = manifest["performance"]
    for field in ("fixture", "method", "budgets"):
        if value.get(field) != expected[field]:
            fail(f"G5 benchmark {field} differs")
    environment = value.get("environment")
    if (
        value.get("schema") != "cxxlens.g5-performance.v1"
        or value.get("source") != "synthetic-planner"
        or not isinstance(environment, dict)
        or list(environment) != expected["environment_fields"]
        or not all(isinstance(item, str) and item for item in environment.values())
    ):
        fail("G5 benchmark provenance is incomplete")
    metrics = value.get("metrics_us", {})
    for name, maximum in expected["envelope_us"].items():
        measured = metrics.get(name)
        if not isinstance(measured, int) or measured < 0 or measured > maximum:
            fail(f"G5 performance envelope exceeded: {name}={measured}, maximum={maximum}")


def git_output(root: pathlib.Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        fail(f"git {' '.join(arguments)} failed: {completed.stderr.strip()}")
    return completed.stdout.strip()


def git_state(root: pathlib.Path) -> dict[str, Any]:
    branch = git_output(root, "branch", "--show-current")
    # GitHub Actions checks out the PR merge ref detached.  Preserve a schema-valid,
    # explicit state for that mode so binding failures report the actual SHA/tree
    # mismatch instead of failing earlier on an empty branch field.
    if not branch:
        branch = "detached"
    return {
        "revision": git_output(root, "rev-parse", "HEAD"),
        "tree": git_output(root, "rev-parse", "HEAD^{tree}"),
        "branch": branch,
        "clean": git_output(root, "status", "--porcelain=v1") == "",
    }


def make_report(
    root: pathlib.Path,
    manifest: dict[str, Any],
    performance: dict[str, Any],
    production_evidence: dict[str, Any],
    run_url: str,
    expected_revision: str,
    generated_at: str,
) -> dict[str, Any]:
    git = git_state(root)
    if git["revision"] != expected_revision or git["branch"] != "main" or not git["clean"]:
        fail(f"G5 report requires exact clean main revision: {git}")
    authority_paths = [MANIFEST, QUERY_CONTRACT, STORE_CONTRACT, RELEASE_BUNDLE, ACCEPTANCE]
    report = {
        "schema": "cxxlens.ng-g5-qualification-report.v1",
        "result": "passed",
        "generated_at": generated_at,
        "run_url": run_url,
        "git": git,
        "authority_digests": [
            {"path": path.as_posix(), "digest": sha256(root / path)}
            for path in authority_paths
        ],
        "runtime_test": "passed",
        "performance": performance,
        "production_coordinator": production_evidence,
    }
    validate_schema(report, load(root / REPORT_SCHEMA), "G5 report")
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--runtime", type=pathlib.Path)
    parser.add_argument("--benchmark", type=pathlib.Path)
    parser.add_argument("--production-evidence", type=pathlib.Path)
    parser.add_argument("--production-binary", type=pathlib.Path)
    parser.add_argument("--production-report", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--run-url")
    parser.add_argument("--expected-revision")
    parser.add_argument("--generated-at")
    arguments = parser.parse_args()
    try:
        manifest = validate_documents(arguments.root)
        production_evidence: dict[str, Any] | None = None
        if arguments.production_evidence:
            production_evidence = validate_production_coordinator_evidence(
                arguments.root,
                arguments.production_evidence.resolve(),
                arguments.expected_revision,
                arguments.production_binary.resolve() if arguments.production_binary else None,
                arguments.production_report.resolve() if arguments.production_report else None,
            )
        elif arguments.command == "report":
            # A local contract check intentionally has no production execution receipt.  A
            # report is the qualification boundary and must never be synthesized from the
            # planner/runtime fixture, so keep the missing-input failure explicit here.
            validate_production_coordinator_evidence(
                arguments.root, None, arguments.expected_revision
            )
        performance: dict[str, Any] | None = None
        if arguments.runtime:
            if arguments.benchmark:
                benchmark = arguments.benchmark
                benchmark.parent.mkdir(parents=True, exist_ok=True)
                performance = run_runtime(arguments.runtime.resolve(), benchmark)
            else:
                with tempfile.TemporaryDirectory(prefix="cxxlens-g5-") as temporary:
                    performance = run_runtime(
                        arguments.runtime.resolve(),
                        pathlib.Path(temporary) / "performance.json",
                    )
            validate_performance(manifest, performance)
        if arguments.command == "report":
            if (
                production_evidence is None
                or performance is None
                or not arguments.output
                or not arguments.run_url
                or not arguments.expected_revision
                or not arguments.production_binary
                or not arguments.production_report
            ):
                fail("report requires runtime, benchmark, output, run URL, and expected revision")
            generated_at = arguments.generated_at or datetime.datetime.now(
                datetime.timezone.utc
            ).isoformat().replace("+00:00", "Z")
            report = make_report(
                arguments.root,
                manifest,
                performance,
                production_evidence,
                arguments.run_url,
                arguments.expected_revision,
                generated_at,
            )
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            arguments.output.write_text(
                json.dumps(report, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8",
            )
        print("G5 closure/incrementality qualification passed")
        return 0
    except G5QualificationError as error:
        print(f"G5 qualification failure: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
