#!/usr/bin/env python3
"""Independently audit foundation and agent artifacts before parallel dispatch."""

from __future__ import annotations

import argparse
import collections
import copy
import datetime
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "agent"))

from api_task_runner import RunnerError, authorize_run  # noqa: E402
from ownership_generator import (  # noqa: E402
    OwnershipError,
    generate_manifest,
    repository_paths,
    transition_request,
    validate_changed_paths,
    validate_manifest,
)
from ready_evaluator import (  # noqa: E402
    ReadyError,
    canonicalize_input,
    digest,
    generate_report as generate_ready_report,
    parse_prompt,
    resolve_api,
    validate_report as validate_ready_report,
)
from task_packet_generator import (  # noqa: E402
    TaskPacketError,
    generate_corpus,
    validate_corpus,
)


SCHEMA_ID = "cxxlens.readiness.authorization.v1"
EVIDENCE_SCHEMA_ID = "cxxlens.readiness.gate-evidence.v1"
GLOBAL_CI_JOBS = [
    "build-test-clang22",
    "contracts-docs",
    "format-lint",
    "gcc-header-compat",
    "install-consumer",
    "public-api",
    "static-analysis",
]
STEWARD_ISSUES = {
    "generator.catalog": 27,
    "steward.ownership": 28,
    "steward.facts": 1,
    "steward.repository": 1,
}


