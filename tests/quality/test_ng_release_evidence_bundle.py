#!/usr/bin/env python3
"""Positive and fail-closed tests for the #173 handoff boundary."""

from __future__ import annotations

import copy
import hashlib
import json
import pathlib
import tempfile
import unittest
import zipfile

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]

import sys

sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_release_evidence_bundle import (  # noqa: E402
    ReleaseEvidenceError,
    load_document,
    main,
    selection_digest,
    validate_bundle,
    validate_selection,
)


SHA = "a" * 40
TREE = "b" * 40
REPOSITORY_ID = 123456
WORKFLOW_IDS = {"heavy": 101, "nightly": 102, "gr": 103, "terminal_scope": 104}
RUN_IDS = {"heavy": 1001, "nightly": 1002, "gr": 1003, "terminal_scope": 1004}
ARTIFACT_IDS = {
    "heavy": (2001, 2002),
    "nightly": (2003,),
    "gr": (2004,),
    "terminal_scope": (2005,),
}
ARCHIVE_TEMP_DIR = tempfile.TemporaryDirectory(prefix="cxxlens-release-evidence-")
ARCHIVE_ROOT = pathlib.Path(ARCHIVE_TEMP_DIR.name)


def selection() -> dict:
    return {
        "schema": "cxxlens.ng-release-evidence-selection.v1",
        "document_version": "1.0.0",
        "contract_id": "development.authenticated-release-evidence-handoff.v1",
        "selection_id": "candidate-a-attempt-1",
        "selected_at": "2026-08-21T03:00:00Z",
        "candidate": {
            "repository": "horiyamayoh/cxxlens",
            "repository_id": REPOSITORY_ID,
            "branch": "main",
            "sha": SHA,
            "tree": TREE,
        },
        "roles": {
            "heavy": {
                "owner_issue": "#173",
                "workflow_path": ".github/workflows/autonomy-heavy.yml",
                "workflow_id": WORKFLOW_IDS["heavy"],
                "event": "workflow_run",
                "run_id": RUN_IDS["heavy"],
                "run_attempt": 1,
                "artifact_ids": list(ARTIFACT_IDS["heavy"]),
            },
            "nightly": {
                "owner_issue": "#173",
                "workflow_path": ".github/workflows/nightly.yml",
                "workflow_id": WORKFLOW_IDS["nightly"],
                "event": "schedule",
                "run_id": RUN_IDS["nightly"],
                "run_attempt": 1,
                "artifact_ids": list(ARTIFACT_IDS["nightly"]),
            },
            "gr": {
                "owner_issue": "#167",
                "workflow_path": ".github/workflows/autonomy-gr.yml",
                "workflow_id": WORKFLOW_IDS["gr"],
                "event": "workflow_dispatch",
                "run_id": RUN_IDS["gr"],
                "run_attempt": 1,
                "artifact_ids": list(ARTIFACT_IDS["gr"]),
            },
            "terminal_scope": {
                "owner_issue": "#179",
                "workflow_path": ".github/workflows/autonomy-production-scope.yml",
                "workflow_id": WORKFLOW_IDS["terminal_scope"],
                "event": "workflow_dispatch",
                "run_id": RUN_IDS["terminal_scope"],
                "run_attempt": 1,
                "artifact_ids": list(ARTIFACT_IDS["terminal_scope"]),
            },
        },
    }


