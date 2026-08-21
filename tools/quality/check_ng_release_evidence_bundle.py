#!/usr/bin/env python3
"""Validate the v1 authenticated release-evidence handoff boundary.

The checker is intentionally offline. A connected producer must obtain the run
and artifact objects from GitHub's Actions API and download each artifact by its
selected artifact ID before constructing the bundle. This checker verifies the
captured metadata, exact role/cardinality rules, archive/file digests, and
candidate cross-bindings; it never discovers a latest run and never issues GR.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import stat
import sys
import zipfile
from dataclasses import dataclass
from typing import Any
from urllib.parse import urlparse

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
SELECTION_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_release_evidence_selection.schema.yaml"
)
BUNDLE_SCHEMA = pathlib.Path("schemas/cxxlens_ng_release_evidence_bundle.schema.yaml")
MAX_ARTIFACT_BYTES = 1 << 30


class ReleaseEvidenceError(ValueError):
    """A fail-closed release-evidence handoff violation."""


@dataclass(frozen=True)
class FileSpec:
    path: str
    schema: str
    result: str | frozenset[str]


@dataclass(frozen=True)
class ArtifactSpec:
    kind: str
    name_template: str
    files: tuple[FileSpec, ...]


@dataclass(frozen=True)
class RoleSpec:
    owner_issue: str
    workflow_path: str
    events: frozenset[str]
    artifacts: tuple[ArtifactSpec, ...]


ROLE_SPECS: dict[str, RoleSpec] = {
    "heavy": RoleSpec(
        owner_issue="#173",
        workflow_path=".github/workflows/autonomy-heavy.yml",
        events=frozenset({"workflow_run"}),
        artifacts=(
            ArtifactSpec(
                kind="quality-report",
                name_template="cxxlens-autonomy-heavy-provisional-{sha}",
                files=(
                    FileSpec(
                        "cxxlens-autonomy-heavy.json",
                        "cxxlens.quality-run-report.v1",
                        "passed",
                    ),
                    FileSpec("cxxlens-autonomy-heavy.junit.xml", "junit.xml", "passed"),
                ),
            ),
            ArtifactSpec(
                kind="postflight",
                name_template="cxxlens-autonomy-heavy-postflight-{sha}",
                files=(
                    FileSpec(
                        "cxxlens-autonomy-heavy-postflight.json",
                        "cxxlens.autonomy-heavy-freshness.v1",
                        frozenset({"current", "superseded"}),
                    ),
                ),
            ),
        ),
    ),
    "nightly": RoleSpec(
        owner_issue="#173",
        workflow_path=".github/workflows/nightly.yml",
        events=frozenset({"schedule", "workflow_dispatch"}),
        artifacts=(
            ArtifactSpec(
                kind="evidence-set",
                name_template="cxxlens-nightly-evidence-{sha}",
                files=(
                    FileSpec(
                        "nightly-evidence-set.json",
                        "cxxlens.quality-evidence-set.v1",
                        "passed",
                    ),
                ),
            ),
        ),
    ),
    "gr": RoleSpec(
        owner_issue="#167",
        workflow_path=".github/workflows/autonomy-gr.yml",
        events=frozenset({"workflow_dispatch"}),
        artifacts=(
            ArtifactSpec(
                kind="gr-report",
                name_template="cxxlens-ng-release-qualification-{sha}",
                files=(
                    FileSpec(
                        "cxxlens-ng-release-qualification-report.json",
                        "cxxlens.ng-release-qualification-report.v1",
                        frozenset({"qualified", "not-qualified"}),
                    ),
                ),
            ),
        ),
    ),
    "terminal_scope": RoleSpec(
        owner_issue="#179",
        workflow_path=".github/workflows/autonomy-production-scope.yml",
        events=frozenset({"workflow_dispatch"}),
        artifacts=(
            ArtifactSpec(
                kind="terminal-scope-report",
                name_template="cxxlens-ng-production-scope-closure-{sha}",
                files=(
                    FileSpec(
                        "cxxlens-ng-production-scope-closure-report.json",
                        "cxxlens.ng-production-scope-closure-report.v1",
                        frozenset({"qualified", "classified-with-gaps"}),
                    ),
                ),
            ),
        ),
    ),
}


def fail(message: str) -> None:
    raise ReleaseEvidenceError(message)


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            fail(f"duplicate JSON object member: {key}")
        result[key] = value
    return result


def load_document(path: pathlib.Path) -> dict[str, Any]:
    try:
        raw = path.read_bytes()
    except OSError as error:
        fail(f"cannot read {path}: {error}")
    try:
        if path.suffix.lower() == ".json":
            value = json.loads(
                raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys
            )
        else:
            value = yaml.safe_load(raw)
    except (UnicodeError, json.JSONDecodeError, yaml.YAMLError) as error:
        fail(f"cannot decode {path}: {error}")
    if not isinstance(value, dict):
        fail(f"expected object document: {path}")
    return value


def load_schema(root: pathlib.Path, relative: pathlib.Path) -> dict[str, Any]:
    value = load_document(root / relative)
    try:
        jsonschema.Draft202012Validator.check_schema(value)
    except jsonschema.SchemaError as error:
        fail(f"invalid checker schema {relative}: {error.message}")
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


def canonical_digest(value: Any) -> str:
    encoded = json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def selection_digest(selection: dict[str, Any]) -> str:
    return canonical_digest(selection)


def _same(left: Any, right: Any, label: str) -> None:
    if left != right:
        fail(f"{label} differs: expected={right!r}, observed={left!r}")


def _url_ends_with(url: str, suffix: str, label: str) -> None:
    path = urlparse(url).path.rstrip("/")
    if not path.endswith(suffix):
        fail(f"{label} does not identify the exact producer object: {url}")


def validate_selection(
    selection: dict[str, Any], root: pathlib.Path = ROOT
) -> dict[str, Any]:
    validate_schema(
        selection,
        load_schema(root, SELECTION_SCHEMA),
        "release evidence selection",
    )
    seen_artifacts: set[int] = set()
    seen_runs: dict[int, str] = {}
    seen_workflows: dict[int, str] = {}
    for role_name in ROLE_SPECS:
        role = selection["roles"][role_name]
        run_id = role["run_id"]
        prior_role = seen_runs.get(run_id)
        if prior_role is not None:
            fail(
                "run ID is selected for multiple roles: "
                f"{run_id} ({prior_role}, {role_name})"
            )
        seen_runs[run_id] = role_name
        workflow_id = role["workflow_id"]
        prior_role = seen_workflows.get(workflow_id)
        if prior_role is not None:
            fail(
                "workflow ID is selected for multiple roles: "
                f"{workflow_id} ({prior_role}, {role_name})"
            )
        seen_workflows[workflow_id] = role_name
        for artifact_id in role["artifact_ids"]:
            if artifact_id in seen_artifacts:
                fail(f"artifact ID is selected for multiple roles: {artifact_id}")
            seen_artifacts.add(artifact_id)
    return selection


def _validate_files(
    artifact: dict[str, Any], spec: ArtifactSpec, candidate: dict[str, Any]
) -> list[dict[str, Any]]:
    files = artifact["files"]
    expected = {row.path: row for row in spec.files}
    observed: dict[str, dict[str, Any]] = {}
    for row in files:
        path = row["path"]
        if path in observed:
            fail(f"artifact contains duplicate file path: {path}")
        if path.startswith("/") or path.startswith("./") or ".." in path.split("/"):
            fail(f"artifact contains unsafe file path: {path}")
        observed[path] = row
        if row["revision"] != candidate["sha"] or row["tree"] != candidate["tree"]:
            fail(f"artifact file is not bound to candidate revision/tree: {path}")
        if row["byte_count"] > MAX_ARTIFACT_BYTES:
            fail(f"artifact file exceeds v1 byte bound: {path}")
    if set(observed) != set(expected):
        fail(
            "artifact file set differs: "
            f"expected={sorted(expected)}, observed={sorted(observed)}"
        )
    for path, file_spec in expected.items():
        row = observed[path]
        _same(row["schema"], file_spec.schema, f"artifact file schema {path}")
        if isinstance(file_spec.result, str):
            _same(row["result"], file_spec.result, f"artifact file result {path}")
        elif row["result"] not in file_spec.result:
            fail(
                f"artifact file result {path} is not an allowed terminal: "
                f"{row['result']!r}"
            )
    return list(observed.values())


def _archive_path(root: pathlib.Path, relative: str) -> pathlib.Path:
    relative_path = pathlib.PurePosixPath(relative)
    if (
        relative_path.is_absolute()
        or ".." in relative_path.parts
        or "\\" in relative
        or "\x00" in relative
    ):
        fail(f"downloaded archive path is unsafe: {relative}")
    root_path = root.resolve()
    path = (root_path / pathlib.Path(*relative_path.parts)).resolve()
    if path != root_path and root_path not in path.parents:
        fail(f"downloaded archive escapes artifact root: {relative}")
    try:
        mode = path.stat(follow_symlinks=False).st_mode
    except OSError as error:
        fail(f"downloaded archive is unavailable: {relative}: {error}")
    if stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
        fail(f"downloaded archive is not a regular file: {relative}")
    return path


def _digest_file(path: pathlib.Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    count = 0
    try:
        with path.open("rb") as stream:
            while True:
                chunk = stream.read(1024 * 1024)
                if not chunk:
                    break
                count += len(chunk)
                if count > MAX_ARTIFACT_BYTES:
                    fail(f"downloaded archive exceeds v1 byte bound: {path}")
                digest.update(chunk)
    except OSError as error:
        fail(f"cannot read downloaded archive {path}: {error}")
    return "sha256:" + digest.hexdigest(), count


def _validate_archive(
    artifact: dict[str, Any],
    expected_files: dict[str, dict[str, Any]],
    artifact_root: pathlib.Path,
) -> None:
    path = _archive_path(artifact_root, artifact["downloaded_archive"]["path"])
    archive_digest, archive_size = _digest_file(path)
    _same(archive_size, artifact["size_bytes"], f"downloaded archive size {path.name}")
    _same(archive_digest, artifact["digest"], f"downloaded archive digest {path.name}")
    seen: set[str] = set()
    total_uncompressed = 0
    try:
        with zipfile.ZipFile(path) as archive:
            for info in archive.infolist():
                name = info.filename
                if (
                    not name
                    or name.startswith("/")
                    or "\\" in name
                    or "\x00" in name
                    or ".." in pathlib.PurePosixPath(name).parts
                    or name.endswith("/")
                ):
                    fail(f"downloaded archive contains unsafe member: {name!r}")
                if name in seen:
                    fail(f"downloaded archive contains duplicate member: {name}")
                seen.add(name)
                mode = (info.external_attr >> 16) & 0o170000
                if mode not in {0, stat.S_IFREG}:
                    fail(f"downloaded archive contains non-regular member: {name}")
                row = expected_files.get(name)
                if row is None:
                    fail(f"downloaded archive contains unexpected member: {name}")
                digest = hashlib.sha256()
                count = 0
                with archive.open(info, "r") as stream:
                    while True:
                        chunk = stream.read(1024 * 1024)
                        if not chunk:
                            break
                        count += len(chunk)
                        total_uncompressed += len(chunk)
                        if count > MAX_ARTIFACT_BYTES or total_uncompressed > MAX_ARTIFACT_BYTES:
                            fail("downloaded archive uncompressed bytes exceed v1 bound")
                        digest.update(chunk)
                _same(count, row["byte_count"], f"downloaded member byte count {name}")
                _same(
                    "sha256:" + digest.hexdigest(),
                    row["digest"],
                    f"downloaded member digest {name}",
                )
    except (OSError, zipfile.BadZipFile, RuntimeError) as error:
        if isinstance(error, ReleaseEvidenceError):
            raise
        fail(f"cannot inspect downloaded archive {path}: {error}")
    if seen != set(expected_files):
        fail(
            "downloaded archive file set differs: "
            f"expected={sorted(expected_files)}, observed={sorted(seen)}"
        )


def _validate_artifact(
    artifact: dict[str, Any],
    spec: ArtifactSpec,
    selection_role: dict[str, Any],
    candidate: dict[str, Any],
    seen_artifacts: set[int],
    artifact_root: pathlib.Path,
    seen_archive_paths: set[str],
) -> list[dict[str, Any]]:
    artifact_id = artifact["artifact_id"]
    if artifact_id in seen_artifacts:
        fail(f"artifact ID is duplicated across the handoff: {artifact_id}")
    seen_artifacts.add(artifact_id)
    if artifact_id not in selection_role["artifact_ids"]:
        fail(f"artifact ID was not explicitly selected: {artifact_id}")
    archive_relative_path = artifact["downloaded_archive"]["path"]
    if archive_relative_path in seen_archive_paths:
        fail(f"downloaded archive path is reused across artifact IDs: {archive_relative_path}")
    seen_archive_paths.add(archive_relative_path)
    _same(
        artifact["name"],
        spec.name_template.format(sha=candidate["sha"]),
        f"artifact name for {spec.kind}",
    )
    _same(artifact["kind"], spec.kind, f"artifact kind for {artifact_id}")
    if artifact["size_bytes"] > MAX_ARTIFACT_BYTES:
        fail(f"artifact exceeds v1 byte bound: {artifact_id}")
    _same(
        artifact["digest"],
        artifact["archive_digest"],
        f"artifact archive digest for {artifact_id}",
    )
    _url_ends_with(
        artifact["api_url"],
        f"/actions/artifacts/{artifact_id}",
        f"artifact API URL {artifact_id}",
    )
    _url_ends_with(
        artifact["archive_download_url"],
        f"/actions/artifacts/{artifact_id}/zip",
        f"artifact download URL {artifact_id}",
    )
    files = _validate_files(artifact, spec, candidate)
    _validate_archive(
        artifact,
        {row["path"]: row for row in files},
        artifact_root,
    )
    return files


def _validate_role(
    role_name: str,
    selection_role: dict[str, Any],
    receipt: dict[str, Any],
    candidate: dict[str, Any],
    selection_digest_value: str,
    seen_artifacts: set[int],
    artifact_root: pathlib.Path,
    seen_archive_paths: set[str],
) -> bool:
    role_spec = ROLE_SPECS[role_name]
    _same(receipt["owner_issue"], role_spec.owner_issue, f"{role_name} owner issue")
    _same(receipt["workflow_path"], role_spec.workflow_path, f"{role_name} workflow path")
    _same(receipt["workflow_id"], selection_role["workflow_id"], f"{role_name} workflow ID")
    _same(receipt["event"], selection_role["event"], f"{role_name} event")
    if receipt["event"] not in role_spec.events:
        fail(f"{role_name} uses an ineligible event: {receipt['event']}")
    if receipt["workflow_path"] != role_spec.workflow_path:
        fail(f"{role_name} workflow path is not canonical")
    run = receipt["run"]
    _same(run["repository_id"], candidate["repository_id"], f"{role_name} run repository")
    _same(run["workflow_id"], selection_role["workflow_id"], f"{role_name} run workflow ID")
    _same(run["run_id"], selection_role["run_id"], f"{role_name} run ID")
    _same(run["run_attempt"], 1, f"{role_name} run attempt")
    _same(run["workflow_path"], role_spec.workflow_path, f"{role_name} run workflow path")
    _same(run["event"], selection_role["event"], f"{role_name} run event")
    _same(run["head_sha"], candidate["sha"], f"{role_name} run head SHA")
    _same(
        run["head_repository_id"],
        candidate["repository_id"],
        f"{role_name} run head repository",
    )
    _url_ends_with(run["api_url"], f"/actions/runs/{run['run_id']}", f"{role_name} run API URL")
    _url_ends_with(run["html_url"], f"/actions/runs/{run['run_id']}", f"{role_name} run HTML URL")
    if role_name in {"gr", "terminal_scope"}:
        _same(
            receipt["input_selection_digest"],
            selection_digest_value,
            f"{role_name} input selection digest",
        )
    artifacts = receipt["artifacts"]
    if len(artifacts) != len(role_spec.artifacts):
        fail(f"{role_name} artifact cardinality differs")
    by_kind = {row["kind"]: row for row in artifacts}
    if len(by_kind) != len(artifacts):
        fail(f"{role_name} contains duplicate artifact kinds")
    for artifact_spec in role_spec.artifacts:
        if artifact_spec.kind not in by_kind:
            fail(f"{role_name} is missing artifact kind: {artifact_spec.kind}")
        _validate_artifact(
            by_kind[artifact_spec.kind],
            artifact_spec,
            selection_role,
            candidate,
            seen_artifacts,
            artifact_root,
            seen_archive_paths,
        )
    if role_name == "heavy":
        quality_rows = by_kind["quality-report"]["files"]
        postflight_rows = by_kind["postflight"]["files"]
        expected_qualified = (
            all(row["result"] == "passed" for row in quality_rows)
            and len(postflight_rows) == 1
            and postflight_rows[0]["result"] == "current"
        )
    else:
        expected_qualified = all(
            row["result"] in {"passed", "qualified"}
            for artifact in artifacts
            for row in artifact["files"]
        )
    _same(
        receipt["disposition"],
        "qualified" if expected_qualified else "not-qualified",
        f"{role_name} disposition",
    )
    return expected_qualified


def validate_bundle(
    bundle: dict[str, Any],
    selection: dict[str, Any],
    root: pathlib.Path = ROOT,
    artifact_root: pathlib.Path | None = None,
) -> dict[str, Any]:
    validate_schema(
        bundle,
        load_schema(root, BUNDLE_SCHEMA),
        "release evidence bundle",
    )
    validate_selection(selection, root)
    candidate = selection["candidate"]
    _same(bundle["candidate"], candidate, "bundle candidate")
    expected_selection_digest = selection_digest(selection)
    _same(bundle["selection_digest"], expected_selection_digest, "selection digest")
    _same(
        bundle["authentication"],
        {
            "provider": "github-actions",
            "metadata_source": "github-actions-rest-api",
            "attempt_policy": "first-attempt-only",
            "download_policy": "producer-run-artifact-id-direct-download",
        },
        "authentication policy",
    )
    seen_artifacts: set[int] = set()
    seen_archive_paths: set[str] = set()
    artifact_root = (artifact_root or root).resolve()
    role_qualified: dict[str, bool] = {}
    for role_name in ROLE_SPECS:
        role_qualified[role_name] = _validate_role(
            role_name,
            selection["roles"][role_name],
            bundle["roles"][role_name],
            candidate,
            expected_selection_digest,
            seen_artifacts,
            artifact_root,
            seen_archive_paths,
        )
    all_qualified = all(role_qualified.values())
    any_superseded = any(
        row["result"] == "superseded"
        for role in bundle["roles"].values()
        for artifact in role["artifacts"]
        for row in artifact["files"]
    )
    if all_qualified:
        _same(bundle["outcome"], "qualified-inputs-ready", "bundle outcome")
        _same(bundle["reason_codes"], [], "qualified bundle reason codes")
    elif any_superseded:
        _same(bundle["outcome"], "superseded", "stale bundle outcome")
        if "evidence.main-superseded" not in bundle["reason_codes"]:
            fail("superseded bundle lacks evidence.main-superseded")
    else:
        _same(bundle["outcome"], "not-qualified", "not-qualified bundle outcome")
        if "evidence.role-not-qualified" not in bundle["reason_codes"]:
            fail("not-qualified bundle lacks evidence.role-not-qualified")
    if bundle["outcome"] == "evidence-invalid":
        fail("evidence-invalid is not an accepted offline handoff")
    return bundle


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--selection", type=pathlib.Path, required=True)
    parser.add_argument("--bundle", type=pathlib.Path, required=True)
    parser.add_argument(
        "--artifact-root",
        type=pathlib.Path,
        help="root directory containing downloaded producer artifact ZIPs",
    )
    arguments = parser.parse_args(argv)
    root = arguments.root.resolve()
    try:
        selection = load_document(arguments.selection)
        bundle = load_document(arguments.bundle)
        validate_bundle(bundle, selection, root, arguments.artifact_root)
    except ReleaseEvidenceError as error:
        print(f"release-evidence-bundle: {error}", file=sys.stderr)
        return 1
    print("release-evidence-bundle: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
