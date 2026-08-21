#!/usr/bin/env python3
"""Collect exact release producer objects using explicit GitHub IDs only.

This is the connected half of the release-evidence handoff.  It never searches
for a latest run: every run and artifact is addressed by an ID from the
selection document, downloaded directly, and represented in the offline bundle
that ``check_ng_release_evidence_bundle.py`` validates.  The ``download``
command is also used by owner workflows to stream one explicitly selected
artifact under the same metadata and size boundary before handoff validation.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import re
import stat
import subprocess
import sys
import zipfile
from typing import Any
from urllib.parse import urlparse

from check_ng_release_evidence_bundle import (  # noqa: E402
    ArtifactSpec,
    ROLE_SPECS,
    ReleaseEvidenceError,
    selection_digest,
    validate_selection,
)


class ReleaseEvidenceCollectionError(ValueError):
    """A selected GitHub object could not be authenticated and downloaded."""


def fail(message: str) -> None:
    raise ReleaseEvidenceCollectionError(message)


MAX_ARTIFACT_BYTES = 1 << 30
STREAM_CHUNK_BYTES = 1024 * 1024
REPOSITORY_COMPONENT = re.compile(r"^[A-Za-z0-9_.-]+$")


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
        fail(f"expected JSON object: {path}")
    return value


def write_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def gh_json(endpoint: str) -> dict[str, Any]:
    try:
        result = subprocess.run(
            ["gh", "api", endpoint],
            check=True,
            capture_output=True,
            env=os.environ.copy(),
        )
    except (OSError, subprocess.CalledProcessError) as error:
        fail(f"GitHub API request failed for explicit endpoint {endpoint}: {error}")
    try:
        value = json.loads(result.stdout.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys)
    except (UnicodeError, json.JSONDecodeError) as error:
        fail(f"GitHub API response is not strict JSON for {endpoint}: {error}")
    if not isinstance(value, dict):
        fail(f"GitHub API response is not an object for {endpoint}")
    return value


def gh_archive(endpoint: str, path: pathlib.Path) -> None:
    temporary = path.with_name(path.name + ".partial")
    process: subprocess.Popen[bytes] | None = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary.unlink(missing_ok=True)
        process = subprocess.Popen(
            ["gh", "api", endpoint],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=os.environ.copy(),
        )
        if process.stdout is None:
            fail(f"GitHub artifact download produced no stream: {endpoint}")
        with temporary.open("wb") as output:
            total = 0
            while True:
                chunk = process.stdout.read(STREAM_CHUNK_BYTES)
                if not chunk:
                    break
                total += len(chunk)
                if total > MAX_ARTIFACT_BYTES:
                    fail(
                        f"GitHub artifact exceeds {MAX_ARTIFACT_BYTES}-byte bound: "
                        f"{endpoint}"
                    )
                output.write(chunk)
        stderr = process.communicate()[1]
        if process.returncode != 0:
            detail = stderr.decode("utf-8", errors="replace").strip()
            suffix = f": {detail}" if detail else ""
            fail(f"GitHub artifact download failed for explicit endpoint {endpoint}{suffix}")
        temporary.replace(path)
    except (OSError, subprocess.SubprocessError) as error:
        fail(f"GitHub artifact download failed for explicit endpoint {endpoint}: {error}")
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        temporary.unlink(missing_ok=True)


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_file(path: pathlib.Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    try:
        with path.open("rb") as stream:
            while True:
                chunk = stream.read(STREAM_CHUNK_BYTES)
                if not chunk:
                    break
                size += len(chunk)
                if size > MAX_ARTIFACT_BYTES:
                    fail(f"downloaded artifact exceeds {MAX_ARTIFACT_BYTES}-byte bound: {path}")
                digest.update(chunk)
    except OSError as error:
        fail(f"cannot read downloaded artifact {path}: {error}")
    return "sha256:" + digest.hexdigest(), size


def _valid_digest(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) == len("sha256:") + 64
        and value.startswith("sha256:")
        and all(character in "0123456789abcdef" for character in value[7:])
    )


def _url_ends_with(value: Any, suffix: str) -> bool:
    if not isinstance(value, str):
        return False
    return urlparse(value).path.rstrip("/").endswith(suffix)


def validate_artifact_metadata(
    artifact: dict[str, Any], artifact_id: int, expected_name: str, run_id: int
) -> None:
    """Authenticate artifact metadata before opening its archive stream."""

    if artifact.get("id") != artifact_id:
        fail(f"artifact API identity differs: {artifact_id}")
    if artifact.get("name") != expected_name:
        fail(
            "artifact name differs: "
            f"expected={expected_name!r}, observed={artifact.get('name')!r}"
        )
    if artifact.get("expired") is not False:
        fail(f"artifact is expired or has no expiry attestation: {artifact_id}")
    workflow_run = artifact.get("workflow_run")
    if not isinstance(workflow_run, dict) or workflow_run.get("id") != run_id:
        fail(f"artifact {artifact_id} was not produced by selected run")
    size = artifact.get("size_in_bytes")
    if not isinstance(size, int) or isinstance(size, bool) or not 0 < size <= MAX_ARTIFACT_BYTES:
        fail(f"artifact size is outside the bounded range: {artifact_id}")
    if not _valid_digest(artifact.get("digest")):
        fail(f"artifact digest is unavailable or malformed: {artifact_id}")
    if not _url_ends_with(artifact.get("url"), f"/actions/artifacts/{artifact_id}"):
        fail(f"artifact API URL is not bound to its ID: {artifact_id}")
    if not _url_ends_with(
        artifact.get("archive_download_url"), f"/actions/artifacts/{artifact_id}/zip"
    ):
        fail(f"artifact download URL is not bound to its ID: {artifact_id}")


def _artifact_endpoint(repository: str, artifact_id: int) -> str:
    """Build an explicit artifact endpoint from a validated repository name."""

    components = repository.split("/")
    if (
        len(components) != 2
        or not all(REPOSITORY_COMPONENT.fullmatch(component) for component in components)
    ):
        fail(f"repository is not a canonical owner/name pair: {repository!r}")
    if not isinstance(artifact_id, int) or isinstance(artifact_id, bool) or artifact_id <= 0:
        fail(f"artifact ID is invalid: {artifact_id!r}")
    return f"repos/{repository}/actions/artifacts/{artifact_id}/zip"


def download_selected_artifact(
    *,
    metadata_path: pathlib.Path,
    repository: str,
    artifact_id: int,
    run_id: int,
    expected_name: str,
    output_path: pathlib.Path,
) -> tuple[str, int]:
    """Download one explicitly selected artifact with metadata and size bounds.

    Owner workflows use this connected helper before the offline handoff checker.
    Metadata is authenticated before the archive stream starts, and the final
    bytes are checked against GitHub's advertised digest and size.  A failed
    download or mismatch never leaves a seemingly usable output archive.
    """

    metadata = load_json(metadata_path)
    validate_artifact_metadata(metadata, artifact_id, expected_name, run_id)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        gh_archive(_artifact_endpoint(repository, artifact_id), output_path)
        observed_digest, observed_size = digest_file(output_path)
        if observed_size != metadata["size_in_bytes"]:
            fail(
                f"artifact {artifact_id} size differs from GitHub metadata: "
                f"expected={metadata['size_in_bytes']}, observed={observed_size}"
            )
        if observed_digest != metadata["digest"]:
            fail(
                f"artifact {artifact_id} digest differs from GitHub metadata: "
                f"expected={metadata['digest']}, observed={observed_digest}"
            )
    except Exception:
        try:
            output_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise
    return observed_digest, observed_size


def normalize_run(
    raw: dict[str, Any],
    selected: dict[str, Any],
    candidate_sha: str,
    candidate_repository_id: int,
) -> dict[str, Any]:
    repository = raw.get("repository")
    if not isinstance(repository, dict) or not isinstance(repository.get("id"), int):
        fail("selected run lacks repository identity")
    head_repository = raw.get("head_repository")
    if not isinstance(head_repository, dict) or not isinstance(head_repository.get("id"), int):
        fail("selected run lacks head repository identity")
    for key in ("id", "workflow_id", "run_attempt"):
        if not isinstance(raw.get(key), int) or raw[key] <= 0:
            fail(f"selected run has invalid {key}")
    if raw.get("run_attempt") != 1:
        fail("only first-attempt producer runs are release eligible")
    if raw.get("workflow_id") != selected["workflow_id"]:
        fail("selected run workflow ID differs")
    if raw.get("path") != selected["workflow_path"]:
        fail("selected run workflow path differs")
    if raw.get("event") != selected["event"]:
        fail("selected run event differs")
    if raw.get("head_sha") != candidate_sha:
        fail("selected run head SHA differs from selection candidate")
    if raw.get("head_branch") != "main":
        fail("selected run is not on main")
    if repository["id"] != candidate_repository_id or head_repository["id"] != candidate_repository_id:
        fail("selected run repository identity differs from candidate")
    if raw.get("status") != "completed" or raw.get("conclusion") != "success":
        fail("selected producer run is not completed successfully")
    return {
        "repository_id": repository["id"],
        "workflow_id": raw["workflow_id"],
        "run_id": raw["id"],
        "run_attempt": 1,
        "workflow_path": raw["path"],
        "event": raw["event"],
        "status": raw["status"],
        "conclusion": raw["conclusion"],
        "head_sha": raw["head_sha"],
        "head_repository_id": head_repository["id"],
        "api_url": raw.get("url") or raw.get("jobs_url", ""),
        "html_url": raw.get("html_url", ""),
        "created_at": raw["created_at"],
        "updated_at": raw["updated_at"],
    }


def authenticate_workflow(raw: dict[str, Any], selected: dict[str, Any]) -> None:
    if raw.get("id") != selected["workflow_id"]:
        fail("selected workflow identity differs")
    if raw.get("path") != selected["workflow_path"]:
        fail("selected workflow path differs")
    if raw.get("state") != "active":
        fail("selected workflow is not active")


def authenticate_candidate(
    raw: dict[str, Any], candidate_sha: str, candidate_tree: str
) -> None:
    if raw.get("sha") != candidate_sha:
        fail("candidate commit identity differs from selection")
    commit = raw.get("commit")
    tree = commit.get("tree") if isinstance(commit, dict) else None
    if not isinstance(tree, dict) or tree.get("sha") != candidate_tree:
        fail("candidate commit tree differs from selection")


def artifact_result(role: str, kind: str, path: str, content: bytes) -> str:
    if role == "heavy":
        if kind == "quality-report":
            return "passed"
        try:
            value = json.loads(content.decode("utf-8"))
        except (UnicodeError, json.JSONDecodeError) as error:
            fail(f"heavy postflight is not JSON: {error}")
        return "current" if value.get("disposition") == "current" else "superseded"
    if role == "nightly":
        return "passed"
    try:
        value = json.loads(content.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        fail(f"{role} report is not JSON: {error}")
    if path.endswith("release-qualification-report.json"):
        return "qualified" if value.get("release", {}).get("state") == "qualified" else "not-qualified"
    if path.endswith("production-scope-closure-report.json"):
        status = value.get("closure_status")
        if status not in {"qualified", "classified-with-gaps"}:
            fail(f"terminal report closure status is invalid: {status!r}")
        return status
    if path.endswith("release-owner-handoff.json"):
        return value.get("report", {}).get("outcome", "not-qualified")
    fail(f"cannot derive producer result for {path}")


def read_artifact_files(
    archive_path: pathlib.Path,
    role: str,
    spec: ArtifactSpec,
    candidate_sha: str,
    candidate_tree: str,
) -> list[dict[str, Any]]:
    expected = {row.path: row for row in spec.files}
    observed: dict[str, dict[str, Any]] = {}
    total_uncompressed = 0
    try:
        with zipfile.ZipFile(archive_path) as archive:
            infos = archive.infolist()
            for info in infos:
                name = info.filename
                path = pathlib.PurePosixPath(name)
                if (
                    not name
                    or path.is_absolute()
                    or "\\" in name
                    or "\x00" in name
                    or ".." in path.parts
                    or name.endswith("/")
                ):
                    fail(f"downloaded artifact has unsafe member: {name!r}")
                mode = (info.external_attr >> 16) & 0o170000
                if mode not in {0, stat.S_IFREG}:
                    fail(f"downloaded artifact contains non-regular member: {name!r}")
                if name in observed:
                    fail(f"downloaded artifact has duplicate member: {name}")
                if name not in expected:
                    fail(f"downloaded artifact has unexpected member: {name}")
                digest = hashlib.sha256()
                member_size = 0
                needs_content = not (
                    role == "nightly"
                    or (role == "heavy" and spec.kind == "quality-report")
                )
                content_parts: list[bytes] = []
                with archive.open(info, "r") as stream:
                    while True:
                        chunk = stream.read(STREAM_CHUNK_BYTES)
                        if not chunk:
                            break
                        member_size += len(chunk)
                        total_uncompressed += len(chunk)
                        if (
                            member_size > MAX_ARTIFACT_BYTES
                            or total_uncompressed > MAX_ARTIFACT_BYTES
                        ):
                            fail(
                                "downloaded artifact uncompressed bytes exceed "
                                f"{MAX_ARTIFACT_BYTES}-byte bound"
                            )
                        digest.update(chunk)
                        if needs_content:
                            content_parts.append(chunk)
                content = b"".join(content_parts) if needs_content else b""
                observed[name] = {
                    "path": name,
                    "schema": expected[name].schema,
                    "digest": "sha256:" + digest.hexdigest(),
                    "byte_count": member_size,
                    "revision": candidate_sha,
                    "tree": candidate_tree,
                    "result": artifact_result(role, spec.kind, name, content),
                }
    except ReleaseEvidenceCollectionError:
        raise
    except (OSError, zipfile.BadZipFile, RuntimeError) as error:
        fail(f"cannot inspect downloaded artifact {archive_path}: {error}")
    if set(observed) != set(expected):
        fail(f"artifact file set differs for {spec.kind}")
    return [observed[path] for path in sorted(observed)]


def collect_role(
    role: str,
    selected: dict[str, Any],
    artifact_root: pathlib.Path,
    selected_digest: str,
    repository_name: str,
    candidate_sha: str,
    candidate_tree: str,
    candidate_repository_id: int,
) -> tuple[dict[str, Any], dict[str, Any]]:
    endpoint_prefix = f"repos/{repository_name}/actions"
    raw_workflow = gh_json(f"{endpoint_prefix}/workflows/{selected['workflow_id']}")
    authenticate_workflow(raw_workflow, selected)
    raw_run = gh_json(f"{endpoint_prefix}/runs/{selected['run_id']}")
    run = normalize_run(raw_run, selected, candidate_sha, candidate_repository_id)
    receipts: list[dict[str, Any]] = []
    role_spec = ROLE_SPECS[role]
    for artifact_id, artifact_spec in zip(selected["artifact_ids"], role_spec.artifacts, strict=True):
        raw_artifact = gh_json(f"{endpoint_prefix}/artifacts/{artifact_id}")
        validate_artifact_metadata(
            raw_artifact,
            artifact_id,
            role_spec.artifacts[len(receipts)].name_template.format(sha=candidate_sha),
            selected["run_id"],
        )
        archive_path = artifact_root / f"{artifact_id}.zip"
        gh_archive(f"{endpoint_prefix}/artifacts/{artifact_id}/zip", archive_path)
        archive_digest, archive_size = digest_file(archive_path)
        if raw_artifact.get("size_in_bytes") != archive_size:
            fail(f"artifact {artifact_id} size differs from GitHub metadata")
        if raw_artifact.get("digest") != archive_digest:
            fail(f"artifact {artifact_id} digest differs from GitHub metadata")
        files = read_artifact_files(
            archive_path,
            role,
            artifact_spec,
            candidate_sha,
            candidate_tree,
        )
        receipts.append(
            {
                "kind": artifact_spec.kind,
                "artifact_id": artifact_id,
                "name": raw_artifact.get("name"),
                "api_url": raw_artifact.get("url"),
                "archive_download_url": raw_artifact.get("archive_download_url"),
                "expired": raw_artifact.get("expired"),
                "size_bytes": archive_size,
                "digest": archive_digest,
                "archive_digest": archive_digest,
                "download_source": "producer-run-artifact-id-direct-download",
                "downloaded_archive": {"path": archive_path.name},
                "files": files,
            }
        )
    receipt = {
        "owner_issue": role_spec.owner_issue,
        "workflow_path": role_spec.workflow_path,
        "workflow_id": selected["workflow_id"],
        "event": selected["event"],
        "run": run,
        "artifacts": receipts,
        "disposition": "qualified",
    }
    if role in {"gr", "terminal_scope"}:
        receipt["input_selection_digest"] = selected_digest
    if role == "heavy":
        postflight = next(row for row in receipts if row["kind"] == "postflight")
        receipt["disposition"] = (
            "qualified"
            if all(file_row["result"] == "passed" for row in receipts if row["kind"] == "quality-report" for file_row in row["files"])
            and postflight["files"][0]["result"] == "current"
            else "not-qualified"
        )
    elif role != "nightly":
        receipt["disposition"] = (
            "qualified"
            if all(file_row["result"] in {"passed", "qualified"} for row in receipts for file_row in row["files"])
            else "not-qualified"
        )
    return receipt, {"run": run, "artifacts": receipts}


def collect(
    *,
    selection_path: pathlib.Path,
    selection_output: pathlib.Path,
    bundle_output: pathlib.Path,
    artifact_root: pathlib.Path,
) -> dict[str, Any]:
    selection = load_json(selection_path)
    validate_selection(selection)
    candidate_sha = selection["candidate"]["sha"]
    candidate_tree = selection["candidate"]["tree"]
    repository_name = selection["candidate"]["repository"]
    selected_digest = selection_digest(selection)
    artifact_root.mkdir(parents=True, exist_ok=True)
    candidate_raw = gh_json(
        f"repos/{repository_name}/commits/{candidate_sha}"
    )
    authenticate_candidate(candidate_raw, candidate_sha, candidate_tree)
    roles: dict[str, Any] = {}
    for role in ROLE_SPECS:
        roles[role], _ = collect_role(
            role,
            selection["roles"][role],
            artifact_root,
            selected_digest,
            repository_name,
            candidate_sha,
            candidate_tree,
            selection["candidate"]["repository_id"],
        )
    all_qualified = all(role["disposition"] == "qualified" for role in roles.values())
    any_superseded = any(
        file_row["result"] == "superseded"
        for role in roles.values()
        for artifact in role["artifacts"]
        for file_row in artifact["files"]
    )
    if all_qualified:
        outcome = "qualified-inputs-ready"
        reason_codes: list[str] = []
    elif any_superseded:
        outcome = "superseded"
        reason_codes = ["evidence.main-superseded"]
    else:
        outcome = "not-qualified"
        reason_codes = ["evidence.role-not-qualified"]
    bundle = {
        "schema": "cxxlens.ng-release-evidence-bundle.v1",
        "document_version": "1.0.0",
        "contract_id": "development.authenticated-release-evidence-handoff.v1",
        "bundle_id": f"bundle-{selection['selection_id']}",
        "generated_at": datetime.datetime.now(datetime.UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "authentication": {
            "provider": "github-actions",
            "metadata_source": "github-actions-rest-api",
            "attempt_policy": "first-attempt-only",
            "download_policy": "producer-run-artifact-id-direct-download",
        },
        "selection_digest": selected_digest,
        "candidate": selection["candidate"],
        "roles": roles,
        "outcome": outcome,
        "reason_codes": reason_codes,
        "gr_issued": False,
        "production_qualification": "not-claimed",
    }
    write_json(selection_output, selection)
    write_json(bundle_output, bundle)
    return bundle


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("collect", "download"))
    parser.add_argument("--selection", type=pathlib.Path)
    parser.add_argument("--selection-output", type=pathlib.Path)
    parser.add_argument("--bundle-output", type=pathlib.Path)
    parser.add_argument("--artifact-root", type=pathlib.Path)
    parser.add_argument("--artifact-metadata", type=pathlib.Path)
    parser.add_argument("--repository")
    parser.add_argument("--artifact-id", type=int)
    parser.add_argument("--run-id", type=int)
    parser.add_argument("--expected-name")
    parser.add_argument("--output", type=pathlib.Path)
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "collect":
            required = {
                "--selection": arguments.selection,
                "--selection-output": arguments.selection_output,
                "--bundle-output": arguments.bundle_output,
                "--artifact-root": arguments.artifact_root,
            }
            missing = [name for name, value in required.items() if value is None]
            if missing:
                parser.error("collect requires " + ", ".join(missing))
            bundle = collect(
                selection_path=arguments.selection.resolve(),
                selection_output=arguments.selection_output.resolve(),
                bundle_output=arguments.bundle_output.resolve(),
                artifact_root=arguments.artifact_root.resolve(),
            )
        else:
            required = {
                "--artifact-metadata": arguments.artifact_metadata,
                "--repository": arguments.repository,
                "--artifact-id": arguments.artifact_id,
                "--run-id": arguments.run_id,
                "--expected-name": arguments.expected_name,
                "--output": arguments.output,
            }
            missing = [name for name, value in required.items() if value is None]
            if missing:
                parser.error("download requires " + ", ".join(missing))
            digest, size = download_selected_artifact(
                metadata_path=arguments.artifact_metadata.resolve(),
                repository=arguments.repository,
                artifact_id=arguments.artifact_id,
                run_id=arguments.run_id,
                expected_name=arguments.expected_name,
                output_path=arguments.output.resolve(),
            )
            print(f"release-evidence-downloader: {digest} ({size} bytes)")
            return 0
    except (ReleaseEvidenceCollectionError, ReleaseEvidenceError) as error:
        print(f"release-evidence-collector: {error}", file=sys.stderr)
        return 1
    print(f"release-evidence-collector: {bundle['outcome']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