def _run(role: str) -> dict:
    event = {
        "heavy": "workflow_run",
        "nightly": "schedule",
        "gr": "workflow_dispatch",
        "terminal_scope": "workflow_dispatch",
    }[role]
    path = {
        "heavy": ".github/workflows/autonomy-heavy.yml",
        "nightly": ".github/workflows/nightly.yml",
        "gr": ".github/workflows/autonomy-gr.yml",
        "terminal_scope": ".github/workflows/autonomy-production-scope.yml",
    }[role]
    run_id = RUN_IDS[role]
    return {
        "repository_id": REPOSITORY_ID,
        "workflow_id": WORKFLOW_IDS[role],
        "run_id": run_id,
        "run_attempt": 1,
        "workflow_path": path,
        "event": event,
        "status": "completed",
        "conclusion": "success",
        "head_sha": SHA,
        "head_repository_id": REPOSITORY_ID,
        "api_url": f"https://api.github.com/repos/horiyamayoh/cxxlens/actions/runs/{run_id}",
        "html_url": f"https://github.com/horiyamayoh/cxxlens/actions/runs/{run_id}",
        "created_at": "2026-08-21T03:00:00Z",
        "updated_at": "2026-08-21T03:10:00Z",
    }


def _file(path: str, schema: str, result: str, index: int) -> dict:
    marker = format(index % 16, "x")
    return {
        "path": path,
        "schema": schema,
        "digest": "sha256:" + marker * 64,
        "byte_count": 128 + index,
        "revision": SHA,
        "tree": TREE,
        "result": result,
    }


def _artifact(
    role: str,
    kind: str,
    artifact_id: int,
    index: int,
    selected_digest: str | None = None,
) -> dict:
    if role == "heavy" and kind == "quality-report":
        name = f"cxxlens-autonomy-heavy-provisional-{SHA}"
        files = [
            _file("cxxlens-autonomy-heavy.json", "cxxlens.quality-run-report.v1", "passed", index),
            _file("cxxlens-autonomy-heavy.junit.xml", "junit.xml", "passed", index + 1),
        ]
    elif role == "heavy":
        name = f"cxxlens-autonomy-heavy-postflight-{SHA}"
        files = [
            _file(
                "cxxlens-autonomy-heavy-postflight.json",
                "cxxlens.autonomy-heavy-freshness.v1",
                "current",
                index,
            )
        ]
    elif role == "nightly":
        name = f"cxxlens-nightly-evidence-{SHA}"
        files = [
            _file(
                "nightly-evidence-set.json",
                "cxxlens.quality-evidence-set.v1",
                "passed",
                index,
            )
        ]
    elif role == "gr":
        name = f"cxxlens-ng-release-qualification-{SHA}"
        files = [
            _file(
                "cxxlens-ng-release-qualification-report.json",
                "cxxlens.ng-release-qualification-report.v1",
                "qualified",
                index,
            ),
            _file(
                "cxxlens-ng-release-owner-handoff.json",
                "cxxlens.ng-release-owner-handoff.v1",
                "qualified",
                index + 1,
            ),
        ]
    else:
        name = f"cxxlens-ng-production-scope-closure-{SHA}"
        files = [
            _file(
                "cxxlens-ng-production-scope-closure-report.json",
                "cxxlens.ng-production-scope-closure-report.v1",
                "qualified",
                index,
            ),
            _file(
                "cxxlens-ng-release-owner-handoff.json",
                "cxxlens.ng-release-owner-handoff.v1",
                "qualified",
                index + 1,
            ),
        ]
    if role in {"gr", "terminal_scope"} and selected_digest is None:
        raise AssertionError("owner fixture requires the selection digest")
    archive_path = ARCHIVE_ROOT / f"{artifact_id}.zip"
    contents: dict[str, bytes] = {}
    for row in files:
        if row["path"] == "cxxlens-ng-release-owner-handoff.json":
            continue
        contents[row["path"]] = f"fixture:{artifact_id}:{row['path']}\n".encode("utf-8")
    if role in {"gr", "terminal_scope"}:
        report_path = files[0]["path"]
        report_digest = "sha256:" + hashlib.sha256(contents[report_path]).hexdigest()
        owner_issue = "#167" if role == "gr" else "#179"
        owner_workflow = (
            ".github/workflows/autonomy-gr.yml"
            if role == "gr"
            else ".github/workflows/autonomy-production-scope.yml"
        )
        report_schema = (
            "cxxlens.ng-release-qualification-report.v1"
            if role == "gr"
            else "cxxlens.ng-production-scope-closure-report.v1"
        )
        handoff = {
            "schema": "cxxlens.ng-release-owner-handoff.v1",
            "document_version": "1.0.0",
            "role": role,
            "owner_issue": owner_issue,
            "workflow_path": owner_workflow,
            "candidate": {"sha": SHA, "tree": TREE},
            "input_selection_digest": selected_digest,
            "source": {
                "repository_id": REPOSITORY_ID,
                "workflow_id": 999,
                "workflow_path": ".github/workflows/quality.yml",
                "run_id": 3000 + artifact_id,
                "run_attempt": 1,
                "artifact_id": 4000 + artifact_id,
                "artifact_name": name,
                "artifact_digest": "sha256:" + "1" * 64,
            },
            "report": {
                "path": report_path,
                "schema": report_schema,
                "digest": report_digest,
                "outcome": "qualified",
            },
        }
        contents["cxxlens-ng-release-owner-handoff.json"] = (
            json.dumps(handoff, sort_keys=True).encode("utf-8") + b"\n"
        )
    with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for row in files:
            content = contents[row["path"]]
            row["byte_count"] = len(content)
            row["digest"] = "sha256:" + hashlib.sha256(content).hexdigest()
            archive.writestr(row["path"], content)
    archive_digest = "sha256:" + hashlib.sha256(archive_path.read_bytes()).hexdigest()
    return {
        "kind": kind,
        "artifact_id": artifact_id,
        "name": name,
        "api_url": (
            "https://api.github.com/repos/horiyamayoh/cxxlens/actions/artifacts/"
            f"{artifact_id}"
        ),
        "archive_download_url": (
            "https://api.github.com/repos/horiyamayoh/cxxlens/actions/artifacts/"
            f"{artifact_id}/zip"
        ),
        "expired": False,
        "size_bytes": archive_path.stat().st_size,
        "digest": archive_digest,
        "archive_digest": archive_digest,
        "download_source": "producer-run-artifact-id-direct-download",
        "downloaded_archive": {"path": archive_path.name},
        "files": files,
    }


