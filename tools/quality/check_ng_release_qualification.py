#!/usr/bin/env python3
"""Validate and materialize the exact-SHA distribution 1.0 GR report."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import pathlib
import platform
import subprocess
import sys
import xml.etree.ElementTree as ET
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = pathlib.Path("schemas/cxxlens_ng_release_qualification.yaml")
MANIFEST_SCHEMA = pathlib.Path("schemas/cxxlens_ng_release_qualification.schema.yaml")
REPORT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_release_qualification_report.schema.yaml")
INSTALL_SCHEMA = pathlib.Path("schemas/cxxlens_ng_install_artifact_manifest.schema.yaml")
G5_REPORT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_g5_qualification_report.schema.yaml")
FOUNDATION_REPORT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_foundation_completion_report.schema.yaml")
READINESS_REPORT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_api_development_readiness_report.schema.yaml")
SECURITY_REPORT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_security_conformance_report.schema.yaml")
ACCEPTANCE = pathlib.Path("schemas/cxxlens_ng_acceptance_manifest.yaml")
RELEASE = pathlib.Path("schemas/cxxlens_ng_release_bundle.yaml")
SUPPORT = pathlib.Path("schemas/cxxlens_ng_provider_support_matrix.yaml")
SECURITY = pathlib.Path("schemas/cxxlens_ng_security_profile.yaml")
REGISTRY = pathlib.Path("schemas/cxxlens_ng_relation_registry.yaml")


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


def validate_schema(value: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(
            schema, format_checker=jsonschema.Draft202012Validator.FORMAT_CHECKER
        ).validate(value)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"{label} schema validation failed: {error.message}")


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


def validate_documents(root: pathlib.Path) -> dict[str, Any]:
    manifest = load(root / MANIFEST)
    validate_schema(manifest, load(root / MANIFEST_SCHEMA), "release qualification")
    validate_schema(
        {
            "schema": "cxxlens.ng-release-qualification-report.v1",
            "result": "passed",
            "generated_at": "2026-07-18T00:00:00Z",
            "run_url": "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
            "git": {"revision": "1" * 40, "tree": "2" * 40, "branch": "main", "clean": True},
            "release": {"id": "distribution-1.0", "version": "1.0.0", "state": "qualified"},
            "prerequisites": {"gates": [f"gate.g{i}" for i in range(6)] + ["gate.release"], "migrations": [f"R{i}" for i in range(5)], "foundation_report_digest": "sha256:" + "1" * 64, "readiness_report_digest": "sha256:" + "2" * 64, "g5_report_digest": "sha256:" + "3" * 64, "same_revision": True},
            "packages": [
                {"configuration": configuration, "prefix_digest": "sha256:" + digit * 64, "manifest_digest": "sha256:" + digit * 64, "toolchain_digest": "sha256:" + digit * 64, "real_project": "passed", "storage_backends": ["memory", "sqlite"], "relocated": True, "license": "sha256:" + digit * 64, "notice": "sha256:" + digit * 64}
                for configuration, digit in (("static", "1"), ("shared", "2"))
            ],
            "production_support": [
                {"provider_id": "cxxlens.clang22.reference", "provider_version": "1.0.0", "binary_digest": "sha256:" + digit * 64, "relation": relation, "interpretation": "cc.clang22-canonical-1", "toolchain": "clang version 22.1.0", "platform": f"linux-x86_64-{configuration}", "status": "production-supported", "capabilities": ["canonical-entity", "call-site", "direct-target", "process-isolation"], "guarantee": "exact-claims-with-coverage-unresolved-and-provenance", "security_profile_digest": "sha256:" + "3" * 64, "evidence_digest": "sha256:" + "4" * 64}
                for configuration, digit in (("static", "1"), ("shared", "2"))
                for relation in ("cc.entity@1", "cc.call_site@1", "cc.call_direct_target@1")
            ],
            "security": {"status": "green", "contract_digest": "sha256:" + "3" * 64, "vector_count": 1},
            "performance": {"report_digest": "sha256:" + "4" * 64, "warm_zero_plan_median_us": 1, "bounded_closure_median_us": 1},
            "documentation": {"doxygen_contract": "passed", "support_matrix": "exact-report-only"},
            "negative_evidence": manifest["security"]["required_negative_evidence"],
            "authority_digests": [{"path": f"authority-{index}", "digest": "sha256:" + str(index) * 64} for index in range(1, 7)],
        },
        load(root / REPORT_SCHEMA),
        "release report",
    )

    missing = [path for path in manifest["required_artifacts"] if not (root / path).is_file()]
    if missing:
        fail(f"required GR artifacts are missing: {missing}")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    if "VERSION 1.0.0" not in cmake or "LICENSE NOTICE" not in cmake:
        fail("distribution package is not version 1.0.0 with license/notice")

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
    if row["state"] != "qualified" or row["production_supported"] is not True:
        fail("distribution 1.0 is not qualified")
    expected_release_binding = {
        "gate": "gate.release",
        "authority": MANIFEST.as_posix(),
        "checker": "tools/quality/check_ng_release_qualification.py",
        "ci_job": "release-qualification",
        "status": "implemented",
        "claim_scope": "exact-gr-report-tuples-only",
        "report_artifact": "cxxlens-ng-release-qualification-${revision}",
        "required_evidence": [
            "same-sha-foundation-report",
            "same-sha-wave0-readiness-report",
            "same-sha-g5-report",
            "static-relocated-install-artifact",
            "shared-relocated-install-artifact",
            "static-shared-runtime-junit",
            "real-project-memory-sqlite-and-major-rejection",
            "security-conformance-and-negative-paths",
            "doxygen-contract-and-support-matrix",
            "license-and-notice",
        ],
    }
    if release.get("release_qualification") != expected_release_binding:
        fail("release qualification binding differs")

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
    for marker in (
        "cc::relations::entity::descriptor()",
        "cc::relations::call_site::descriptor()",
        "cc::relations::call_direct_target::descriptor()",
        '"cc.clang22-canonical-1"',
    ):
        if marker not in worker_source:
            fail(f"Clang 22 worker production surface marker is missing: {marker}")
    workflow = (root / ".github/workflows/quality.yml").read_text(encoding="utf-8")
    for marker in ("release-qualification:", "needs: [g5-qualification]", "check_ng_release_qualification.py report"):
        if marker not in workflow:
            fail(f"release qualification workflow marker is missing: {marker}")
    install = (root / "tests/install/run_install_test.cmake.in").read_text(encoding="utf-8")
    for marker in ("real-project-consumer", "share/doc/cxxlens/LICENSE", "share/doc/cxxlens/NOTICE", "libcxxlens_base.so.1"):
        if marker not in install:
            fail(f"release install qualification marker is missing: {marker}")
    return manifest


def find_one(root: pathlib.Path, name: str) -> pathlib.Path:
    matches = sorted(path for path in root.rglob(name) if path.is_file())
    if len(matches) != 1:
        fail(f"expected exactly one {name}, found {len(matches)}")
    return matches[0]


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
        if not candidate.is_file() or digest(candidate) != files[relative]["digest"]:
            fail(f"installed file digest mismatch: {configuration}:{relative}")
    return configuration, value


def make_report(root: pathlib.Path, manifest: dict[str, Any], evidence: pathlib.Path, security_path: pathlib.Path, run_url: str, expected_revision: str, generated_at: str) -> dict[str, Any]:
    git = git_state(root)
    if git != {"revision": expected_revision, "tree": git["tree"], "branch": "main", "clean": True}:
        fail(f"GR report requires exact clean main revision: {git}")

    install_paths = sorted(evidence.rglob("install-artifact-manifest.json"))
    if len(install_paths) != 2:
        fail(f"GR requires exactly static/shared install manifests, found {len(install_paths)}")
    packages: list[dict[str, Any]] = []
    install_values: dict[str, dict[str, Any]] = {}
    for path in install_paths:
        configuration, value = verify_install_manifest(root, path, git["revision"], git["tree"], manifest["package"]["required_files"])
        if configuration in install_values:
            fail(f"duplicate install configuration: {configuration}")
        install_values[configuration] = value
        xml = find_one(path.parent, f"ctest-install-{'ON' if configuration == 'shared' else 'OFF'}.xml")
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

    build_xml = sorted(evidence.rglob("ctest-build-*.xml"))
    if len(build_xml) != 2:
        fail(f"GR requires exactly static/shared runtime JUnit, found {len(build_xml)}")
    runtime_tests = set().union(*(junit_tests(path) for path in build_xml))
    for test in manifest["provider"]["required_positive_evidence"] + manifest["security"]["required_negative_evidence"][:2]:
        if test not in runtime_tests:
            fail(f"provider runtime evidence is missing: {test}")

    foundation_path = find_one(evidence, "cxxlens-ng-foundation-completion-report.json")
    foundation = load(foundation_path)
    validate_schema(foundation, load(root / FOUNDATION_REPORT_SCHEMA), "foundation report")
    readiness_path = find_one(evidence, "cxxlens-ng-api-development-readiness-report.json")
    readiness = load(readiness_path)
    validate_schema(readiness, load(root / READINESS_REPORT_SCHEMA), "readiness report")
    for label, value in (("foundation", foundation), ("readiness", readiness)):
        if value["git"]["revision"] != git["revision"] or value["git"]["tree"] != git["tree"] or value["result"] != "passed":
            fail(f"{label} evidence is not from the exact GR revision")

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

    security_digest = digest(root / SECURITY)
    production_support = []
    for configuration in ("static", "shared"):
        value = install_values[configuration]
        files = {row["path"]: row["digest"] for row in value["files"]}
        worker = files["bin/cxxlens-clang-worker-22"]
        evidence_digest = canonical_digest({"install_manifest": value["manifest_digest"], "g5_report": digest(g5_path), "security_report": digest(security_path), "configuration": configuration})
        for relation in manifest["provider"]["relations"]:
            production_support.append({"provider_id": manifest["provider"]["provider_id"], "provider_version": manifest["provider"]["provider_version"], "binary_digest": worker, "relation": relation, "interpretation": manifest["provider"]["interpretation"], "toolchain": value["toolchain"]["identity"], "platform": f"linux-{platform.machine().lower()}-{configuration}", "status": "production-supported", "capabilities": manifest["provider"]["capabilities"], "guarantee": manifest["provider"]["guarantee"], "security_profile_digest": security_digest, "evidence_digest": evidence_digest})

    metrics = g5["performance"]["metrics_us"]
    authority_paths = [MANIFEST, RELEASE, ACCEPTANCE, SUPPORT, SECURITY, REGISTRY]
    report = {
        "schema": "cxxlens.ng-release-qualification-report.v1",
        "result": "passed",
        "generated_at": generated_at,
        "run_url": run_url,
        "git": git,
        "release": {"id": "distribution-1.0", "version": "1.0.0", "state": "qualified"},
        "prerequisites": {"gates": manifest["prerequisites"]["gates"] + ["gate.release"], "migrations": manifest["prerequisites"]["migrations"], "foundation_report_digest": digest(foundation_path), "readiness_report_digest": digest(readiness_path), "g5_report_digest": digest(g5_path), "same_revision": True},
        "packages": packages,
        "production_support": production_support,
        "security": {"status": security["status"], "contract_digest": security["contract_digest"], "vector_count": security["vector_count"]},
        "performance": {"report_digest": digest(g5_path), "warm_zero_plan_median_us": metrics["warm_zero_plan_median"], "bounded_closure_median_us": metrics["bounded_closure_median"]},
        "documentation": {"doxygen_contract": "passed", "support_matrix": "exact-report-only"},
        "negative_evidence": manifest["security"]["required_negative_evidence"],
        "authority_digests": [{"path": path.as_posix(), "digest": digest(root / path)} for path in authority_paths],
    }
    validate_schema(report, load(root / REPORT_SCHEMA), "release report")
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--evidence-dir", type=pathlib.Path)
    parser.add_argument("--security-report", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--run-url")
    parser.add_argument("--expected-revision")
    parser.add_argument("--generated-at")
    arguments = parser.parse_args()
    try:
        root = arguments.root.resolve()
        manifest = validate_documents(root)
        if arguments.command == "report":
            if not all((arguments.evidence_dir, arguments.security_report, arguments.output, arguments.run_url, arguments.expected_revision)):
                fail("report requires evidence, security report, output, run URL, and expected revision")
            generated_at = arguments.generated_at or datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")
            report = make_report(root, manifest, arguments.evidence_dir.resolve(), arguments.security_report.resolve(), arguments.run_url, arguments.expected_revision, generated_at)
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            arguments.output.write_text(json.dumps(report, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
        print("distribution 1.0 release qualification passed")
        return 0
    except (ReleaseQualificationError, OSError) as error:
        print(f"release qualification failure: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
