#!/usr/bin/env python3
"""Fail-closed tests for the connected #167/#179 owner handoff checker."""

from __future__ import annotations

import json
import pathlib
import tempfile
import unittest
import zipfile

import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_release_owner_handoff import (  # noqa: E402
    OwnerHandoffError,
    check_and_extract,
)


SHA = "a" * 40
TREE = "b" * 40


def _run(path: str = ".github/workflows/quality.yml") -> dict:
    return {
        "id": 1001,
        "workflow_id": 1002,
        "path": path,
        "event": "push",
        "status": "completed",
        "conclusion": "success",
        "head_sha": SHA,
        "head_branch": "main",
        "run_attempt": 1,
        "repository": {"id": 1234},
        "head_repository": {"id": 1234},
        "created_at": "2026-08-21T03:00:00Z",
        "updated_at": "2026-08-21T03:10:00Z",
    }


def _artifact(name: str) -> dict:
    return {
        "id": 2001,
        "name": name,
        "expired": False,
        "size_in_bytes": 1,
        "digest": "sha256:" + "c" * 64,
        "workflow_run": {"id": 1001},
        "archive_download_url": "https://api.github.com/repos/example/cxxlens/actions/artifacts/2001/zip",
    }


class ReleaseOwnerHandoffTest(unittest.TestCase):
    def test_source_run_must_be_quality_producer(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            run = root / "run.json"
            workflow = root / "workflow.json"
            artifact = root / "artifact.json"
            archive = root / "artifact.zip"
            run.write_text(json.dumps(_run(".github/workflows/autonomy-gr.yml")), encoding="utf-8")
            workflow.write_text(json.dumps({"id": 1002, "path": ".github/workflows/quality.yml", "state": "active"}), encoding="utf-8")
            artifact.write_text(json.dumps(_artifact(f"cxxlens-ng-release-qualification-{SHA}")), encoding="utf-8")
            archive.write_bytes(b"not-used")
            with self.assertRaisesRegex(OwnerHandoffError, "canonical quality producer"):
                check_and_extract(
                    root=ROOT,
                    role="gr",
                    candidate_sha=SHA,
                    candidate_tree=TREE,
                    selection_digest="sha256:" + "d" * 64,
                    source_run_path=run,
                    source_workflow_path=workflow,
                    source_artifact_path=artifact,
                    archive_path=archive,
                    report_output=root / "report.json",
                    handoff_output=root / "handoff.json",
                )

    def test_source_artifact_digest_is_required_before_report_acceptance(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            run = root / "run.json"
            workflow = root / "workflow.json"
            artifact = root / "artifact.json"
            archive = root / "artifact.zip"
            run.write_text(json.dumps(_run()), encoding="utf-8")
            workflow.write_text(json.dumps({"id": 1002, "path": ".github/workflows/quality.yml", "state": "active"}), encoding="utf-8")
            metadata = _artifact(f"cxxlens-ng-release-qualification-{SHA}")
            metadata["digest"] = None
            artifact.write_text(json.dumps(metadata), encoding="utf-8")
            with zipfile.ZipFile(archive, "w") as value:
                value.writestr("cxxlens-ng-release-qualification-report.json", b"{}")
            with self.assertRaisesRegex(OwnerHandoffError, "digest is unavailable"):
                check_and_extract(
                    root=ROOT,
                    role="gr",
                    candidate_sha=SHA,
                    candidate_tree=TREE,
                    selection_digest="sha256:" + "d" * 64,
                    source_run_path=run,
                    source_workflow_path=workflow,
                    source_artifact_path=artifact,
                    archive_path=archive,
                    report_output=root / "report.json",
                    handoff_output=root / "handoff.json",
                )


if __name__ == "__main__":
    unittest.main()
