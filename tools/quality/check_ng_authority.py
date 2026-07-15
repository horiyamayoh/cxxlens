#!/usr/bin/env python3
"""Validate the next-generation authority transition and legacy API lock."""

from __future__ import annotations

import argparse
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
TRANSITION = "schemas/cxxlens_ng_authority_transition.yaml"
TRANSITION_SCHEMA = "schemas/cxxlens_ng_authority_transition.schema.yaml"
BASELINE = "schemas/cxxlens_legacy_api_baseline.yaml"
BASELINE_SCHEMA = "schemas/cxxlens_legacy_api_baseline.schema.yaml"
CATALOG = "schemas/cxxlens_public_api_contract.yaml"
FREEZE = "schemas/cxxlens_public_api_contract_freeze.yaml"
FREEZE_SCHEMA = "schemas/cxxlens_public_api_contract_freeze.schema.yaml"
REPORT_SCHEMA = "schemas/cxxlens_ng_authority_transition_report.schema.yaml"

PRESERVED_INVARIANTS = [
    "deterministic-reduction-and-serialization",
    "evidence-coverage-unresolved-guarantee",
    "native-lifetime-confinement",
    "no-first-wins",
    "no-shell-compilation-database",
    "no-silent-omission",
    "plan-before-side-effect",
    "process-failure-isolation",
    "root-independent-canonical-identity",
    "snapshot-bound-half-open-byte-source-span",
]


class AuthorityError(ValueError):
    """A stable authority-transition invariant violation."""


def fail(message: str) -> None:
    raise AuthorityError(message)


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail(f"expected mapping: {path}")
    return value


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def digest_value(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value).encode("utf-8")).hexdigest()