class AuditError(ValueError):
    """A final-readiness invariant violation with a stable code."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code


def fail(code: str, message: str) -> None:
    raise AuditError(code, message)


def pretty_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def file_digest(path: pathlib.Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def load_json(path: pathlib.Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def load_yaml(path: pathlib.Path) -> Any:
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def state_counts(values: list[str]) -> dict[str, int]:
    counts = collections.Counter(values)
    return {state: counts.get(state, 0) for state in ("blocked", "complete", "ready")}


def catalog_entries(catalog: dict[str, Any]) -> list[tuple[dict[str, Any], dict[str, Any]]]:
    return [
        (package, api)
        for package in catalog["packages"]
        for api in package["apis"]
    ]


def validate_upstream_artifacts(inputs: dict[str, Any], root: pathlib.Path) -> None:
    expected_corpus = generate_corpus(inputs["catalog"], root)
    validate_corpus(
        inputs["corpus"],
        inputs["catalog"],
        root,
        inputs["task_schema"],
    )
    if inputs["corpus"] != expected_corpus:
        fail("readiness.task-packet-drift", "task packet regeneration differs")

    paths = repository_paths(root)
    expected_ownership = generate_manifest(inputs["corpus"], paths)
    validate_manifest(
        inputs["ownership"],
        inputs["corpus"],
        paths,
        inputs["ownership_schema"],
    )
    if inputs["ownership"] != expected_ownership:
        fail("readiness.ownership-drift", "ownership regeneration differs")

    expected_ready = generate_ready_report(
        inputs["corpus"],
        inputs["ownership"],
        inputs["m0"],
        inputs["m1"],
        inputs["m2"],
        inputs["requests"],
    )
    validate_ready_report(inputs["ready"], expected_ready, inputs["ready_schema"])
    for milestone in ("m0", "m1", "m2"):
        jsonschema.Draft202012Validator(inputs[f"{milestone}_schema"]).validate(
            inputs[milestone]
        )


def input_fingerprints(inputs: dict[str, Any], root: pathlib.Path) -> dict[str, str]:
    return {
        "catalog": inputs["corpus"]["catalog_fingerprint"],
        "task_packets": inputs["corpus"]["semantic_digest"],
        "ownership": inputs["ownership"]["semantic_digest"],
        "ready_report": inputs["ready"]["semantic_digest"],
        "dependency_requests": digest(canonicalize_input(inputs["requests"])),
        "m0": digest(canonicalize_input(inputs["m0"])),
        "m1": digest(canonicalize_input(inputs["m1"])),
        "m2": digest(canonicalize_input(inputs["m2"])),
        "design_package": file_digest(root / "docs/design/SHA256SUMS"),
        "quality_workflow": file_digest(root / ".github/workflows/quality.yml"),
        "build_gates": digest(
            {
                path: file_digest(root / path)
                for path in (
                    "CMakeLists.txt",
                    "CMakePresets.json",
                    "cmake/CxxlensDeveloperTools.cmake",
                    "tests/CMakeLists.txt",
                )
            }
        ),
        "audit_policy": digest(
            {
                "implementation": file_digest(root / "tools/agent/readiness_audit.py"),
                "runner": file_digest(root / "tools/agent/api_task_runner.py"),
                "unit_local_gate": file_digest(root / "tools/agent/unit_local_gate.py"),
                "ready_evaluator": file_digest(root / "tools/agent/ready_evaluator.py"),
                "schema": file_digest(
                    root / "schemas/cxxlens.readiness.authorization.v1.schema.yaml"
                ),
                "gate_evidence_schema": file_digest(
                    root / "schemas/cxxlens.readiness.gate-evidence.v1.schema.yaml"
                ),
                "run_schema": file_digest(
                    root / "schemas/cxxlens.api-ready.run-manifest.v1.schema.yaml"
                ),
                "execution_result_schema": file_digest(
                    root / "schemas/cxxlens.api-ready.execution-result.v1.schema.yaml"
                ),
                "tests": file_digest(
                    root / "tests/agent/readiness/test_readiness_audit.py"
                ),
                "runner_tests": file_digest(
                    root / "tests/agent/runner/test_runner.py"
                ),
                "negative_fixtures": file_digest(
                    root / "tests/agent/readiness/fixtures/negative_cases.yaml"
                ),
            }
        ),
    }


def foundation_gates() -> list[dict[str, Any]]:
    rows = []
    for milestone, issue in (("M0", 12), ("M1", 22), ("M2", 26)):
        lowered = milestone.lower()
        variants = ("OFF", "ON") if milestone == "M0" else (None,)
        replay_commands = []
        for variant in variants:
            configure = ["cmake", "--preset", f"{lowered}-acceptance"]
            if variant is not None:
                configure.append(f"-DCXXLENS_BUILD_SHARED={variant}")
            replay_commands.extend(
                [
                    {
                        "argv": configure,
                        "environment": {"CXX": "clang++-22"},
                    },
                    {
                        "argv": [
                            "cmake",
                            "--build",
                            "--preset",
                            f"{lowered}-acceptance",
                            "--target",
                            f"cxxlens-{lowered}-acceptance",
                        ],
                        "environment": {},
                    },
                ]
            )
        rows.append(
            {
                "id": milestone,
                "issue": issue,
                "required_ci_jobs": (
                    ["m0-acceptance (OFF)", "m0-acceptance (ON)"]
                    if milestone == "M0"
                    else [f"{lowered}-acceptance"]
                ),
                "required_status": "success",
                "completion_manifest": f"schemas/cxxlens_{lowered}_completion.yaml",
                "replay_commands": replay_commands,
                "evidence": sorted(
                    {
                        f"schemas/cxxlens_{lowered}_completion.yaml",
                        f"schemas/cxxlens_{lowered}_completion.schema.yaml",
                        f"schemas/cxxlens_{lowered}_acceptance_report.schema.yaml",
                        ".github/workflows/quality.yml",
                    }
                ),
            }
        )
    return rows


def global_gate(ready: dict[str, Any]) -> dict[str, Any]:
    return {
        "id": "GLOBAL",
        "required_ci_jobs": GLOBAL_CI_JOBS,
        "required_status": "success",
        "replay_commands": [
            {
                "argv": gate["command"]["argv"],
                "environment": gate["command"]["environment"],
            }
            for gate in ready["global_gates"]
        ],
        "evidence": sorted(
            {
                ".github/workflows/quality.yml",
                "CMakePresets.json",
                "cmake/CxxlensDeveloperTools.cmake",
                "schemas/cxxlens.api-ready.report.v1.json",
            }
        ),
    }


def gate_declarations(inputs: dict[str, Any]) -> list[dict[str, Any]]:
    return [global_gate(inputs["ready"]), *foundation_gates()]


def declaration_digest(gate: dict[str, Any]) -> str:
    return digest(canonicalize_input(gate))


def missing_observation(gate: dict[str, Any]) -> dict[str, Any]:
    return {
        "status": "missing",
        "provider": None,
        "source_sha": None,
        "tree_sha": None,
        "declaration_digest": declaration_digest(gate),
        "executions": [],
    }


def seal_execution(execution: dict[str, Any]) -> dict[str, Any]:
    sealed = copy.deepcopy(execution)
    sealed.pop("artifact_record_digest", None)
    sealed["artifact_record_digest"] = digest(sealed)
    return sealed


def seal_evidence_bundle(bundle: dict[str, Any]) -> dict[str, Any]:
    sealed = copy.deepcopy(bundle)
    sealed.pop("semantic_digest", None)
    sealed["semantic_digest"] = digest(sealed)
    return sealed


def git_output(root: pathlib.Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments],
        cwd=root,
        check=False,
        text=True,
        capture_output=True,
    )
    if completed.returncode != 0:
        fail(
            "readiness.source-identity-unavailable",
            (completed.stderr or completed.stdout).strip(),
        )
    return completed.stdout.strip()


def repository_name(root: pathlib.Path) -> str:
    remote = git_output(root, "config", "--get", "remote.origin.url")
    match = re.search(r"(?:github\.com[:/])([^/]+/[^/]+?)(?:\.git)?$", remote)
    if match is None:
        fail("readiness.repository-identity", remote)
    return match.group(1)


def current_source_identity(root: pathlib.Path, *, require_clean: bool) -> dict[str, Any]:
    dirty_paths = [
        line[3:]
        for line in git_output(
            root, "status", "--porcelain", "--untracked-files=all"
        ).splitlines()
        if line
    ]
    if require_clean and dirty_paths:
        fail("readiness.source-dirty", ", ".join(sorted(dirty_paths)))
    return {
        "binding": "exact-git-object",
        "repository": repository_name(root),
        "commit_sha": git_output(root, "rev-parse", "HEAD"),
        "tree_sha": git_output(root, "rev-parse", "HEAD^{tree}"),
        "dirty": bool(dirty_paths),
    }


def unbound_source_identity(root: pathlib.Path) -> dict[str, Any]:
    return {
        "binding": "unbound",
        "repository": repository_name(root),
        "commit_sha": None,
        "tree_sha": None,
        "dirty": True,
    }


def expected_execution_identities(
    gate: dict[str, Any], provider: str
) -> list[str]:
    if provider == "github-actions":
        return gate["required_ci_jobs"]
    return [
        f"{gate['id']}.replay.{index}"
        for index, _ in enumerate(gate["replay_commands"], start=1)
    ]


def validate_gate_evidence(
    bundle: dict[str, Any],
    inputs: dict[str, Any],
    expected_source: dict[str, Any],
) -> str:
    if not isinstance(bundle, dict) or bundle.get("schema") != EVIDENCE_SCHEMA_ID:
        fail("readiness.gate-evidence-missing", "canonical evidence bundle is absent")
    unsigned = copy.deepcopy(bundle)
    semantic_digest = unsigned.pop("semantic_digest", None)
    if semantic_digest != digest(unsigned):
        fail(
            "readiness.gate-evidence-digest-mismatch",
            "evidence bundle digest mismatch",
        )
    declarations = gate_declarations(inputs)
    expected_ids = [gate["id"] for gate in declarations]
    rows = bundle.get("gates")
    row_ids = (
        [row.get("gate_id") for row in rows if isinstance(row, dict)]
        if isinstance(rows, list)
        else []
    )
    if sorted(row_ids) != sorted(expected_ids) or len(row_ids) != len(set(row_ids)):
        fail("readiness.required-gate-omitted", "required gate coverage differs")
    try:
        schema = inputs["gate_evidence_schema"]
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(bundle)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail("readiness.gate-evidence-schema", error.message)

    source = bundle["source"]
    if expected_source.get("dirty") or source != expected_source:
        fail(
            "readiness.gate-source-mismatch",
            "gate evidence is not bound to the exact clean source commit and tree",
        )
    if (
        bundle["repository"] != source["repository"]
        or bundle["workflow_identity"] != ".github/workflows/quality.yml"
    ):
        fail("readiness.repository-identity", bundle["repository"])
    fingerprints = input_fingerprints(inputs, pathlib.Path(inputs["root"]))
    if bundle["workflow_fingerprint"] != fingerprints["quality_workflow"]:
        fail("readiness.gate-workflow-drift", "quality workflow changed after observation")

    all_success = True
    rows_by_id = {row["gate_id"]: row for row in rows}
    for gate in declarations:
        row = rows_by_id[gate["id"]]
        if row["provider"] != bundle["provider"]:
            fail("readiness.gate-provider-drift", gate["id"])
        if row["declaration_digest"] != declaration_digest(gate):
            fail("readiness.gate-declaration-drift", gate["id"])
        executions = row["executions"]
        identities = [execution["identity"] for execution in executions]
        expected_identities = expected_execution_identities(gate, bundle["provider"])
        if sorted(identities) != sorted(expected_identities) or len(identities) != len(
            set(identities)
        ):
            fail("readiness.required-job-omitted", gate["id"])
        for execution in executions:
            if (
                execution["source_sha"] != source["commit_sha"]
                or execution["tree_sha"] != source["tree_sha"]
            ):
                fail("readiness.gate-source-mismatch", execution["identity"])
            record = copy.deepcopy(execution)
            record_digest = record.pop("artifact_record_digest")
            if record_digest != digest(record):
                fail("readiness.gate-artifact-stale", execution["identity"])
            if bundle["provider"] == "local-replay":
                if (
                    execution["provider_run_id"] is not None
                    or execution["provider_check_id"] is not None
                ):
                    fail("readiness.gate-provider-drift", execution["identity"])
                index = expected_identities.index(execution["identity"])
                command = gate["replay_commands"][index]
                if execution["command_fingerprint"] != digest(command):
                    fail("readiness.gate-command-drift", execution["identity"])
                execution_success = (
                    execution["conclusion"] == "success"
                    and execution["exit_status"] == 0
                )
            else:
                if (
                    execution["provider_run_id"] is None
                    or execution["provider_check_id"] is None
                ):
                    fail("readiness.gate-provider-drift", execution["identity"])
                expected_command = digest(
                    {
                        "job": execution["identity"],
                        "workflow": bundle["workflow_fingerprint"],
                    }
                )
                if execution["command_fingerprint"] != expected_command:
                    fail("readiness.gate-command-drift", execution["identity"])
                execution_success = (
                    execution["conclusion"] == "success"
                    and execution["exit_status"] is None
                )
            all_success = all_success and execution_success
        computed_status = (
            "success"
            if executions
            and all(
                execution["conclusion"] == "success"
                and (
                    execution["exit_status"] == 0
                    if bundle["provider"] == "local-replay"
                    else execution["exit_status"] is None
                )
                for execution in executions
            )
            else "failed"
        )
        if row["observed_status"] != computed_status:
            fail("readiness.gate-status-drift", gate["id"])
    return "success" if all_success else "failed"


def observed_gate(
    gate: dict[str, Any], bundle: dict[str, Any] | None
) -> dict[str, Any]:
    if bundle is None:
        observation = missing_observation(gate)
    else:
        row = next(item for item in bundle["gates"] if item["gate_id"] == gate["id"])
        observation = {
            "status": row["observed_status"],
            "provider": row["provider"],
            "source_sha": bundle["source"]["commit_sha"],
            "tree_sha": bundle["source"]["tree_sha"],
            "declaration_digest": row["declaration_digest"],
            "executions": row["executions"],
        }
    return {**gate, "observed_execution": observation}


def utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")


def executable_version(name: str) -> dict[str, Any]:
    executable = shutil.which(name)
    if executable is None:
        return {"name": name, "path": None, "version_digest": None}
    completed = subprocess.run(
        [executable, "--version"],
        check=False,
        capture_output=True,
    )
    output = completed.stdout + completed.stderr
    return {
        "name": name,
        "path": executable,
        "version_digest": "sha256:" + hashlib.sha256(output).hexdigest(),
    }


def local_toolchain_fingerprint(command: dict[str, Any]) -> str:
    names = {command["argv"][0]}
    if command["environment"].get("CXX"):
        names.add(command["environment"]["CXX"])
    return digest(
        {
            "declared_environment": command["environment"],
            "executables": [executable_version(name) for name in sorted(names)],
            "machine": platform.machine(),
            "platform": platform.system(),
            "python": platform.python_version(),
        }
    )


def evidence_bundle(
    provider: str,
    repository: str,
    source: dict[str, Any],
    workflow_fingerprint: str,
    gate_rows: list[dict[str, Any]],
) -> dict[str, Any]:
    return seal_evidence_bundle(
        {
            "schema": EVIDENCE_SCHEMA_ID,
            "provider": provider,
            "repository": repository,
            "workflow_identity": ".github/workflows/quality.yml",
            "source": source,
            "workflow_fingerprint": workflow_fingerprint,
            "issued_at": utc_now(),
            "gates": gate_rows,
        }
    )


def collect_local_evidence(
    inputs: dict[str, Any], root: pathlib.Path
) -> dict[str, Any]:
    source = current_source_identity(root, require_clean=True)
    workflow_fingerprint = input_fingerprints(inputs, root)["quality_workflow"]
    rows = []
    for gate in gate_declarations(inputs):
        executions = []
        for index, command in enumerate(gate["replay_commands"], start=1):
            started_at = utc_now()
            environment = os.environ.copy()
            environment.update(command["environment"])
            completed = subprocess.run(
                command["argv"],
                cwd=root,
                env=environment,
                check=False,
                capture_output=True,
            )
            completed_at = utc_now()
            output = completed.stdout + b"\n--stderr--\n" + completed.stderr
            executions.append(
                seal_execution(
                    {
                        "identity": f"{gate['id']}.replay.{index}",
                        "provider_run_id": None,
                        "provider_check_id": None,
                        "source_sha": source["commit_sha"],
                        "tree_sha": source["tree_sha"],
                        "conclusion": (
                            "success" if completed.returncode == 0 else "failure"
                        ),
                        "exit_status": completed.returncode,
                        "started_at": started_at,
                        "completed_at": completed_at,
                        "command_fingerprint": digest(command),
                        "environment_fingerprint": digest(command["environment"]),
                        "toolchain_fingerprint": local_toolchain_fingerprint(command),
                        "artifact_log_digest": "sha256:"
                        + hashlib.sha256(output).hexdigest(),
                        "provider_record_digest": digest(
                            {
                                "argv": command["argv"],
                                "environment": command["environment"],
                                "returncode": completed.returncode,
                            }
                        ),
                    }
                )
            )
        rows.append(
            {
                "gate_id": gate["id"],
                "gate_kind": "global" if gate["id"] == "GLOBAL" else "foundation",
                "provider": "local-replay",
                "required_status": "success",
                "observed_status": (
                    "success"
                    if all(execution["conclusion"] == "success" for execution in executions)
                    else "failed"
                ),
                "declaration_digest": declaration_digest(gate),
                "executions": executions,
            }
        )
    return evidence_bundle(
        "local-replay",
        source["repository"],
        source,
        workflow_fingerprint,
        rows,
    )


def workflow_run_id(check: dict[str, Any]) -> str | None:
    details_url = check.get("details_url") or ""
    match = re.search(r"/actions/runs/([0-9]+)(?:/|$)", details_url)
    return match.group(1) if match is not None else None


def workflow_job_id(check: dict[str, Any]) -> int:
    details_url = check.get("details_url") or ""
    match = re.search(r"/job/([0-9]+)(?:/|$)", details_url)
    if match is None:
        fail("readiness.github-job-identity", details_url)
    return int(match.group(1))


def github_api_json(root: pathlib.Path, endpoint: str) -> dict[str, Any]:
    completed = subprocess.run(
        ["gh", "api", "-H", "Accept: application/vnd.github+json", endpoint],
        cwd=root,
        check=False,
        text=True,
        capture_output=True,
    )
    if completed.returncode != 0:
        fail("readiness.github-query-failed", (completed.stderr or completed.stdout).strip())
    try:
        value = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        fail("readiness.github-response-invalid", str(error))
    if not isinstance(value, dict):
        fail("readiness.github-response-invalid", endpoint)
    return value


def github_job_log(root: pathlib.Path, repository: str, check_id: int) -> bytes:
    completed = subprocess.run(
        ["gh", "api", f"repos/{repository}/actions/jobs/{check_id}/logs"],
        cwd=root,
        check=False,
        capture_output=True,
    )
    if completed.returncode != 0:
        fail("readiness.github-log-missing", str(check_id))
    return completed.stdout


def collect_online_evidence(
    inputs: dict[str, Any],
    root: pathlib.Path,
    repository: str,
    selected_run_id: str | None = None,
) -> dict[str, Any]:
    source = current_source_identity(root, require_clean=True)
    if source["repository"] != repository:
        fail("readiness.repository-identity", repository)
    workflow_fingerprint = input_fingerprints(inputs, root)["quality_workflow"]
    response = github_api_json(
        root,
        f"repos/{repository}/commits/{source['commit_sha']}/check-runs?per_page=100",
    )
    checks = [
        check
        for check in response.get("check_runs", [])
        if check.get("head_sha") == source["commit_sha"]
        and (check.get("app") or {}).get("slug") == "github-actions"
    ]
    required_names = sorted(
        {
            job
            for gate in gate_declarations(inputs)
            for job in gate["required_ci_jobs"]
        }
    )
    by_run: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
    for check in checks:
        run_id = workflow_run_id(check)
        if run_id is not None:
            by_run[run_id].append(check)
    if selected_run_id is None:
        candidates = [
            run_id
            for run_id, run_checks in by_run.items()
            if sorted({check["name"] for check in run_checks} & set(required_names))
            == required_names
        ]
        if len(candidates) != 1:
            fail("readiness.github-run-ambiguous", f"candidates={sorted(candidates)}")
        selected_run_id = candidates[0]
    selected = by_run.get(selected_run_id, [])
    selected_by_name: dict[str, dict[str, Any]] = {}
    for name in required_names:
        matches = [check for check in selected if check.get("name") == name]
        if len(matches) != 1:
            fail("readiness.required-job-omitted", name)
        selected_by_name[name] = matches[0]

    rows = []
    for gate in gate_declarations(inputs):
        executions = []
        for name in gate["required_ci_jobs"]:
            check = selected_by_name[name]
            job_id = workflow_job_id(check)
            log = github_job_log(root, repository, job_id)
            conclusion = check.get("conclusion") or "in_progress"
            executions.append(
                seal_execution(
                    {
                        "identity": name,
                        "provider_run_id": selected_run_id,
                        "provider_check_id": str(check["id"]),
                        "source_sha": source["commit_sha"],
                        "tree_sha": source["tree_sha"],
                        "conclusion": conclusion,
                        "exit_status": None,
                        "started_at": check.get("started_at") or check.get("created_at"),
                        "completed_at": check.get("completed_at") or check.get("updated_at"),
                        "command_fingerprint": digest(
                            {"job": name, "workflow": workflow_fingerprint}
                        ),
                        "environment_fingerprint": workflow_fingerprint,
                        "toolchain_fingerprint": workflow_fingerprint,
                        "artifact_log_digest": "sha256:"
                        + hashlib.sha256(log).hexdigest(),
                        "provider_record_digest": digest(
                            {
                                "app": (check.get("app") or {}).get("slug"),
                                "check_id": check["id"],
                                "conclusion": conclusion,
                                "details_url": check.get("details_url"),
                                "head_sha": check.get("head_sha"),
                                "name": name,
                                "run_id": selected_run_id,
                                "workflow_job_id": job_id,
                            }
                        ),
                    }
                )
            )
        rows.append(
            {
                "gate_id": gate["id"],
                "gate_kind": "global" if gate["id"] == "GLOBAL" else "foundation",
                "provider": "github-actions",
                "required_status": "success",
                "observed_status": (
                    "success"
                    if all(execution["conclusion"] == "success" for execution in executions)
                    else "failed"
                ),
                "declaration_digest": declaration_digest(gate),
                "executions": executions,
            }
        )
    return evidence_bundle(
        "github-actions", repository, source, workflow_fingerprint, rows
    )


def audit_blockers(node: dict[str, Any]) -> list[dict[str, Any]]:
    rows = []
    for blocker in node["blockers"]:
        resolution_issue = (
            28
            if blocker["code"] == "dependency-request-open"
            else STEWARD_ISSUES.get(blocker["steward"], 1)
        )
        rows.append({**blocker, "resolution_issue": resolution_issue})
    return rows


def make_dry_runs(inputs: dict[str, Any]) -> dict[str, Any]:
    packets = sorted(inputs["corpus"]["packets"], key=lambda item: item["api_id"])
    prompts = []
    for kind in ("free_function", "method", "method_family", "builder_family", "static_factory"):
        packet = next(item for item in packets if item["kind"] == kind)
        if parse_prompt(f"{packet['api_id']} を実装してください") != packet["api_id"]:
            fail("readiness.prompt-drift", packet["api_id"])
        resolved = resolve_api(
            packet["api_id"], inputs["ready"], inputs["corpus"], inputs["ownership"]
        )
        prompts.append(
            {
                "api_id": packet["api_id"],
                "kind": kind,
                "phase": packet["phase"],
                "atomic_unit_id": resolved["atomic_unit_id"],
                "state": resolved["state"],
                "start_authorized": resolved["start_authorized"],
                "packet_digest": resolved["task_packet_digest"],
                "shard_digest": resolved["shard_digest"],
                "acceptance_argv": resolved["acceptance_command"]["argv"],
            }
        )

    unknown_code = ""
    try:
        resolve_api("API-NOT-999", inputs["ready"], inputs["corpus"], inputs["ownership"])
    except ReadyError as error:
        unknown_code = error.code
    if unknown_code != "ready.unknown-api":
        fail("readiness.unknown-prompt-started", unknown_code)

    blocked_node = next(node for node in inputs["ready"]["nodes"] if node["state"] == "blocked")
    blocked_resolution = resolve_api(
        blocked_node["api_ids"][0],
        inputs["ready"],
        inputs["corpus"],
        inputs["ownership"],
    )
    blocked_code = ""
    try:
        authorize_run(blocked_resolution)
    except RunnerError as error:
        blocked_code = error.code
    if blocked_code != "runner.not-ready":
        fail("readiness.blocked-prompt-started", blocked_node["api_ids"][0])

    unit_id = blocked_node["atomic_unit_id"]
    unauthorized_path = next(
        item["path"]
        for item in inputs["ownership"]["tracked_paths"]
        if item["owner_role"]
        != next(
            unit["unit_owner_role"]
            for unit in inputs["ownership"]["units"]
            if unit["atomic_unit_id"] == unit_id
        )
    )
    unauthorized_code = ""
    try:
        validate_changed_paths(inputs["ownership"], unit_id, [unauthorized_path])
    except OwnershipError as error:
        unauthorized_code = error.code
    if unauthorized_code not in {
        "ownership.generated-direct-edit",
        "ownership.unauthorized-path",
    }:
        fail("readiness.unauthorized-edit-accepted", unauthorized_path)

    pending = next(
        request for request in inputs["requests"]["requests"] if request["state"] == "pending"
    )
    accepted = transition_request(pending, "accepted", inputs["ownership"])
    resolved = transition_request(
        accepted,
        "resolved",
        inputs["ownership"],
        ["shared contract published and task packet reissued"],
    )
    resolved_requests = copy.deepcopy(inputs["requests"])
    resolved_requests["requests"] = [
        resolved if request["request_id"] == pending["request_id"] else request
        for request in resolved_requests["requests"]
    ]
    reissued = generate_ready_report(
        inputs["corpus"],
        inputs["ownership"],
        inputs["m0"],
        inputs["m1"],
        inputs["m2"],
        resolved_requests,
    )
    original_node = next(
        node
        for node in inputs["ready"]["nodes"]
        if node["atomic_unit_id"] == pending["requesting_atomic_unit"]
    )
    reissued_node = next(
        node
        for node in reissued["nodes"]
        if node["atomic_unit_id"] == pending["requesting_atomic_unit"]
    )
    open_observed = any(
        blocker["code"] == "dependency-request-open" for blocker in original_node["blockers"]
    )
    removed = not any(
        blocker["code"] == "dependency-request-open" for blocker in reissued_node["blockers"]
    )
    if not open_observed or not removed or reissued["semantic_digest"] == inputs["ready"]["semantic_digest"]:
        fail("readiness.dependency-request-reissue", pending["request_id"])

    return {
        "prompts": prompts,
        "unknown_prompt": {"code": unknown_code, "before_compilation": True},
        "blocked_prompt": {"code": blocked_code, "before_compilation": True},
        "unauthorized_edit": {
            "code": unauthorized_code,
            "before_compilation": True,
        },
        "dependency_request": {
            "request_id": pending["request_id"],
            "atomic_unit_id": pending["requesting_atomic_unit"],
            "transition_order": ["pending", "accepted", "resolved"],
            "open_blocker_observed": open_observed,
            "resolved_blocker_removed": removed,
            "report_reissued": reissued["semantic_digest"]
            != inputs["ready"]["semantic_digest"],
        },
    }


def generate_authorization(
    inputs: dict[str, Any],
    root: pathlib.Path,
    gate_evidence: dict[str, Any] | None = None,
    expected_source: dict[str, Any] | None = None,
) -> dict[str, Any]:
    validate_upstream_artifacts(inputs, root)
    fingerprints = input_fingerprints(inputs, root)
    declarations = gate_declarations(inputs)
    if gate_evidence is None:
        evidence_status = "missing"
        source_identity = unbound_source_identity(root)
    else:
        source_identity = expected_source or current_source_identity(
            root, require_clean=True
        )
        evidence_status = validate_gate_evidence(
            gate_evidence, inputs, source_identity
        )
    gates_verified = evidence_status == "success"
    packets_by_api = {
        packet["api_id"]: packet for packet in inputs["corpus"]["packets"]
    }
    nodes_by_id = {
        node["atomic_unit_id"]: node for node in inputs["ready"]["nodes"]
    }
    shards_by_id = {shard["id"]: shard for shard in inputs["ready"]["shards"]}
    integration_by_package = {
        shard["package_id"]: shard
        for shard in inputs["ready"]["package_integration_shards"]
    }
    wave_by_unit = {
        unit_id: index
        for index, wave in enumerate(inputs["ready"]["topological_waves"])
        for unit_id in wave
    }
    units = []
    for unit_id, node in sorted(nodes_by_id.items()):
        packets = sorted(
            (
                packet
                for packet in packets_by_api.values()
                if packet["atomic_unit_id"] == unit_id
            ),
            key=lambda item: item["api_id"],
        )
        package_ids = {packet["package"]["id"] for packet in packets}
        if len(package_ids) != 1:
            fail("readiness.family-split", f"{unit_id}: {sorted(package_ids)}")
        package_id = next(iter(package_ids))
        shard = shards_by_id[node["shard_id"]]
        units.append(
            {
                "atomic_unit_id": unit_id,
                "package_id": package_id,
                "api_ids": node["api_ids"],
                "state": node["state"],
                "topological_wave": wave_by_unit[unit_id],
                "dispatch_authorized": node["state"] == "ready" and gates_verified,
                "shard_id": shard["id"],
                "shard_digest": shard["semantic_digest"],
                "package_integration_shard_id": integration_by_package[package_id]["id"],
                "blockers": audit_blockers(node),
            }
        )

    unit_audit = {unit["atomic_unit_id"]: unit for unit in units}
    apis = []
    for package, api in sorted(catalog_entries(inputs["catalog"]), key=lambda item: item[1]["id"]):
        packet = packets_by_api[api["id"]]
        unit = unit_audit[packet["atomic_unit_id"]]
        apis.append(
            {
                "api_id": api["id"],
                "atomic_unit_id": unit["atomic_unit_id"],
                "package_id": package["id"],
                "kind": api["kind"],
                "phase": api["phase"],
                "declaration_status": api["declaration"]["status"],
                "implementation_state": api["implementation_state"],
                "state": unit["state"],
                "dispatch_authorized": unit["dispatch_authorized"],
                "task_packet_digest": packet["semantic_digest"],
                "blockers": unit["blockers"],
            }
        )

    api_states = state_counts([api["state"] for api in apis])
    unit_states = state_counts([unit["state"] for unit in units])
    candidate_ready_waves = inputs["ready"]["summary"]["ready_waves"]
    candidate_ready_units = sorted(
        unit["atomic_unit_id"] for unit in units if unit["state"] == "ready"
    )
    authorized_units = sorted(
        unit["atomic_unit_id"] for unit in units if unit["dispatch_authorized"]
    )
    if authorized_units:
        reason_code = "ready-wave-available"
    elif candidate_ready_units and evidence_status == "missing":
        reason_code = "gate-evidence-missing"
    elif candidate_ready_units:
        reason_code = "gate-evidence-failed"
    else:
        reason_code = "no-ready-incomplete-unit"
    entries = catalog_entries(inputs["catalog"])
    report: dict[str, Any] = {
        "schema": SCHEMA_ID,
        "policy": {
            "waivers_allowed": False,
            "dispatch_scope": "manifest_ready_units_only",
            "rollback_triggers": [
                "catalog-or-signature-drift",
                "completion-or-foundation-gate-failure",
                "dependency-or-provider-drift",
                "exact-source-or-observed-gate-drift",
                "ownership-or-shared-contract-drift",
                "schema-task-packet-or-shard-drift",
            ],
        },
        "input_fingerprints": fingerprints,
        "source_identity": source_identity,
        "gate_evidence": {
            "status": evidence_status,
            "provider": gate_evidence["provider"] if gate_evidence is not None else None,
            "bundle_digest": (
                gate_evidence["semantic_digest"] if gate_evidence is not None else None
            ),
        },
        "global_gate": observed_gate(declarations[0], gate_evidence),
        "foundation_gates": [
            observed_gate(gate, gate_evidence) for gate in declarations[1:]
        ],
        "catalog_audit": {
            "package_count": len(inputs["catalog"]["packages"]),
            "api_count": len(entries),
            "exact_declaration_count": sum(
                api["declaration"]["status"] == "exact" for _, api in entries
            ),
            "unresolved_declaration_count": sum(
                api["declaration"]["status"] == "unresolved" for _, api in entries
            ),
            "duplicate_api_count": 0,
            "dangling_api_dependency_count": 0,
            "unresolved_ready_signature_count": sum(
                api["readiness"]["state"] == "ready"
                and api["declaration"]["status"] != "exact"
                for _, api in entries
            ),
            "evidence_free_conformant_count": sum(
                api["implementation_state"] == "conformant"
                and not api["implementation_evidence"]
                for _, api in entries
            ),
        },
        "infrastructure_audit": {
            "atomic_unit_count": len(inputs["corpus"]["atomic_units"]),
            "packet_count": len(inputs["corpus"]["packets"]),
            "tracked_path_count": inputs["ownership"]["summary"]["tracked_path_count"],
            "reserved_path_count": inputs["ownership"]["summary"]["reserved_path_count"],
            "dependency_edge_count": len(inputs["ready"]["edges"]),
            "package_integration_shard_count": len(
                inputs["ready"]["package_integration_shards"]
            ),
            "write_overlap_count": 0,
            "unowned_path_count": 0,
            "cycle_count": 0,
            "dangling_edge_count": 0,
            "provider_ambiguity_count": 0,
        },
        "dry_runs": make_dry_runs(inputs),
        "units": units,
        "apis": apis,
        "authorization": {
            "decision": "authorized" if authorized_units else "denied",
            "reason_code": reason_code,
            "candidate_ready_unit_ids": candidate_ready_units,
            "ready_unit_ids": authorized_units,
            "ready_waves": candidate_ready_waves if gates_verified else [],
            "validity": {
                "state": "current" if gates_verified else "unverified",
                "basis": "exact-source-and-observed-gates",
                "expires_on": "source-workflow-gate-or-input-drift",
                "fingerprint_digest": digest(
                    {
                        "gate_evidence": (
                            gate_evidence["semantic_digest"]
                            if gate_evidence is not None
                            else None
                        ),
                        "inputs": fingerprints,
                        "source": source_identity,
                    }
                ),
            },
        },
        "summary": {
            "package_count": len(inputs["catalog"]["packages"]),
            "api_count": len(apis),
            "unit_count": len(units),
            "api_state_counts": api_states,
            "unit_state_counts": unit_states,
        },
        "checks": [
            "all-api-exactly-once",
            "catalog-chapter40-and-schema",
            "dependency-request-block-resolve-reissue",
            "foundation-completion-and-ci-linkage",
            "gate-evidence-exact-source-and-result",
            "input-drift-auto-expiry",
            "ownership-and-package-integration",
            "prompt-and-shard-dry-run",
            "provider-dag-and-ready-predicate",
            "public-install-doxygen-flagship-evidence",
            "root-order-process-backend-cache-determinism",
            "task-packet-family-atomicity",
        ],
    }
    report["semantic_digest"] = digest(report)
    return report


def validate_authorization(
    report: dict[str, Any],
    expected: dict[str, Any],
    schema: dict[str, Any] | None = None,
) -> None:
    if not isinstance(report, dict) or report.get("schema") != SCHEMA_ID:
        fail("readiness.unknown-schema", "authorization schema is unsupported")
    if report.get("input_fingerprints") != expected["input_fingerprints"]:
        fail("readiness.input-drift", "authorization evidence expired on input drift")
    evidence_status = report.get("gate_evidence", {}).get("status")
    gates_verified = evidence_status == "success"
    if gates_verified:
        source = report.get("source_identity", {})
        if (
            source.get("binding") != "exact-git-object"
            or source.get("dirty")
            or not source.get("commit_sha")
            or not source.get("tree_sha")
        ):
            fail("readiness.gate-source-mismatch", "authorization source is unbound")
        observations = [
            report.get("global_gate", {}).get("observed_execution", {}),
            *[
                gate.get("observed_execution", {})
                for gate in report.get("foundation_gates", [])
            ],
        ]
        if not observations or any(
            observation.get("status") != "success" for observation in observations
        ):
            fail("readiness.gate-not-successful", "required gate is not successful")
    units = report.get("units", [])
    unit_ids = [unit["atomic_unit_id"] for unit in units]
    expected_unit_ids = [unit["atomic_unit_id"] for unit in expected["units"]]
    if sorted(unit_ids) != sorted(expected_unit_ids) or len(unit_ids) != len(set(unit_ids)):
        fail("readiness.unit-coverage", "atomic units are not covered exactly once")
    apis = report.get("apis", [])
    api_ids = [api["api_id"] for api in apis]
    expected_api_ids = [api["api_id"] for api in expected["apis"]]
    if sorted(api_ids) != sorted(expected_api_ids) or len(api_ids) != len(set(api_ids)):
        fail("readiness.api-coverage", "catalog APIs are not covered exactly once")
    units_by_id = {unit["atomic_unit_id"]: unit for unit in units}
    for unit in units:
        if unit["dispatch_authorized"] != (
            unit["state"] == "ready" and gates_verified
        ):
            fail("readiness.false-authorization", unit["atomic_unit_id"])
        if unit["state"] == "blocked" and not unit["blockers"]:
            fail("readiness.blocker-missing", unit["atomic_unit_id"])
    for api in apis:
        unit = units_by_id.get(api["atomic_unit_id"])
        if unit is None or api["api_id"] not in unit["api_ids"]:
            fail("readiness.api-unit-drift", api["api_id"])
        if (
            api["state"] != unit["state"]
            or api["dispatch_authorized"] != unit["dispatch_authorized"]
            or api["blockers"] != unit["blockers"]
        ):
            fail("readiness.family-split", api["api_id"])
    ready_units = sorted(
        unit["atomic_unit_id"] for unit in units if unit["dispatch_authorized"]
    )
    authorization = report.get("authorization", {})
    decision = authorization.get("decision")
    if (decision == "authorized") != bool(ready_units):
        fail("readiness.false-authorization", f"decision={decision}")
    if authorization.get("ready_unit_ids") != ready_units:
        fail("readiness.ready-set-drift", "authorization ready set differs from units")
    candidate_ready_units = sorted(
        unit["atomic_unit_id"] for unit in units if unit["state"] == "ready"
    )
    if authorization.get("candidate_ready_unit_ids") != candidate_ready_units:
        fail("readiness.ready-set-drift", "candidate ready set differs from units")
    if decision == "authorized" and not gates_verified:
        fail("readiness.gate-not-successful", "dispatch requires observed green gates")
    unsigned = copy.deepcopy(report)
    semantic_digest = unsigned.pop("semantic_digest", None)
    if semantic_digest != digest(unsigned):
        fail("readiness.digest-mismatch", "authorization digest mismatch")
    if schema is not None:
        try:
            jsonschema.Draft202012Validator.check_schema(schema)
            jsonschema.Draft202012Validator(schema).validate(report)
        except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
            fail("readiness.schema-invalid", error.message)
    if report != expected:
        fail("readiness.stale-authorization", "authorization manifest is stale")


def load_inputs(root: pathlib.Path) -> dict[str, Any]:
    schemas = root / "schemas"
    return {
        "root": root,
        "catalog": load_yaml(schemas / "cxxlens_public_api_contract.yaml"),
        "corpus": load_json(schemas / "cxxlens.agent-task-packet-corpus.v1.json"),
        "task_schema": load_yaml(schemas / "cxxlens.agent-task-packet.v1.schema.yaml"),
        "ownership": load_json(schemas / "cxxlens.agent-ownership.v1.json"),
        "ownership_schema": load_yaml(
            schemas / "cxxlens.agent-ownership.v1.schema.yaml"
        ),
        "requests": load_json(schemas / "cxxlens.dependency-request.examples.v1.json"),
        "ready": load_json(schemas / "cxxlens.api-ready.report.v1.json"),
        "ready_schema": load_yaml(schemas / "cxxlens.api-ready.v1.schema.yaml"),
        "m0": load_yaml(schemas / "cxxlens_m0_completion.yaml"),
        "m0_schema": load_yaml(schemas / "cxxlens_m0_completion.schema.yaml"),
        "m1": load_yaml(schemas / "cxxlens_m1_completion.yaml"),
        "m1_schema": load_yaml(schemas / "cxxlens_m1_completion.schema.yaml"),
        "m2": load_yaml(schemas / "cxxlens_m2_completion.yaml"),
        "m2_schema": load_yaml(schemas / "cxxlens_m2_completion.schema.yaml"),
        "authorization_schema": load_yaml(
            schemas / "cxxlens.readiness.authorization.v1.schema.yaml"
        ),
        "gate_evidence_schema": load_yaml(
            schemas / "cxxlens.readiness.gate-evidence.v1.schema.yaml"
        ),
    }


def replay_static_checks(root: pathlib.Path) -> None:
    commands = [
        [sys.executable, f"tools/quality/check_{milestone}_completion.py", str(root)]
        for milestone in ("m0", "m1", "m2")
    ] + [
        [sys.executable, "tools/agent/task_packet_generator.py", "check", "--root", "."],
        [sys.executable, "tools/agent/ownership_generator.py", "check", "--root", "."],
        [sys.executable, "tools/agent/ready_evaluator.py", "check", "--root", "."],
    ]
    for command in commands:
        completed = subprocess.run(
            command,
            cwd=root,
            check=False,
            text=True,
            capture_output=True,
        )
        if completed.returncode != 0:
            detail = (completed.stderr or completed.stdout).strip()
            fail("readiness.replay-failed", f"argv={command} output={detail}")


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "mode", choices=("generate", "check", "replay", "online", "verify")
    )
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--evidence", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--repository")
    parser.add_argument("--workflow-run-id")
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    inputs = load_inputs(root)
    canonical_path = root / "schemas/cxxlens.readiness.authorization.v1.json"
    if args.mode == "generate":
        generated = generate_authorization(inputs, root)
        canonical_path.write_text(pretty_json(generated), encoding="utf-8")
    elif args.mode == "check":
        generated = generate_authorization(inputs, root)
        replay_static_checks(root)
        report = load_json(canonical_path)
        validate_authorization(report, generated, inputs["authorization_schema"])
    elif args.mode in {"replay", "online"}:
        if args.mode == "replay":
            bundle = collect_local_evidence(inputs, root)
        else:
            repository = args.repository or repository_name(root)
            bundle = collect_online_evidence(
                inputs, root, repository, args.workflow_run_id
            )
        evidence_path = args.evidence or (
            root / f"build/readiness/gate-evidence.{args.mode}.json"
        )
        output_path = args.output or (
            root / f"build/readiness/authorization.{args.mode}.json"
        )
        evidence_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        evidence_path.write_text(pretty_json(bundle), encoding="utf-8")
        source = current_source_identity(root, require_clean=True)
        generated = generate_authorization(inputs, root, bundle, source)
        output_path.write_text(pretty_json(generated), encoding="utf-8")
        validate_authorization(generated, generated, inputs["authorization_schema"])
        if generated["gate_evidence"]["status"] != "success":
            fail("readiness.gate-not-successful", bundle["semantic_digest"])
    else:
        if args.evidence is None or args.output is None:
            fail(
                "readiness.gate-evidence-missing",
                "verify requires --evidence and --output authorization paths",
            )
        bundle = load_json(args.evidence)
        source = current_source_identity(root, require_clean=True)
        generated = generate_authorization(inputs, root, bundle, source)
        validate_authorization(
            load_json(args.output), generated, inputs["authorization_schema"]
        )
    summary = generated["summary"]
    print(
        f"readiness audit {args.mode} passed: {summary['package_count']} packages, "
        f"{summary['api_count']} APIs, states {summary['unit_state_counts']}, "
        f"decision {generated['authorization']['decision']}, "
        f"digest {generated['semantic_digest']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        AuditError,
        json.JSONDecodeError,
        jsonschema.ValidationError,
        OSError,
        OwnershipError,
        ReadyError,
        RunnerError,
        subprocess.CalledProcessError,
        TaskPacketError,
        yaml.YAMLError,
    ) as error:
        print(f"readiness audit failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
