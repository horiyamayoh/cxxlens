#!/usr/bin/env python3
"""Validate the NG foundation and emit a final commit-bound completion report."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from typing import Any

import jsonschema
import yaml

from collect_toolchain_provenance import pinned_actions, provenance_digest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = pathlib.Path("schemas/cxxlens_ng_foundation_completion_manifest.yaml")
MANIFEST_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_foundation_completion_manifest.schema.yaml"
)
REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_foundation_completion_report.schema.yaml"
)
AUDIT_REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_foundation_audit_report.schema.yaml"
)
AUDIT_CHECKER = pathlib.Path("tools/quality/check_ng_foundation_audits.py")
ACCEPTANCE = pathlib.Path("schemas/cxxlens_ng_acceptance_manifest.yaml")
ACCEPTANCE_SCHEMA = pathlib.Path("schemas/cxxlens_ng_acceptance_manifest.schema.yaml")
RELEASE_BUNDLE = pathlib.Path("schemas/cxxlens_ng_release_bundle.yaml")
SUPPORT_MATRIX = pathlib.Path("schemas/cxxlens_ng_provider_support_matrix.yaml")
PUBLIC_API = pathlib.Path("schemas/cxxlens_ng_public_api_catalog.yaml")
LEDGER = pathlib.Path("schemas/cxxlens_asset_migration_ledger.json")
EXPECTED_ZERO_AUDITS = {
    "legacy_assets",
    "legacy_authority_references",
    "legacy_code_paths",
    "legacy_schemas",
    "legacy_public_headers",
    "legacy_ci_gates",
    "migration_blockers",
    "unowned_contracts",
    "documentation_drift",
}


class CompletionError(ValueError):
    """A fail-closed foundation completion violation."""


def fail(message: str) -> None:
    raise CompletionError(message)


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


def canonical_digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(
        json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode(
            "utf-8"
        )
    ).hexdigest()


def load_provenance_directory(directory: pathlib.Path) -> list[dict[str, Any]]:
    records = []
    for path in sorted(directory.rglob("toolchain*.json")):
        value = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(value, dict):
            fail(f"toolchain provenance is not a mapping: {path}")
        records.append(value)
    if not records:
        fail("foundation completion requires toolchain provenance records")
    return records


def summarize_supply_chain(
    root: pathlib.Path,
    records: list[dict[str, Any]],
    git_state: dict[str, Any],
) -> dict[str, Any]:
    lock_digest = sha256(root / "tools/ci/llvm22-noble.lock.json")
    requirements_digest = sha256(root / "tools/quality/requirements.lock")
    expected_source = {
        "revision": git_state["revision"],
        "tree": git_state["tree"],
    }
    expected_actions = pinned_actions(root)
    for record in records:
        if record.get("digest") != provenance_digest(record):
            fail("toolchain provenance digest mismatch")
        if record.get("source") != expected_source:
            fail("toolchain provenance source differs from completion source")
        supply_chain = record.get("supply_chain", {})
        if supply_chain.get("lock_digest") != lock_digest:
            fail("toolchain provenance supply-chain lock mismatch")
        if supply_chain.get("requirements_digest") != requirements_digest:
            fail("toolchain provenance requirements lock mismatch")
        if record.get("actions") != expected_actions:
            fail("toolchain provenance action set mismatch")
        runner = record.get("runner")
        if not isinstance(runner, dict) or runner.get("image_os") in (None, "unavailable") or runner.get(
            "image_version"
        ) in (None, "unavailable"):
            fail("toolchain provenance lacks hosted runner image identity")

    def unique_rows(field: str) -> list[Any]:
        values = {
            json.dumps(row, ensure_ascii=False, sort_keys=True, separators=(",", ":")): row
            for record in records
            for row in (
                record.get(field, [])
                if isinstance(record.get(field), list)
                else [record.get(field)]
            )
            if row is not None
        }
        return [values[key] for key in sorted(values)]

    runners = unique_rows("runner")
    tools = unique_rows("tools")
    packages = unique_rows("packages")
    python_packages = unique_rows("python_distributions")
    if not packages or any("package_digest" not in row for row in packages):
        fail("toolchain provenance lacks package digests")
    if not python_packages or any("record_digest" not in row for row in python_packages):
        fail("toolchain provenance lacks Python distribution digests")
    return {
        "schema": "cxxlens.ci-supply-chain-summary.v1",
        "lock_digest": lock_digest,
        "requirements_digest": requirements_digest,
        "provenance_digests": sorted({record["digest"] for record in records}),
        "actions": expected_actions,
        "actions_digest": canonical_digest(expected_actions),
        "runners": runners,
        "tools": tools,
        "packages": packages,
        "python_distributions": python_packages,
    }


def rows_by_id(rows: list[dict[str, Any]], label: str) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for row in rows:
        identifier = row["id"]
        if identifier in result:
            fail(f"{label} contains duplicate ID: {identifier}")
        result[identifier] = row
    return result


def run_check(root: pathlib.Path, relative: str, *arguments: str) -> None:
    completed = subprocess.run(
        [sys.executable, str(root / relative), *arguments],
        cwd=root,
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        fail(f"{relative} failed: {detail}")


def validate_versions(root: pathlib.Path, expected: dict[str, str]) -> None:
    relation = load_document(root / "schemas/cxxlens_ng_relation_registry.yaml")
    logical = load_document(root / "schemas/cxxlens_ng_logical_query_contract.yaml")
    runtime = load_document(root / "schemas/cxxlens_ng_query_runtime_contract.yaml")
    snapshot = load_document(root / "schemas/cxxlens_ng_snapshot_store_contract.yaml")
    sqlite = load_document(root / "schemas/cxxlens_ng_sqlite_store_contract.yaml")
    actual = {
        "relation_registry": relation["compatibility"]["current"],
        "claim_envelope": relation["system_claim_envelope"]["schema"],
        "identity_encoding": snapshot["canonical_encoding"]["format"],
        "condition_semantics": logical["condition_algebra"]["representation"],
        "logical_query_ir": logical["compatibility"]["current"],
        "query_runtime": runtime["compatibility"]["current"],
        "provider_protocol": load_document(
            root / "schemas/cxxlens_ng_provider_protocol.yaml"
        )["compatibility"]["current"],
        "snapshot_identity": snapshot["compatibility"]["current"],
        "snapshot_payload": sqlite["payload"]["write_schema"],
        "sqlite_physical_format": sqlite["physical_format"]["current"],
        "recipe_semantics": "cxxlens.recipes.calls_to_function-1.2.0",
    }
    if actual != expected:
        fail(f"foundation version contract differs: expected={expected}, actual={actual}")


def validate_documents(root: pathlib.Path) -> dict[str, Any]:
    manifest = load_document(root / MANIFEST)
    validate_schema(
        manifest,
        load_document(root / MANIFEST_SCHEMA),
        "foundation completion manifest",
    )

    acceptance = load_document(root / ACCEPTANCE)
    validate_schema(
        acceptance,
        load_document(root / ACCEPTANCE_SCHEMA),
        "acceptance manifest",
    )
    gates = rows_by_id(acceptance["entries"], "acceptance manifest")
    required_implemented = {
        "gate.g0",
        "gate.g1",
        "gate.g2",
        "gate.g3",
        "gate.g4",
        "gate.ci-supply-chain",
        "gate.quality-evidence",
        "gate.foundation",
    }
    if any(gates[identifier]["status"] != "implemented" for identifier in required_implemented):
        fail("G0-G4 and gate.foundation must be implemented")
    if gates["gate.release"]["status"] != "deferred":
        fail("distribution release must remain explicitly deferred")
    owned_gate_ids = ("gate.g5", "gate.release")
    for identifier in owned_gate_ids:
        if gates[identifier]["owner_issue"] != gates[identifier]["contract_issue"]:
            fail(f"gate owner/contract issue differs: {identifier}")
    deferred_issue_refs = {
        gates["gate.release"]["owner_issue"]
    }
    if gates["gate.g5"]["owner_issue"] == gates["gate.release"]["owner_issue"]:
        fail("G5 and distribution release require distinct tracking issues")
    completed_issue_refs = {
        reference
        for entry in gates.values()
        if entry["status"] == "implemented"
        for reference in (entry["owner_issue"], entry["contract_issue"])
    }
    completed_issue_refs.update(
        f"#{number}" for number in manifest["required_closed_issues"]
    )
    completed_issue_refs.update(
        (manifest["authority"]["gate_issue"], manifest["authority"]["tracking_issue"])
    )
    reused_issue_refs = sorted(deferred_issue_refs & completed_issue_refs)
    if reused_issue_refs:
        fail(f"deferred gates reuse completed issue contracts: {reused_issue_refs}")

    release = load_document(root / RELEASE_BUNDLE)
    if release["distribution_surface"]["implementation_state"] != "implemented":
        fail("distribution surface is not implemented")

    support = load_document(root / SUPPORT_MATRIX)
    blockers = [row["id"] for row in support["entries"] if row["blocker_issue"] is not None]
    if blockers:
        fail(f"provider support matrix contains migration blockers: {blockers}")

    public_api = load_document(root / PUBLIC_API)
    pending = [
        row["id"] for row in public_api["entries"] if row["status"] != "implemented"
    ]
    if public_api["maturity"] != "implemented" or pending:
        fail(f"public API catalog contains non-implemented entries: {pending}")

    if set(manifest["zero_audits"]) != EXPECTED_ZERO_AUDITS:
        fail("foundation zero-audit set differs from the accepted gate")

    machine_authorities = [
        relative
        for relative in manifest["authority_documents"]
        if relative.startswith("schemas/") and relative.endswith(".yaml")
    ]
    for relative in machine_authorities:
        authority = load_document(root / relative).get("authority")
        if not isinstance(authority, dict) or not any(
            key in authority
            for key in ("owner", "owner_issue", "decision_issue", "exact_contract_issue")
        ):
            fail(f"foundation machine authority is unowned: {relative}")

    for relative in [
        *manifest["authority_documents"],
        *manifest["evidence"]["required_paths"],
        *[
            path
            for paths in manifest["evidence"]["claims"].values()
            for path in paths
        ],
        *manifest["handoff_documents"],
    ]:
        if not (root / relative).is_file():
            fail(f"foundation evidence path is missing: {relative}")

    validate_versions(root, manifest["version_contracts"])
    run_check(
        root,
        "tools/quality/check_ng_migration_completion.py",
        "check",
        "--root",
        str(root),
    )
    run_check(
        root,
        "tools/quality/check_documentation_consistency.py",
        "check",
        "--root",
        str(root),
    )
    run_check(
        root,
        "tools/quality/verify_checksums.py",
        "check",
        "--root",
        str(root),
    )
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


def github_issue_states(repository: str, issue_numbers: list[int], token: str) -> dict[int, str]:
    result: dict[int, str] = {}
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": "cxxlens-foundation-completion",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"
    for number in issue_numbers:
        request = urllib.request.Request(
            f"https://api.github.com/repos/{repository}/issues/{number}",
            headers=headers,
        )
        try:
            with urllib.request.urlopen(request, timeout=20) as response:
                document = json.load(response)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            fail(f"could not read GitHub issue #{number}: {error}")
        if (
            not isinstance(document, dict)
            or "pull_request" in document
            or document.get("state") not in ("open", "closed")
        ):
            fail(f"GitHub issue #{number} response is not a valid issue state")
        result[number] = document["state"]
    return result


def issue_reference_number(reference: Any, label: str) -> int:
    if (
        not isinstance(reference, str)
        or not reference.startswith("#")
        or not reference[1:].isdigit()
    ):
        fail(f"invalid {label} issue reference: {reference!r}")
    return int(reference[1:])


def declared_issue_numbers(manifest: dict[str, Any]) -> list[int]:
    numbers = list(manifest["required_closed_issues"])
    numbers.extend(
        issue_reference_number(manifest["authority"][key], key)
        for key in ("gate_issue", "tracking_issue")
    )
    return sorted(set(numbers))


def declared_issue_states(
    manifest: dict[str, Any], issue_states: dict[int, str]
) -> dict[int, str]:
    return {
        number: issue_states.get(number, "missing")
        for number in declared_issue_numbers(manifest)
    }


def validate_audit_report(
    root: pathlib.Path,
    audit_report: dict[str, Any],
    git_state: dict[str, Any],
) -> dict[str, dict[str, Any]]:
    validate_schema(
        audit_report,
        load_document(root / AUDIT_REPORT_SCHEMA),
        "foundation audit report",
    )
    if audit_report["revision"] != git_state["revision"]:
        fail("foundation audit report revision differs from completion revision")
    if audit_report["tree"] != git_state["tree"]:
        fail("foundation audit report tree differs from completion tree")
    audits = audit_report["audits"]
    if set(audits) != EXPECTED_ZERO_AUDITS:
        fail("foundation audit report set differs from the accepted gate")
    for identifier, entry in audits.items():
        if entry["revision"] != git_state["revision"]:
            fail(f"foundation audit revision differs: {identifier}")
        if entry["tree"] != git_state["tree"]:
            fail(f"foundation audit tree differs: {identifier}")
        if entry["count"] != len(entry["finding_ids"]):
            fail(f"foundation audit count differs from finding IDs: {identifier}")
    nonzero = {
        identifier: entry["finding_ids"]
        for identifier, entry in audits.items()
        if entry["count"]
    }
    if nonzero:
        fail(f"foundation audit findings are nonzero: {nonzero}")
    return audits


def run_audit_checker(
    root: pathlib.Path,
    manifest: dict[str, Any],
    git_state: dict[str, Any],
    issue_states: dict[int, str],
) -> dict[str, dict[str, Any]]:
    with tempfile.TemporaryDirectory(prefix="cxxlens-foundation-audit-") as temporary:
        issue_path = pathlib.Path(temporary) / "issue-states.json"
        manifest_path = pathlib.Path(temporary) / "manifest.json"
        issue_path.write_text(
            json.dumps({str(number): state for number, state in issue_states.items()}),
            encoding="utf-8",
        )
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        completed = subprocess.run(
            [
                sys.executable,
                str(root / AUDIT_CHECKER),
                "report",
                "--root",
                str(root),
                "--manifest",
                str(manifest_path),
                "--issue-states",
                str(issue_path),
                "--revision",
                git_state["revision"],
                "--tree",
                git_state["tree"],
            ],
            cwd=root,
            check=False,
            capture_output=True,
            text=True,
        )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        fail(f"foundation audit checker failed: {detail}")
    try:
        audit_report = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        fail(f"foundation audit checker emitted invalid JSON: {error}")
    if not isinstance(audit_report, dict):
        fail("foundation audit checker report is not a mapping")
    return validate_audit_report(root, audit_report, git_state)


def build_report(
    root: pathlib.Path,
    manifest: dict[str, Any],
    *,
    git_state: dict[str, Any],
    issue_states: dict[int, str],
    repository: str,
    run_url: str,
    ci_jobs: list[str],
    generated_at: str,
    expected_revision: str | None = None,
    audit_entries: dict[str, dict[str, Any]] | None = None,
    provenance_records: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    if expected_revision and git_state["revision"] != expected_revision:
        fail(
            "checked-out revision differs from workflow revision: "
            f"{git_state['revision']} != {expected_revision}"
        )
    if git_state != {
        "revision": git_state["revision"],
        "tree": git_state["tree"],
        "branch": "main",
        "clean": True,
    }:
        fail(f"final report requires clean main checkout: {git_state}")

    required_jobs = manifest["evidence"]["required_ci_jobs"]
    if sorted(ci_jobs) != sorted(required_jobs):
        fail(f"CI job evidence differs: expected={required_jobs}, actual={ci_jobs}")

    missing_closed = [
        number
        for number in manifest["required_closed_issues"]
        if issue_states.get(number) != "closed"
    ]
    if missing_closed:
        fail(f"required issues are not closed: {missing_closed}")
    gate_number = issue_reference_number(
        manifest["authority"]["gate_issue"], "gate_issue"
    )
    tracking_number = issue_reference_number(
        manifest["authority"]["tracking_issue"], "tracking_issue"
    )
    if (
        issue_states.get(gate_number) != "closed"
        or issue_states.get(tracking_number) != "closed"
    ):
        fail("gate and tracking issues must be closed for a passed completion report")
    scoped_issue_states = declared_issue_states(manifest, issue_states)

    ledger = load_document(root / LEDGER)
    authority_digests = {
        relative: sha256(root / relative)
        for relative in manifest["authority_documents"]
    }
    evidence_claims = {
        claim: {
            "paths": paths,
            "digests": {path: sha256(root / path) for path in paths},
        }
        for claim, paths in manifest["evidence"]["claims"].items()
    }
    audits = (
        validate_audit_report(
            root,
            {
                "schema": "cxxlens.ng-foundation-audit-report.v1",
                "revision": git_state["revision"],
                "tree": git_state["tree"],
                "audits": audit_entries,
            },
            git_state,
        )
        if audit_entries is not None
        else run_audit_checker(root, manifest, git_state, scoped_issue_states)
    )
    if not provenance_records:
        fail("foundation completion requires supply-chain provenance")
    supply_chain = summarize_supply_chain(root, provenance_records, git_state)
    report = {
        "schema": "cxxlens.ng-foundation-completion-report.v1",
        "result": "passed",
        "manifest_digest": sha256(root / MANIFEST),
        "git": git_state,
        "authority_digests": authority_digests,
        "version_contracts": manifest["version_contracts"],
        "evidence_claims": evidence_claims,
        "ci": {
            "repository": repository,
            "run_url": run_url,
            "jobs": {job: "success" for job in sorted(ci_jobs)},
        },
        "supply_chain": supply_chain,
        "issue_states": {
            "required_closed": {
                str(number): "closed"
                for number in sorted(manifest["required_closed_issues"])
            },
            "gate_issue": "closed",
            "tracking_issue": "closed",
        },
        "asset_ledger": {
            "digest": sha256(root / LEDGER),
            "asset_count": ledger["asset_count"],
            "classifications": ledger["classifications"],
        },
        "audits": audits,
        "reproduction": {
            "distribution": "ng-foundation",
            "kernel_semantics": "1.0.0",
            "relation_registry_digest": sha256(
                root / "schemas/cxxlens_ng_relation_registry.yaml"
            ),
            "generated_at": generated_at,
        },
    }
    validate_schema(
        report,
        load_document(root / REPORT_SCHEMA),
        "foundation completion report",
    )
    return report


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--repository")
    parser.add_argument("--run-url")
    parser.add_argument("--expected-revision")
    parser.add_argument("--branch", default="main")
    parser.add_argument("--ci-job", action="append", default=[])
    parser.add_argument("--provenance-dir", type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    try:
        manifest = validate_documents(root)
        if args.command == "check":
            print("NG foundation completion static checks passed")
            return 0
        if not args.output or not args.repository or not args.run_url or not args.provenance_dir:
            fail("report requires --output, --repository, --run-url, and --provenance-dir")
        numbers = declared_issue_numbers(manifest)
        issue_states = github_issue_states(
            args.repository,
            sorted(set(numbers)),
            os.environ.get("GITHUB_TOKEN", ""),
        )
        generated_at = (
            datetime.datetime.now(datetime.timezone.utc)
            .replace(microsecond=0)
            .isoformat()
            .replace("+00:00", "Z")
        )
        git_state = current_git_state(root)
        git_state["branch"] = args.branch
        report = build_report(
            root,
            manifest,
            git_state=git_state,
            issue_states=issue_states,
            repository=args.repository,
            run_url=args.run_url,
            ci_jobs=args.ci_job,
            generated_at=generated_at,
            expected_revision=args.expected_revision,
            provenance_records=load_provenance_directory(args.provenance_dir.resolve()),
        )
        args.output.write_text(
            json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    except (
        CompletionError,
        OSError,
        subprocess.SubprocessError,
        json.JSONDecodeError,
        yaml.YAMLError,
    ) as error:
        print(f"NG foundation completion check failed: {error}", file=sys.stderr)
        return 1
    print("NG foundation completion report passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