def digest_file(path: pathlib.Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def git(root: pathlib.Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=False,
        text=True,
        capture_output=True,
    )
    if completed.returncode != 0:
        fail(f"git {' '.join(arguments)} failed: {completed.stderr.strip()}")
    return completed.stdout.strip()


def schema_validate(document: dict[str, Any], schema: dict[str, Any], name: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"{name} schema validation failed: {error.message}")


def api_signatures(catalog: dict[str, Any]) -> list[dict[str, str]]:
    rows = []
    for package in catalog["packages"]:
        for api in package["apis"]:
            rows.append(
                {
                    "api_id": api["id"],
                    "atomic_unit": api["atomic_unit"]["id"],
                    "package": package["id"],
                    "signature_fingerprint": api["declaration"]["signature_fingerprint"],
                }
            )
    return sorted(rows, key=lambda row: row["api_id"])


def make_baseline(root: pathlib.Path) -> dict[str, Any]:
    catalog = load_yaml(root / CATALOG)
    freeze = load_yaml(root / FREEZE)
    signatures = api_signatures(catalog)
    states = {
        api["id"]: api["implementation_state"]
        for package in catalog["packages"]
        for api in package["apis"]
    }
    document: dict[str, Any] = {
        "schema": "cxxlens.legacy-api-baseline.v1",
        "document_version": "1.0.0",
        "captured_from": {
            "commit": git(root, "rev-parse", "HEAD"),
            "tree": git(root, "rev-parse", "HEAD^{tree}"),
            "freeze_input_fingerprint": freeze["inputs"]["fingerprint"],
        },
        "catalog": {
            "packages": len(catalog["packages"]),
            "apis": len(signatures),
            "file_sha256": digest_file(root / CATALOG),
            "signature_digest": digest_value(signatures),
            "signatures": signatures,
        },
        "implementation": {
            "conformant_count": sum(state == "conformant" for state in states.values()),
            "unimplemented_count": sum(
                state == "unimplemented" for state in states.values()
            ),
            "conformant": sorted(
                api_id for api_id, state in states.items() if state == "conformant"
            ),
            "unimplemented": sorted(
                api_id for api_id, state in states.items() if state == "unimplemented"
            ),
        },
        "acceptance": [
            {
                "profile": f"M{profile}",
                "path": f"schemas/cxxlens_m{profile}_completion.yaml",
                "sha256": digest_file(
                    root / f"schemas/cxxlens_m{profile}_completion.yaml"
                ),
            }
            for profile in range(3)
        ],
        "preserved_invariants": PRESERVED_INVARIANTS,
    }
    document["semantic_digest"] = digest_value(document)
    return document


def validate_baseline(root: pathlib.Path, baseline: dict[str, Any]) -> None:
    schema_validate(
        baseline,
        load_yaml(root / BASELINE_SCHEMA),
        "legacy API baseline",
    )
    unsigned = copy.deepcopy(baseline)
    actual_digest = unsigned.pop("semantic_digest")
    if actual_digest != digest_value(unsigned):
        fail("legacy API baseline semantic digest is stale")
    catalog = load_yaml(root / CATALOG)
    if digest_file(root / CATALOG) != baseline["catalog"]["file_sha256"]:
        fail("legacy API catalog file changed after authority reset")
    signatures = api_signatures(catalog)
    if signatures != baseline["catalog"]["signatures"]:
        fail("legacy API ID or signature surface changed after authority reset")
    if digest_value(signatures) != baseline["catalog"]["signature_digest"]:
        fail("legacy API signature digest differs")
    current_states = {
        api["id"]: api["implementation_state"]
        for package in catalog["packages"]
        for api in package["apis"]
    }
    if sorted(
        api_id for api_id, state in current_states.items() if state == "conformant"
    ) != baseline["implementation"]["conformant"]:
        fail("legacy conformant API baseline changed")
    if sorted(
        api_id for api_id, state in current_states.items() if state == "unimplemented"
    ) != baseline["implementation"]["unimplemented"]:
        fail("legacy unimplemented API baseline changed")
    for evidence in baseline["acceptance"]:
        if digest_file(root / evidence["path"]) != evidence["sha256"]:
            fail(f"legacy acceptance evidence changed: {evidence['path']}")


def validate_entrypoints(root: pathlib.Path) -> None:
    expectations = {
        "AGENTS.md": "docs/design/cxxlens_next_generation_integrated_design_ja.md",
        "README.md": "docs/design/cxxlens_next_generation_integrated_design_ja.md",
        "CONTRIBUTING.md": "旧 `schemas/cxxlens_public_api_contract.yaml` は移行 baseline",
        "docs/design/README.md": "CXXLENS-NG-SRAD-002",
        "docs/archive/legacy-v1/agent/readiness_audit_reference.md": "旧124 API Phase C authorizationはIssue #57により失効",
    }
    for relative, marker in expectations.items():
        text = (root / relative).read_text(encoding="utf-8")
        if marker not in text:
            fail(f"authority entrypoint is stale: {relative}")
    design = (root / "docs/design/cxxlens_next_generation_integrated_design_ja.md").read_text(
        encoding="utf-8"
    )
    for marker in (
        "CXXLENS-NG-SRAD-002",
        "0.3.0-normative",
        "Issue #57 により次世代 `cxxlens` の最上位規範へ昇格",
    ):
        if marker not in design:
            fail(f"normative design marker is missing: {marker}")
    for adr in range(2, 5):
        path = next((root / "docs/design/adr").glob(f"{adr:04d}-*.md"), None)
        if path is None:
            fail(f"accepted authority ADR {adr:04d} is missing")
        text = path.read_text(encoding="utf-8")
        if "- Status: Accepted" not in text or "- Decision issue: #57" not in text:
            fail(f"authority ADR is not accepted by #57: {path}")


def validate_dispatch_revocation(root: pathlib.Path) -> None:
    runner = (root / "tools/agent/api_task_runner.py").read_text(encoding="utf-8")
    workflow = (root / ".github/workflows/api-unit.yml").read_text(encoding="utf-8")
    if "runner.legacy-authority-superseded" not in runner:
        fail("legacy API runner does not fail closed after supersession")
    if "legacy-authority-superseded" not in workflow:
        fail("legacy API workflow does not fail closed after supersession")


def validate(root: pathlib.Path) -> tuple[dict[str, Any], dict[str, Any]]:
    transition = load_yaml(root / TRANSITION)
    schema_validate(
        transition,
        load_yaml(root / TRANSITION_SCHEMA),
        "NG authority transition",
    )
    baseline = load_yaml(root / BASELINE)
    validate_baseline(root, baseline)
    if transition["baseline"]["source_commit"] != baseline["captured_from"]["commit"]:
        fail("authority transition and legacy baseline source commit differ")
    if sorted(transition["preserved_invariants"]) != PRESERVED_INVARIANTS:
        fail("authority transition does not preserve the required invariants")
    superseded = {row["id"]: row for row in transition["superseded"]}
    expected_superseded = {
        "legacy-integrated-design": "docs/archive/legacy-v1/design/cxxlens_integrated_design_ja.md",
        "legacy-api-catalog": "schemas/cxxlens_public_api_contract.yaml",
        "legacy-api-freeze": "schemas/cxxlens_public_api_contract_freeze.yaml",
    }
    if set(superseded) != set(expected_superseded):
        fail("authority transition superseded set differs")
    for identifier, path in expected_superseded.items():
        row = superseded[identifier]
        if (
            row["path"] != path
            or row["disposition"] != "migration-provenance"
            or row["new_work_authorized"] is not False
        ):
            fail(f"authority transition supersession differs: {identifier}")
    freeze = load_yaml(root / FREEZE)
    schema_validate(freeze, load_yaml(root / FREEZE_SCHEMA), "legacy API freeze")
    if freeze["state"] != "superseded" or freeze["phase_c_authorized"] is not False:
        fail("legacy API freeze still authorizes Phase C")
    validate_entrypoints(root)
    validate_dispatch_revocation(root)
    for relative in transition["validation"]["inputs"]:
        if not (root / relative).is_file():
            fail(f"authority validation input is missing: {relative}")
    return transition, baseline


def exact_report(root: pathlib.Path, transition: dict[str, Any]) -> dict[str, Any]:
    status = git(root, "status", "--porcelain")
    if status:
        fail("commit-bound authority report requires a clean worktree")
    inputs = [
        {"path": path, "sha256": digest_file(root / path)}
        for path in sorted(transition["validation"]["inputs"])
    ]
    report = {
        "schema": "cxxlens.ng-authority-transition-report.v1",
        "status": "green",
        "issue": "#57",
        "tracking_issue": "#56",
        "commit": git(root, "rev-parse", "HEAD"),
        "tree": git(root, "rev-parse", "HEAD^{tree}"),
        "worktree_clean": True,
        "input_digest": digest_value(inputs),
        "inputs": inputs,
        "legacy_dispatch_authorized": False,
    }
    schema_validate(report, load_yaml(root / REPORT_SCHEMA), "NG authority report")
    return report


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("generate-baseline", "check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    if args.mode == "generate-baseline":
        baseline = make_baseline(root)
        schema_validate(
            baseline,
            load_yaml(root / BASELINE_SCHEMA),
            "legacy API baseline",
        )
        (root / BASELINE).write_text(
            yaml.safe_dump(baseline, allow_unicode=True, sort_keys=False, width=120),
            encoding="utf-8",
        )
        print(f"generated legacy API baseline {baseline['semantic_digest']}")
        return 0
    transition, baseline = validate(root)
    if args.mode == "report":
        if args.output is None:
            fail("report mode requires --output")
        output = args.output if args.output.is_absolute() else root / args.output
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(
            json.dumps(exact_report(root, transition), indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    print(
        "NG authority transition check passed: legacy 124 API surface locked, "
        f"baseline {baseline['semantic_digest']}, dispatch revoked"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        AuthorityError,
        json.JSONDecodeError,
        jsonschema.ValidationError,
        OSError,
        subprocess.SubprocessError,
        yaml.YAMLError,
    ) as error:
        print(f"NG authority transition check failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
