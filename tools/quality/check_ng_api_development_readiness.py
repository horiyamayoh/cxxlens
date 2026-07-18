#!/usr/bin/env python3
"""Validate Wave 0 API development readiness and emit an exact-SHA baseline."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = pathlib.Path("schemas/cxxlens_ng_api_development_readiness.yaml")
MANIFEST_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_api_development_readiness.schema.yaml"
)
REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_api_development_readiness_report.schema.yaml"
)
RELEASE_BUNDLE = pathlib.Path("schemas/cxxlens_ng_release_bundle.yaml")
PUBLIC_API = pathlib.Path("schemas/cxxlens_ng_public_api_catalog.yaml")
RELATION_REGISTRY = pathlib.Path("schemas/cxxlens_ng_relation_registry.yaml")
ACCEPTANCE = pathlib.Path("schemas/cxxlens_ng_acceptance_manifest.yaml")


class ReadinessError(ValueError):
    """A fail-closed Wave 0 readiness violation."""


def fail(message: str) -> None:
    raise ReadinessError(message)


def load_document(path: pathlib.Path) -> dict[str, Any]:
    text = path.read_text(encoding="utf-8")
    value = json.loads(text) if path.suffix == ".json" else yaml.safe_load(text)
    if not isinstance(value, dict):
        fail(f"expected mapping: {path}")
    return value


def validate_schema(document: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(
            schema,
            format_checker=jsonschema.Draft202012Validator.FORMAT_CHECKER,
        ).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"{label} schema validation failed: {error.message}")


def sha256(path: pathlib.Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def public_header_inventory(root: pathlib.Path) -> tuple[list[str], list[str]]:
    catalog = load_document(root / PUBLIC_API)
    registry = load_document(root / RELATION_REGISTRY)
    admitted = {
        header
        for collection in (catalog["packages"], catalog["entries"])
        for row in collection
        for header in row["headers"]
    }
    actual = {
        path.relative_to(root).as_posix()
        for path in (root / "include/cxxlens").rglob("*.hpp")
    }
    if actual != admitted:
        fail(
            "catalog-driven public header inventory differs: "
            f"missing={sorted(admitted - actual)}, extra={sorted(actual - admitted)}"
        )

    registry_relation_headers = {
        "include/cxxlens/relations/" + row["name"].replace(".", "_") + ".hpp"
        for row in registry["relations"]
        if row.get("generated_cpp_tag")
    }
    admitted_relation_headers = {
        header
        for header in admitted
        if header.startswith("include/cxxlens/relations/")
    }
    unbound = sorted(admitted_relation_headers - registry_relation_headers)
    if unbound:
        fail(f"catalog relation headers lack registry binding: {unbound}")
    return sorted(actual), sorted(admitted_relation_headers)


def cmake_direct_dependencies(root: pathlib.Path) -> dict[str, list[str]]:
    text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    internal_to_public = {
        "cxxlens_base": "cxxlens::base",
        "cxxlens_kernel": "cxxlens::kernel",
        "cxxlens_query": "cxxlens::query",
        "cxxlens_cpp": "cxxlens::cpp",
        "cxxlens_recipes": "cxxlens::recipes",
        "cxxlens": "cxxlens::cxxlens",
        "cxxlens_provider_sdk": "cxxlens::provider_sdk",
    }
    graph = {public: [] for public in internal_to_public.values()}
    for body in re.findall(r"target_link_libraries\s*\((.*?)\)", text, re.DOTALL):
        tokens = [token.strip('"') for token in re.findall(r'"[^"]*"|\S+', body)]
        if not tokens or tokens[0] not in internal_to_public:
            continue
        target = internal_to_public[tokens[0]]
        visibility = "PRIVATE"
        dependencies: list[str] = []
        for token in tokens[1:]:
            if token in {"PUBLIC", "PRIVATE", "INTERFACE"}:
                visibility = token
            elif visibility in {"PUBLIC", "INTERFACE"} and token in graph:
                dependencies.append(token)
        if graph[target]:
            fail(f"public target has multiple link declarations: {target}")
        graph[target] = dependencies
    return graph


def validate_target_contract(root: pathlib.Path, manifest: dict[str, Any]) -> None:
    expected = manifest["target_contract"]["direct_dependencies"]
    bundle = load_document(root / RELEASE_BUNDLE)
    declared = {
        row["name"]: row["direct_dependencies"]
        for row in bundle["distribution_surface"]["public_targets"]
    }
    if declared != expected:
        fail(f"release target graph differs: expected={expected}, actual={declared}")
    actual = cmake_direct_dependencies(root)
    if actual != expected:
        fail(f"CMake target graph differs: expected={expected}, actual={actual}")

    catalog = load_document(root / PUBLIC_API)
    packages = {row["id"]: row for row in catalog["packages"]}
    author = packages.get("author-sdk")
    if author is None or author["target"] != "cxxlens::provider_sdk":
        fail("author SDK package binding is missing")
    if author["link_dependencies"] != expected["cxxlens::provider_sdk"]:
        fail("author SDK package dependencies differ from the target graph")
    if "core-sdk" not in packages:
        fail("core umbrella package is absent from the public API catalog")

    umbrella = (root / "include/cxxlens/sdk.hpp").read_text(encoding="utf-8")
    for header in (
        "claim.hpp",
        "common.hpp",
        "provider.hpp",
        "query.hpp",
        "recipe.hpp",
        "relation.hpp",
        "store.hpp",
        "testing.hpp",
    ):
        if f"<cxxlens/sdk/{header}>" not in umbrella:
            fail(f"high-level author SDK umbrella is missing {header}")


def validate_gate_ownership(root: pathlib.Path, manifest: dict[str, Any]) -> None:
    entries = {
        row["id"]: row
        for row in load_document(root / ACCEPTANCE)["entries"]
    }
    expected = manifest["gate_ownership"]
    if entries["gate.foundation"]["owner_issue"] != expected["foundation"]:
        fail("Foundation gate owner differs")
    if entries["gate.g5"]["owner_issue"] != expected["g5"]:
        fail("G5 gate owner differs")
    if entries["gate.release"]["owner_issue"] != expected["release"]:
        fail("release gate owner differs")
    for identifier in ("gate.g5", "gate.release"):
        if entries[identifier]["owner_issue"] != entries[identifier]["contract_issue"]:
            fail(f"gate owner/contract issue differs: {identifier}")


def validate_workflow(root: pathlib.Path, manifest: dict[str, Any]) -> None:
    workflow = (root / ".github/workflows/quality.yml").read_text(encoding="utf-8")
    if "branches: [main]" in workflow:
        fail("pre-main exact-SHA validation requires push checks on non-main branches")
    for job in (
        "build-test:",
        "quality-contracts:",
        "install-consumer:",
        "gcc-public-headers:",
        "quality-evidence:",
        "foundation-completion:",
        "wave0-readiness:",
    ):
        if job not in workflow:
            fail(f"required CI job is missing: {job}")
    contexts = manifest["required_status_checks"]["contexts"]
    if contexts != sorted(contexts):
        fail("required status contexts must use canonical order")


def validate_documents(root: pathlib.Path) -> dict[str, Any]:
    manifest = load_document(root / MANIFEST)
    validate_schema(
        manifest,
        load_document(root / MANIFEST_SCHEMA),
        "API development readiness manifest",
    )
    validate_target_contract(root, manifest)
    public_header_inventory(root)
    migration = (root / "tools/quality/check_ng_migration_completion.py").read_text(
        encoding="utf-8"
    )
    if "ALLOWED_PUBLIC_HEADERS" in migration:
        fail("migration checker still owns a public header allowlist")
    validate_gate_ownership(root, manifest)
    validate_workflow(root, manifest)
    if len(manifest["api_unit_workflow"]["active_write_units"]) > 1:
        fail("more than one API write unit is active")
    if not (root / "docs/development/agent-api-development-goal.md").is_file():
        fail("agent API development execution contract is missing")
    return manifest


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


def current_git_state(root: pathlib.Path) -> dict[str, Any]:
    return {
        "revision": git_output(root, "rev-parse", "HEAD"),
        "tree": git_output(root, "rev-parse", "HEAD^{tree}"),
        "branch": git_output(root, "branch", "--show-current"),
        "clean": git_output(root, "status", "--porcelain=v1") == "",
    }


def file_rows(paths: list[pathlib.Path], base: pathlib.Path) -> list[dict[str, str]]:
    return [
        {"path": path.relative_to(base).as_posix(), "digest": sha256(path)}
        for path in sorted(set(paths))
    ]


def build_report(
    root: pathlib.Path,
    manifest: dict[str, Any],
    evidence_dir: pathlib.Path,
    run_url: str,
    ci_jobs: list[str],
    generated_at: str,
    expected_revision: str,
) -> dict[str, Any]:
    git = current_git_state(root)
    if git != {
        "revision": expected_revision,
        "tree": git["tree"],
        "branch": "main",
        "clean": True,
    }:
        fail(f"Wave 0 report requires exact clean main revision: {git}")
    required_jobs = [
        *manifest["required_status_checks"]["contexts"],
        "foundation-completion",
    ]
    if sorted(ci_jobs) != sorted(required_jobs):
        fail(f"Wave 0 CI job evidence differs: {ci_jobs}")

    toolchains = sorted(evidence_dir.rglob("toolchain*.json"))
    evidence = sorted(evidence_dir.rglob("*.evidence.json"))
    install_manifests = sorted(evidence_dir.rglob("install-artifact-manifest.json"))
    junit = sorted(evidence_dir.rglob("ctest-*.xml"))
    foundation_paths = sorted(
        evidence_dir.rglob("cxxlens-ng-foundation-completion-report.json")
    )
    if not toolchains or not evidence or len(install_manifests) < 2 or not junit:
        fail("Wave 0 evidence bundle is incomplete")
    if len(foundation_paths) != 1:
        fail("Wave 0 requires exactly one Foundation completion report")
    for path in toolchains:
        document = load_document(path)
        if document.get("source") != {
            "revision": git["revision"],
            "tree": git["tree"],
        }:
            fail(f"toolchain provenance source differs: {path}")

    foundation_path = foundation_paths[0]
    foundation = load_document(foundation_path)
    if foundation.get("result") != "passed" or foundation.get("git") != git:
        fail("Foundation completion report differs from Wave 0 source")

    test_cases = 0
    for path in junit:
        root_element = ET.parse(path).getroot()
        test_cases += len(root_element.findall(".//testcase"))
    if test_cases == 0:
        fail("Wave 0 test inventory is empty")

    public_headers, generated_headers = public_header_inventory(root)
    authority_paths = [
        root / MANIFEST,
        root / RELEASE_BUNDLE,
        root / PUBLIC_API,
        root / RELATION_REGISTRY,
        root / ACCEPTANCE,
    ]
    return {
        "schema": "cxxlens.ng-api-development-readiness-report.v1",
        "result": "passed",
        "generated_at": generated_at,
        "run_url": run_url,
        "git": git,
        "required_ci_jobs": required_jobs,
        "branch_protection": {
            "strict": manifest["required_status_checks"]["strict"],
            "required_statuses": manifest["required_status_checks"]["contexts"],
            "settings_evidence": "github-api-issue-168",
        },
        "authorities": file_rows(authority_paths, root),
        "public_headers": file_rows([root / path for path in public_headers], root),
        "generated_relation_headers": file_rows(
            [root / path for path in generated_headers], root
        ),
        "toolchain_provenance": file_rows(toolchains, evidence_dir),
        "evidence_artifacts": file_rows(evidence, evidence_dir),
        "install_manifests": file_rows(install_manifests, evidence_dir),
        "test_inventory": {"junit_files": len(junit), "test_cases": test_cases},
        "foundation_completion": {
            "path": foundation_path.relative_to(evidence_dir).as_posix(),
            "digest": sha256(foundation_path),
            "result": foundation["result"],
            "revision": foundation["git"]["revision"],
            "tree": foundation["git"]["tree"],
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--evidence-dir", type=pathlib.Path)
    parser.add_argument("--run-url")
    parser.add_argument("--expected-revision")
    parser.add_argument("--ci-job", action="append", default=[])
    arguments = parser.parse_args()
    root = arguments.root.resolve()
    try:
        manifest = validate_documents(root)
        if arguments.command == "check":
            print("API development Wave 0 readiness check passed")
            return 0
        if not all(
            (
                arguments.output,
                arguments.evidence_dir,
                arguments.run_url,
                arguments.expected_revision,
            )
        ):
            fail("report requires output, evidence-dir, run-url, and expected-revision")
        generated_at = (
            datetime.datetime.now(datetime.timezone.utc)
            .replace(microsecond=0)
            .isoformat()
            .replace("+00:00", "Z")
        )
        report = build_report(
            root,
            manifest,
            arguments.evidence_dir.resolve(),
            arguments.run_url,
            arguments.ci_job,
            generated_at,
            arguments.expected_revision,
        )
        validate_schema(
            report,
            load_document(root / REPORT_SCHEMA),
            "API development readiness report",
        )
        arguments.output.write_text(
            json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(f"wrote Wave 0 readiness report to {arguments.output}")
    except (
        ReadinessError,
        OSError,
        json.JSONDecodeError,
        yaml.YAMLError,
        ET.ParseError,
    ) as error:
        print(f"API development readiness check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