def bundle(selection_document: dict | None = None) -> dict:
    selected = selection_document or selection()
    selected_digest = selection_digest(selected)
    roles = {
        "heavy": {
            "owner_issue": "#173",
            "workflow_path": ".github/workflows/autonomy-heavy.yml",
            "workflow_id": WORKFLOW_IDS["heavy"],
            "event": "workflow_run",
            "run": _run("heavy"),
            "artifacts": [
                _artifact("heavy", "quality-report", 2001, 1),
                _artifact("heavy", "postflight", 2002, 3),
            ],
            "disposition": "qualified",
        },
        "nightly": {
            "owner_issue": "#173",
            "workflow_path": ".github/workflows/nightly.yml",
            "workflow_id": WORKFLOW_IDS["nightly"],
            "event": "schedule",
            "run": _run("nightly"),
            "artifacts": [_artifact("nightly", "evidence-set", 2003, 5)],
            "disposition": "qualified",
        },
        "gr": {
            "owner_issue": "#167",
            "workflow_path": ".github/workflows/autonomy-gr.yml",
            "workflow_id": WORKFLOW_IDS["gr"],
            "event": "workflow_dispatch",
            "run": _run("gr"),
            "artifacts": [_artifact("gr", "gr-report", 2004, 6, selected_digest)],
            "disposition": "qualified",
            "input_selection_digest": selected_digest,
        },
        "terminal_scope": {
            "owner_issue": "#179",
            "workflow_path": ".github/workflows/autonomy-production-scope.yml",
            "workflow_id": WORKFLOW_IDS["terminal_scope"],
            "event": "workflow_dispatch",
            "run": _run("terminal_scope"),
            "artifacts": [
                _artifact(
                    "terminal_scope",
                    "terminal-scope-report",
                    2005,
                    7,
                    selected_digest,
                )
            ],
            "disposition": "qualified",
            "input_selection_digest": selected_digest,
        },
    }
    return {
        "schema": "cxxlens.ng-release-evidence-bundle.v1",
        "document_version": "1.0.0",
        "contract_id": "development.authenticated-release-evidence-handoff.v1",
        "bundle_id": "bundle-candidate-a-attempt-1",
        "generated_at": "2026-08-21T03:15:00Z",
        "authentication": {
            "provider": "github-actions",
            "metadata_source": "github-actions-rest-api",
            "attempt_policy": "first-attempt-only",
            "download_policy": "producer-run-artifact-id-direct-download",
        },
        "selection_digest": selected_digest,
        "candidate": copy.deepcopy(selected["candidate"]),
        "roles": roles,
        "outcome": "qualified-inputs-ready",
        "reason_codes": [],
        "gr_issued": False,
        "production_qualification": "not-claimed",
    }


