#!/usr/bin/env python3
"""Offline negative tests for the connected release-evidence collector."""

from __future__ import annotations

import hashlib
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from collect_ng_release_evidence import (  # noqa: E402
    ReleaseEvidenceCollectionError,
    authenticate_candidate,
    authenticate_workflow,
    digest_file,
    normalize_run,
    validate_artifact_metadata,
)


SHA = "a" * 40
TREE = "b" * 40
REPOSITORY_ID = 1234


def _selected() -> dict:
    return {
        "workflow_id": 101,
        "workflow_path": ".github/workflows/autonomy-gr.yml",
        "event": "workflow_dispatch",
    }


def _run() -> dict:
    return {
        "id": 1001,
        "workflow_id": 101,
        "run_attempt": 1,
        "path": ".github/workflows/autonomy-gr.yml",
        "event": "workflow_dispatch",
        "head_sha": SHA,
        "head_branch": "main",
        "status": "completed",
        "conclusion": "success",
        "repository": {"id": REPOSITORY_ID},
        "head_repository": {"id": REPOSITORY_ID},
        "url": "https://api.github.com/repos/example/cxxlens/actions/runs/1001",
        "html_url": "https://github.com/example/cxxlens/actions/runs/1001",
        "created_at": "2026-08-21T03:00:00Z",
        "updated_at": "2026-08-21T03:10:00Z",
    }


class ReleaseEvidenceCollectorTest(unittest.TestCase):
    def test_workflow_path_and_immutable_id_are_bound(self) -> None:
        authenticate_workflow(
            {"id": 101, "path": ".github/workflows/autonomy-gr.yml", "state": "active"},
            _selected(),
        )
        with self.assertRaisesRegex(ReleaseEvidenceCollectionError, "workflow identity"):
            authenticate_workflow(
                {"id": 999, "path": ".github/workflows/autonomy-gr.yml", "state": "active"},
                _selected(),
            )

    def test_candidate_tree_is_not_claimant_selected(self) -> None:
        authenticate_candidate(
            {"sha": SHA, "commit": {"tree": {"sha": TREE}}}, SHA, TREE
        )
        with self.assertRaisesRegex(ReleaseEvidenceCollectionError, "tree differs"):
            authenticate_candidate(
                {"sha": SHA, "commit": {"tree": {"sha": "c" * 40}}}, SHA, TREE
            )

    def test_run_repository_and_head_repository_bind_to_candidate(self) -> None:
        normalize_run(_run(), _selected(), SHA, REPOSITORY_ID)
        foreign = _run()
        foreign["head_repository"] = {"id": REPOSITORY_ID + 1}
        with self.assertRaisesRegex(ReleaseEvidenceCollectionError, "repository identity"):
            normalize_run(foreign, _selected(), SHA, REPOSITORY_ID)

    def test_artifact_metadata_is_authenticated_before_download(self) -> None:
        metadata = {
            "id": 2001,
            "name": f"cxxlens-ng-release-qualification-{SHA}",
            "expired": False,
            "workflow_run": {"id": 1001},
            "size_in_bytes": 1,
            "digest": "sha256:" + "c" * 64,
            "url": "https://api.github.com/repos/example/cxxlens/actions/artifacts/2001",
            "archive_download_url": (
                "https://api.github.com/repos/example/cxxlens/actions/artifacts/2001/zip"
            ),
        }
        validate_artifact_metadata(
            metadata,
            2001,
            f"cxxlens-ng-release-qualification-{SHA}",
            1001,
        )
        for key, value, message in (
            ("expired", True, "expired"),
            ("name", "wrong", "name differs"),
            ("size_in_bytes", (1 << 30) + 1, "bounded range"),
            ("url", "https://api.github.com/actions/artifacts/999", "API URL"),
            ("archive_download_url", "https://api.github.com/actions/artifacts/999/zip", "download URL"),
        ):
            mutated = dict(metadata)
            mutated[key] = value
            with self.subTest(key=key):
                with self.assertRaisesRegex(ReleaseEvidenceCollectionError, message):
                    validate_artifact_metadata(
                        mutated,
                        2001,
                        f"cxxlens-ng-release-qualification-{SHA}",
                        1001,
                    )

    def test_archive_digest_is_streamed_and_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "artifact.zip"
            path.write_bytes(b"bounded")
            digest, size = digest_file(path)
            self.assertEqual(size, 7)
            self.assertEqual(digest, "sha256:" + hashlib.sha256(b"bounded").hexdigest())
            with mock.patch("collect_ng_release_evidence.MAX_ARTIFACT_BYTES", 4):
                with self.assertRaisesRegex(ReleaseEvidenceCollectionError, "exceeds"):
                    digest_file(path)


if __name__ == "__main__":
    unittest.main()
