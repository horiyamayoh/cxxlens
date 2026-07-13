#!/usr/bin/env python3
"""Fail-closed gate for issue #52 high-risk Contract Candidate validation."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import re
import subprocess
import sys
from typing import Any

import jsonschema
import yaml


MANIFEST = "schemas/cxxlens_high_risk_contract_validation.yaml"
SCHEMA = "schemas/cxxlens_high_risk_contract_validation.schema.yaml"
CANDIDATES = "schemas/cxxlens_package_contract_candidates.yaml"
CATALOG = "schemas/cxxlens_public_api_contract.yaml"
EXPECTED_DOMAINS = {
    "graph",
    "flow_resource",
    "transform_transaction",
    "generation_surface",
    "review_gate",
    "qa_process_coverage",
    "interop_extractor",
}


class ValidationError(ValueError):
    """Stable high-risk gate failure."""


def fail(code: str, detail: str) -> None:
    raise ValidationError(f"{code}: {detail}")


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail("high-risk.document", str(path))
    return value


def load_json(path: pathlib.Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail("high-risk.evidence", str(path))
    return value


def semantic_digest(value: dict[str, Any]) -> str:
    unsigned = copy.deepcopy(value)
    unsigned.pop("semantic_digest", None)
    encoded = json.dumps(unsigned, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return "sha256:" + hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def file_sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(
    document: dict[str, Any],
    schema: dict[str, Any],
    candidates: dict[str, Any],
    catalog: dict[str, Any],
    evidence: dict[str, Any],
    root: pathlib.Path,
    *,
    reproduce: bool = True,
) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail("high-risk.schema", error.message)

    groups = {group["issue"]: group for group in candidates["groups"]}
    snapshots = {row["issue"]: row for row in document["candidate_snapshot"]}
    expected_issues = {f"#{number}" for number in range(43, 52)}
    if set(snapshots) != expected_issues or set(groups) != expected_issues:
        fail("high-risk.candidate-coverage", f"snapshot={sorted(snapshots)} groups={sorted(groups)}")
    for issue, snapshot in snapshots.items():
        if snapshot["fingerprint"] != groups[issue]["candidate_fingerprint"]:
            fail("high-risk.candidate-drift", issue)
        backlink = root / snapshot["backlink"].split("#", 1)[0]
        if not backlink.is_file() or "## Issue #52 validation backlink" not in backlink.read_text(encoding="utf-8"):
            fail("high-risk.backlink", issue)

    catalog_apis = {
        api["id"]: (package, api)
        for package in catalog["packages"]
        for api in package["apis"]
    }
    if any(package["contract"]["owner_issue"] == "#52" for package in catalog["packages"]):
        fail("high-risk.api-ownership", "#52 must not own a package or API")

    domains = {row["id"]: row for row in document["domains"]}
    if set(domains) != EXPECTED_DOMAINS or set(document["gate"]["required_domains"]) != EXPECTED_DOMAINS:
        fail("high-risk.domain-coverage", str(sorted(domains)))
    for domain_id, domain in domains.items():
        if domain["result"] != "validated" or domain["contract_changes"]:
            fail("high-risk.unresolved-result", domain_id)
        for fixture_kind in ("positive", "negative", "ambiguous"):
            if fixture_kind not in domain["fixtures"]:
                fail("high-risk.fixture-coverage", f"{domain_id}:{fixture_kind}")
        for binding in domain["candidate_bindings"]:
            issue = binding["issue"]
            if issue not in groups or binding["fingerprint"] != groups[issue]["candidate_fingerprint"]:
                fail("high-risk.binding-drift", f"{domain_id}:{issue}")
            group_api_ids = {row["api_id"] for row in groups[issue]["api_contracts"]}
            if not set(binding["api_ids"]).issubset(group_api_ids):
                fail("high-risk.api-binding", f"{domain_id}:{issue}")
            if any(api_id not in catalog_apis for api_id in binding["api_ids"]):
                fail("high-risk.unknown-api", domain_id)
        for command in domain["commands"]:
            argv = command["argv"]
            if argv[:2] in (["sh", "-c"], ["bash", "-c"], ["/bin/sh", "-c"], ["/bin/bash", "-c"]):
                fail("high-risk.shell-command", domain_id)
        for path in domain["retained_artifacts"]:
            if path.startswith(("include/", "src/")):
                fail("high-risk.production-artifact", path)
            if not (root / path).is_file():
                fail("high-risk.missing-artifact", path)

    evidence_path = root / document["evidence"]["path"]
    if file_sha256(evidence_path) != document["evidence"]["sha256"]:
        fail("high-risk.evidence-byte-drift", str(evidence_path))
    if evidence.get("semantic_digest") != semantic_digest(evidence):
        fail("high-risk.evidence-digest", "evidence semantic digest is invalid")
    if evidence["semantic_digest"] != document["evidence"]["semantic_digest"]:
        fail("high-risk.evidence-manifest-drift", evidence["semantic_digest"])
    if set(evidence.get("results", {})) != EXPECTED_DOMAINS or evidence.get("spike_count") != 7:
        fail("high-risk.evidence-coverage", str(sorted(evidence.get("results", {}))))
    for domain_id, result in evidence["results"].items():
        if result.get("result") != "validated" or not all(result.get("assertions", {}).values()):
            fail("high-risk.evidence-failed", domain_id)

    install_text = "\n".join(
        path.read_text(encoding="utf-8")
        for path in [root / "CMakeLists.txt", *(root / "cmake").glob("*.cmake")]
    )
    for block in re.findall(r"install\s*\((.*?)\)", install_text, flags=re.DOTALL):
        if "contract_spikes" in block or "high_risk_contract_spike" in block:
            fail("high-risk.installed-spike", block[:80])

    if reproduce:
        completed = subprocess.run(
            [sys.executable, str(root / document["spike_harness"]), "check"],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
        )
        if completed.returncode != 0:
            fail("high-risk.evidence-not-reproducible", completed.stderr or completed.stdout)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path.cwd())
    args = parser.parse_args()
    root = args.root.resolve()
    document = load_yaml(root / MANIFEST)
    evidence = load_json(root / document["evidence"]["path"])
    validate(
        document,
        load_yaml(root / SCHEMA),
        load_yaml(root / CANDIDATES),
        load_yaml(root / CATALOG),
        evidence,
        root,
    )
    print(
        "validated high-risk contract gate: "
        f"{len(document['candidate_snapshot'])} candidates, {len(document['domains'])}/7 domains, "
        f"evidence {document['evidence']['semantic_digest']}, production surface clean"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValidationError, OSError, json.JSONDecodeError, yaml.YAMLError) as error:
        print(f"high-risk contract validation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