class ReleaseEvidenceBundleTest(unittest.TestCase):
    def test_exact_attempt_one_bundle_is_valid(self) -> None:
        selected = selection()
        validate_selection(selected, ROOT)
        self.assertEqual(
            validate_bundle(bundle(selected), selected, ROOT, ARCHIVE_ROOT)["outcome"],
            "qualified-inputs-ready",
        )

    def test_attempt_two_is_rejected_fail_closed(self) -> None:
        selected = selection()
        candidate = bundle(selected)
        candidate["roles"]["heavy"]["run"]["run_attempt"] = 2
        with self.assertRaisesRegex(ReleaseEvidenceError, "schema validation failed"):
            validate_bundle(candidate, selected, ROOT, ARCHIVE_ROOT)

    def test_nightly_workflow_call_is_rejected(self) -> None:
        selected = selection()
        selected["roles"]["nightly"]["event"] = "workflow_call"
        with self.assertRaisesRegex(ReleaseEvidenceError, "schema validation failed"):
            validate_selection(selected, ROOT)

    def test_latest_run_discovery_marker_is_rejected(self) -> None:
        selected = selection()
        selected["roles"]["nightly"]["latest_run"] = True
        with self.assertRaisesRegex(ReleaseEvidenceError, "schema validation failed"):
            validate_selection(selected, ROOT)

    def test_run_id_cannot_be_reused_across_roles(self) -> None:
        selected = selection()
        selected["roles"]["terminal_scope"]["run_id"] = RUN_IDS["gr"]
        with self.assertRaisesRegex(ReleaseEvidenceError, "run ID is selected"):
            validate_selection(selected, ROOT)

    def test_workflow_id_cannot_be_reused_across_roles(self) -> None:
        selected = selection()
        selected["roles"]["terminal_scope"]["workflow_id"] = WORKFLOW_IDS["gr"]
        with self.assertRaisesRegex(ReleaseEvidenceError, "workflow ID is selected"):
            validate_selection(selected, ROOT)

    def test_gr_and_terminal_scope_receipts_bind_the_complete_selection(self) -> None:
        selected = selection()
        candidate = bundle(selected)
        selected["selected_at"] = "2026-08-21T03:01:00Z"
        candidate["selection_digest"] = selection_digest(selected)
        with self.assertRaisesRegex(
            ReleaseEvidenceError, "gr input selection digest differs"
        ):
            validate_bundle(candidate, selected, ROOT, ARCHIVE_ROOT)

    def test_artifact_archive_digest_must_match_api_digest(self) -> None:
        selected = selection()
        candidate = bundle(selected)
        candidate["roles"]["nightly"]["artifacts"][0]["archive_digest"] = "sha256:" + "e" * 64
        with self.assertRaisesRegex(ReleaseEvidenceError, "archive digest"):
            validate_bundle(candidate, selected, ROOT, ARCHIVE_ROOT)

    def test_artifact_id_must_be_explicitly_selected(self) -> None:
        selected = selection()
        candidate = bundle(selected)
        candidate["roles"]["gr"]["artifacts"][0]["artifact_id"] = 2999
        with self.assertRaisesRegex(ReleaseEvidenceError, "not explicitly selected"):
            validate_bundle(candidate, selected, ROOT, ARCHIVE_ROOT)

    def test_payload_tree_drift_is_rejected(self) -> None:
        selected = selection()
        candidate = bundle(selected)
        candidate["roles"]["terminal_scope"]["artifacts"][0]["files"][0]["tree"] = "c" * 40
        with self.assertRaisesRegex(ReleaseEvidenceError, "candidate revision/tree"):
            validate_bundle(candidate, selected, ROOT, ARCHIVE_ROOT)

    def test_downloaded_archive_tamper_is_rejected(self) -> None:
        selected = selection()
        candidate = bundle(selected)
        path = ARCHIVE_ROOT / "2003.zip"
        path.write_bytes(path.read_bytes() + b"tampered")
        candidate["roles"]["nightly"]["artifacts"][0]["size_bytes"] = path.stat().st_size
        with self.assertRaisesRegex(ReleaseEvidenceError, "downloaded archive digest"):
            validate_bundle(candidate, selected, ROOT, ARCHIVE_ROOT)

    def test_one_archive_cannot_attest_two_artifact_ids(self) -> None:
        selected = selection()
        candidate = bundle(selected)
        candidate["roles"]["nightly"]["artifacts"][0]["downloaded_archive"]["path"] = "2001.zip"
        with self.assertRaisesRegex(ReleaseEvidenceError, "path is reused"):
            validate_bundle(candidate, selected, ROOT, ARCHIVE_ROOT)

    def test_not_qualified_is_typed_and_never_issues_gr(self) -> None:
        selected = selection()
        candidate = bundle(selected)
        candidate["roles"]["gr"]["artifacts"][0]["files"][0]["result"] = "not-qualified"
        candidate["roles"]["gr"]["disposition"] = "not-qualified"
        candidate["outcome"] = "not-qualified"
        candidate["reason_codes"] = ["evidence.role-not-qualified"]
        validate_bundle(candidate, selected, ROOT, ARCHIVE_ROOT)
        self.assertFalse(candidate["gr_issued"])
        self.assertEqual(candidate["production_qualification"], "not-claimed")

    def test_stale_postflight_is_superseded(self) -> None:
        selected = selection()
        candidate = bundle(selected)
        candidate["roles"]["heavy"]["artifacts"][1]["files"][0]["result"] = "superseded"
        candidate["roles"]["heavy"]["disposition"] = "not-qualified"
        candidate["outcome"] = "superseded"
        candidate["reason_codes"] = ["evidence.main-superseded"]
        validate_bundle(candidate, selected, ROOT, ARCHIVE_ROOT)

    def test_json_duplicate_members_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "duplicate.json"
            path.write_text('{"schema":"one","schema":"two"}\n', encoding="utf-8")
            with self.assertRaisesRegex(ReleaseEvidenceError, "duplicate JSON"):
                load_document(path)

    def test_cli_accepts_explicit_selection_and_bundle(self) -> None:
        selected = selection()
        candidate = bundle(selected)
        with tempfile.TemporaryDirectory() as directory:
            directory_path = pathlib.Path(directory)
            selection_path = directory_path / "selection.json"
            bundle_path = directory_path / "bundle.json"
            selection_path.write_text(json.dumps(selected), encoding="utf-8")
            bundle_path.write_text(json.dumps(candidate), encoding="utf-8")
            self.assertEqual(
                main(
                    [
                        "check",
                        "--root",
                        str(ROOT),
                        "--selection",
                        str(selection_path),
                        "--bundle",
                        str(bundle_path),
                        "--artifact-root",
                        str(ARCHIVE_ROOT),
                    ]
                ),
                0,
            )


if __name__ == "__main__":
    unittest.main()
