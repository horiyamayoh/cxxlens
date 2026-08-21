#!/usr/bin/env python3
"""Validate and extract a connected #167/#179 owner handoff.

The owner workflows use this checker after downloading one explicitly selected
artifact from the legacy quality producer.  It is deliberately offline: the
workflow fetches GitHub metadata and bytes, while this module verifies the
captured objects and emits the owner report plus an immutable handoff sidecar.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import stat
import sys
import zipfile
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
HANDOFF_SCHEMA = pathlib.Path("schemas/cxxlens_ng_release_owner_handoff.schema.yaml")
GR_SCHEMA = pathlib.Path("schemas/cxxlens_ng_release_qualification_report.schema.yaml")
TERMINAL_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_production_scope_closure_report.schema.yaml"
)
MAX_BYTES = 1 << 30


class OwnerHandoffError(ValueError):
    """A source artifact or owner report failed closed validation."""


def fail(message: str) -> None:
    raise OwnerHandoffError(message)


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            fail(f"duplicate JSON object member: {key}")
        result[key] = value
    return result


def load_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot load JSON {path}: {error}")
    if not isinstance(value, dict):
        fail(f"expected object JSON: {path}")
    return value


def load_schema(root: pathlib.Path, path: pathlib.Path) -> dict[str, Any]:
    try:
        value = yaml.safe_load((root / path).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, yaml.YAMLError) as error:
        fail(f"cannot load schema {path}: {error}")
    if not isinstance(value, dict):
        fail(f"schema is not an object: {path}")
    try:
        jsonschema.Draft202012Validator.check_schema(value)
    except jsonschema.SchemaError as error:
        fail(f"invalid schema {path}: {error.message}")
    return value


def validate_schema(
    value: dict[str, Any], schema: dict[str, Any], label: str
) -> None:
    try:
        jsonschema.Draft202012Validator(
            schema,
            format_checker=jsonschema.Draft202012Validator.FORMAT_CHECKER,
        ).validate(value)
    except jsonschema.ValidationError as error:
        location = ".".join(str(part) for part in error.absolute_path)
        suffix = f" at {location}" if location else ""
        fail(f"{label} schema validation failed{suffix}: {error.message}")


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_file(path: pathlib.Path) -> tuple[str, int]:
    try:
        raw = path.read_bytes()
    except OSError as error:
        fail(f"cannot read archive {path}: {error}")
    if len(raw) > MAX_BYTES:
        fail(f"archive exceeds {MAX_BYTES}-byte bound: {path}")
    return digest_bytes(raw), len(raw)


def _required(value: dict[str, Any], key: str, label: str) -> Any:
    if key not in value:
        fail(f"{label} is missing {key}")
    return value[key]


def _same(left: Any, right: Any, label: str) -> None:
    if left != right:
        fail(f"{label} differs: expected={right!r}, observed={left!r}")


def _source_workflow_identity(workflow: dict[str, Any]) -> int:
    workflow_id = _required(workflow, "id", "source workflow")
    if not isinstance(workflow_id, int) or workflow_id <= 0:
        fail("source workflow ID is invalid")
    _same(workflow.get("path"), ".github/workflows/quality.yml", "source workflow path")
    _same(workflow.get("state"), "active", "source workflow state")
    return workflow_id


def _source_run_identity(
    run: dict[str, Any], source_workflow_id: int
) -> tuple[int, int, str, int]:
    run_id = _required(run, "id", "source run")
    workflow_id = _required(run, "workflow_id", "source run")
    path = _required(run, "path", "source run")
    head_sha = _required(run, "head_sha", "source run")
    if not all(isinstance(value, int) and value > 0 for value in (run_id, workflow_id)):
        fail("source run IDs are invalid")
    _same(workflow_id, source_workflow_id, "source run workflow identity")
    if path != ".github/workflows/quality.yml":
        fail(f"source run is not the canonical quality producer: {path}")
    if not isinstance(head_sha, str):
        fail("source run head SHA is invalid")
    _same(run.get("head_branch"), "main", "source run branch")
    _same(run.get("event"), "push", "source run event")
    _same(run.get("status"), "completed", "source run status")
    _same(run.get("conclusion"), "success", "source run conclusion")
    _same(run.get("run_attempt"), 1, "source run attempt")
    repository = run.get("repository")
    if not isinstance(repository, dict) or not isinstance(repository.get("id"), int):
        fail("source run repository identity is missing")
    head_repository = run.get("head_repository")
    if not isinstance(head_repository, dict) or not isinstance(head_repository.get("id"), int):
        fail("source run head repository identity is missing")
    _same(
        head_repository["id"],
        repository["id"],
        "source run head repository identity",
    )
    return run_id, workflow_id, head_sha, repository["id"]


def _artifact_metadata(
    artifact: dict[str, Any], run_id: int, expected_name: str
) -> tuple[int, str, str, int, str]:
    artifact_id = _required(artifact, "id", "source artifact")
    if not isinstance(artifact_id, int) or artifact_id <= 0:
        fail("source artifact ID is invalid")
    _same(artifact.get("name"), expected_name, "source artifact name")
    _same(artifact.get("expired"), False, "source artifact expiry")
    workflow_run = artifact.get("workflow_run")
    if not isinstance(workflow_run, dict):
        fail("source artifact workflow binding is missing")
    _same(workflow_run.get("id"), run_id, "source artifact producer run")
    size = artifact.get("size_in_bytes")
    digest = artifact.get("digest")
    url = artifact.get("archive_download_url")
    if not isinstance(size, int) or size <= 0 or size > MAX_BYTES:
        fail("source artifact size is invalid")
    if not isinstance(digest, str) or not digest.startswith("sha256:"):
        fail("source artifact digest is unavailable")
    if not isinstance(url, str) or not url.endswith(f"/actions/artifacts/{artifact_id}/zip"):
        fail("source artifact download URL is not bound to its ID")
    return artifact_id, str(artifact["name"]), digest, size, url


def _safe_member(name: str) -> None:
    path = pathlib.PurePosixPath(name)
    if (
        not name
        or path.is_absolute()
        or "\\" in name
        or "\x00" in name
        or ".." in path.parts
        or name.endswith("/")
    ):
        fail(f"source artifact contains unsafe member: {name!r}")


def _read_archive(
    archive_path: pathlib.Path, report_path: str
) -> tuple[bytes, dict[str, Any]]:
    try:
        with zipfile.ZipFile(archive_path) as archive:
            infos = archive.infolist()
            if len(infos) != 1 or infos[0].filename != report_path:
                fail(
                    "source artifact must contain exactly one owner report: "
                    f"expected={report_path!r}, observed={[info.filename for info in infos]!r}"
                )
            info = infos[0]
            _safe_member(info.filename)
            mode = (info.external_attr >> 16) & 0o170000
            if mode not in {0, stat.S_IFREG}:
                fail("source report archive member is not regular")
            content = archive.read(info)
    except OwnerHandoffError:
        raise
    except (OSError, zipfile.BadZipFile, RuntimeError) as error:
        fail(f"cannot inspect source artifact archive: {error}")
    if not content or len(content) > MAX_BYTES:
        fail("source report bytes are outside the bounded range")
    try:
        value = json.loads(content.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys)
    except (UnicodeError, json.JSONDecodeError) as error:
        fail(f"source report is not strict JSON: {error}")
    if not isinstance(value, dict):
        fail("source report is not an object")
    return content, value


def _report_spec(role: str) -> tuple[str, pathlib.Path, str, str]:
    if role == "gr":
        return (
            "cxxlens-ng-release-qualification-report.json",
            GR_SCHEMA,
            "cxxlens-ng-release-qualification-",
            "qualified",
        )
    if role == "terminal_scope":
        return (
            "cxxlens-ng-production-scope-closure-report.json",
            TERMINAL_SCHEMA,
            "cxxlens-ng-production-scope-closure-",
            "classified-with-gaps",
        )
    fail(f"unknown owner role: {role}")


def check_and_extract(
    *,
    root: pathlib.Path,
    role: str,
    candidate_sha: str,
    candidate_tree: str,
    selection_digest: str,
    source_run_path: pathlib.Path,
    source_workflow_path: pathlib.Path,
    source_artifact_path: pathlib.Path,
    archive_path: pathlib.Path,
    report_output: pathlib.Path,
    handoff_output: pathlib.Path,
) -> dict[str, Any]:
    report_path, report_schema_path, artifact_prefix, terminal_outcome = _report_spec(role)
    if len(candidate_sha) != 40 or any(character not in "0123456789abcdef" for character in candidate_sha):
        fail("candidate SHA is not full lowercase hex")
    if len(candidate_tree) != 40 or any(character not in "0123456789abcdef" for character in candidate_tree):
        fail("candidate tree is not full lowercase hex")
    if not selection_digest.startswith("sha256:") or len(selection_digest) != 71:
        fail("input selection digest is invalid")

    source_workflow = load_json(source_workflow_path)
    source_workflow_id = _source_workflow_identity(source_workflow)
    run = load_json(source_run_path)
    run_id, workflow_id, source_sha, repository_id = _source_run_identity(
        run, source_workflow_id
    )
    _same(source_sha, candidate_sha, "source run candidate SHA")

    artifact = load_json(source_artifact_path)
    expected_name = artifact_prefix + candidate_sha
    artifact_id, artifact_name, artifact_digest, artifact_size, _ = _artifact_metadata(
        artifact, run_id, expected_name
    )
    observed_digest, observed_size = digest_file(archive_path)
    _same(observed_size, artifact_size, "downloaded source archive size")
    _same(observed_digest, artifact_digest, "downloaded source archive digest")
    content, report = _read_archive(archive_path, report_path)

    report_schema = load_schema(root, report_schema_path)
    validate_schema(report, report_schema, f"{role} owner report")
    git = report.get("git")
    if not isinstance(git, dict):
        fail(f"{role} report git binding is missing")
    _same(git.get("revision"), candidate_sha, f"{role} report revision")
    _same(git.get("tree"), candidate_tree, f"{role} report tree")
    if role == "gr":
        _same(report.get("release"), {"id": "distribution-1.0", "version": "1.0.0", "state": "qualified"}, "GR release state")
        outcome = "qualified"
        owner_issue = "#167"
        owner_workflow = ".github/workflows/autonomy-gr.yml"
    else:
        closure_status = report.get("closure_status")
        if closure_status not in {"classified-with-gaps", "qualified"}:
            fail(f"terminal report closure status is invalid: {closure_status!r}")
        if closure_status == "qualified":
            _same(report.get("mode"), "final", "qualified terminal report mode")
            outcome = "qualified"
        else:
            _same(report.get("mode"), "normal", "classified terminal report mode")
            outcome = terminal_outcome
        owner_issue = "#179"
        owner_workflow = ".github/workflows/autonomy-production-scope.yml"

    report_digest = digest_bytes(content)
    handoff = {
        "schema": "cxxlens.ng-release-owner-handoff.v1",
        "document_version": "1.0.0",
        "role": role,
        "owner_issue": owner_issue,
        "workflow_path": owner_workflow,
        "candidate": {"sha": candidate_sha, "tree": candidate_tree},
        "input_selection_digest": selection_digest,
        "source": {
            "repository_id": int(repository_id),
            "workflow_id": workflow_id,
            "workflow_path": ".github/workflows/quality.yml",
            "run_id": run_id,
            "run_attempt": 1,
            "artifact_id": artifact_id,
            "artifact_name": artifact_name,
            "artifact_digest": artifact_digest,
        },
        "report": {
            "path": report_path,
            "schema": (
                "cxxlens.ng-release-qualification-report.v1"
                if role == "gr"
                else "cxxlens.ng-production-scope-closure-report.v1"
            ),
            "digest": report_digest,
            "outcome": outcome,
        },
    }
    validate_schema(
        handoff,
        load_schema(root, HANDOFF_SCHEMA),
        f"{role} owner handoff",
    )
    report_output.parent.mkdir(parents=True, exist_ok=True)
    handoff_output.parent.mkdir(parents=True, exist_ok=True)
    report_output.write_bytes(content)
    handoff_output.write_text(
        json.dumps(handoff, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return handoff


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("check")
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--role", choices=("gr", "terminal_scope"), required=True)
    parser.add_argument("--candidate-sha", required=True)
    parser.add_argument("--candidate-tree", required=True)
    parser.add_argument("--selection-digest", required=True)
    parser.add_argument("--source-run", type=pathlib.Path, required=True)
    parser.add_argument("--source-workflow", type=pathlib.Path, required=True)
    parser.add_argument("--source-artifact", type=pathlib.Path, required=True)
    parser.add_argument("--archive", type=pathlib.Path, required=True)
    parser.add_argument("--report-output", type=pathlib.Path, required=True)
    parser.add_argument("--handoff-output", type=pathlib.Path, required=True)
    arguments = parser.parse_args(argv)
    if arguments.check != "check":
        parser.error("the only command is check")
    try:
        check_and_extract(
            root=arguments.root.resolve(),
            role=arguments.role,
            candidate_sha=arguments.candidate_sha,
            candidate_tree=arguments.candidate_tree,
            selection_digest=arguments.selection_digest,
            source_run_path=arguments.source_run.resolve(),
            source_workflow_path=arguments.source_workflow.resolve(),
            source_artifact_path=arguments.source_artifact.resolve(),
            archive_path=arguments.archive.resolve(),
            report_output=arguments.report_output.resolve(),
            handoff_output=arguments.handoff_output.resolve(),
        )
    except OwnerHandoffError as error:
        print(f"release-owner-handoff: {error}", file=sys.stderr)
        return 1
    print("release-owner-handoff: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
