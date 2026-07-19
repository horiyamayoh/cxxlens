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


def validate_schema(value: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(
            schema,
            format_checker=jsonschema.Draft202012Validator.FORMAT_CHECKER,
        ).validate(value)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"{label} schema validation failed: {error.message}")


def sha256(path: pathlib.Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def validate_documents(root: pathlib.Path) -> dict[str, Any]:
    manifest = load(root / MANIFEST)
    validate_schema(manifest, load(root / MANIFEST_SCHEMA), "G5 manifest")
    validate_schema(
        {
            "schema": "cxxlens.ng-g5-qualification-report.v1",
            "result": "passed",
            "generated_at": "2026-07-18T00:00:00Z",
            "run_url": "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
            "git": {"revision": "1" * 40, "tree": "2" * 40, "branch": "main", "clean": True},
            "authority_digests": [
                {"path": f"authority-{index}", "digest": "sha256:" + str(index) * 64}
                for index in range(1, 5)
            ],
            "runtime_test": "passed",
            "performance": {
                "schema": "cxxlens.g5-performance.v1",
                "fixture": {"partitions": 2048, "edges": 512},
                "method": {"clock": "steady_clock", "repetitions": 5, "statistic": "median"},
                "budgets": {"max_iterations": 513, "max_edges": 512},
                "metrics_us": {"warm_zero_plan_median": 1, "bounded_closure_median": 1},
                "environment": {
                    "compiler": "test",
                    "operating_system": "test-os",
                    "architecture": "test-arch",
                },
            },
        },
        load(root / REPORT_SCHEMA),
        "G5 report schema",
    )
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
            "static-relocated-install-artifact",
            "shared-relocated-install-artifact",
            "static-shared-runtime-junit",
            "static-shared-clang22-materialization-reports",
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
    return {
        "revision": git_output(root, "rev-parse", "HEAD"),
        "tree": git_output(root, "rev-parse", "HEAD^{tree}"),
        "branch": git_output(root, "branch", "--show-current"),
        "clean": git_output(root, "status", "--porcelain=v1") == "",
    }


def make_report(
    root: pathlib.Path,
    manifest: dict[str, Any],
    performance: dict[str, Any],
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
    }
    validate_schema(report, load(root / REPORT_SCHEMA), "G5 report")
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--runtime", type=pathlib.Path)
    parser.add_argument("--benchmark", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--run-url")
    parser.add_argument("--expected-revision")
    parser.add_argument("--generated-at")
    arguments = parser.parse_args()
    try:
        manifest = validate_documents(arguments.root)
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
            if performance is None or not arguments.output or not arguments.run_url or not arguments.expected_revision:
                fail("report requires runtime, benchmark, output, run URL, and expected revision")
            generated_at = arguments.generated_at or datetime.datetime.now(
                datetime.timezone.utc
            ).isoformat().replace("+00:00", "Z")
            report = make_report(
                arguments.root,
                manifest,
                performance,
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
