#!/usr/bin/env python3
"""Fail-closed tests for distribution 1.0 GR qualification."""

from __future__ import annotations

import copy
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_release_qualification as release  # noqa: E402


class NgReleaseQualificationTests(unittest.TestCase):
    def make_callable_evidence(
        self, evidence: pathlib.Path
    ) -> tuple[dict, dict, dict, pathlib.Path, pathlib.Path]:
        manifest = release.load(ROOT / release.MANIFEST)
        inventory_path = ROOT / release.CALLABLE_INVENTORY
        inventory = release.load(inventory_path)
        git = {
            "revision": "1" * 40,
            "tree": "2" * 40,
            "branch": "main",
            "clean": True,
        }
        run_url = "https://github.com/horiyamayoh/cxxlens/actions/runs/1"
        doxygen_digest = "sha256:" + "4" * 64
        review_path = (
            evidence / manifest["documentation"]["public_callable_review_filename"]
        )
        review_path.write_text(
            release.callable_inventory.review_markdown(
                inventory, git, run_url, doxygen_digest
            ),
            encoding="utf-8",
        )
        inventory_binding = {
            "path": release.CALLABLE_INVENTORY.as_posix(),
            "file_digest": release.digest(inventory_path),
            "semantic_digest": release.callable_inventory.inventory_digest(inventory),
            "callable_count": len(inventory["callables"]),
        }
        report = {
            "schema": "cxxlens.ng-public-callable-inventory-report.v1",
            "result": "passed",
            "generated_at": "2026-07-19T00:00:00Z",
            "run_url": run_url,
            "git": git,
            "inventory": inventory_binding,
            "extractor": inventory["extractor"],
            "headers": {"count": 1, "digest": "sha256:" + "3" * 64},
            "doxygen": {
                "count": len(inventory["callables"]),
                "digest": doxygen_digest,
            },
            "review": {
                "path": review_path.name,
                "digest": release.digest(review_path),
            },
        }
        report_path = (
            evidence / manifest["documentation"]["public_callable_report_filename"]
        )
        report_path.write_text(
            json.dumps(report, ensure_ascii=False, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        readiness = {
            "public_callable_inventory": {
                "path": report_path.name,
                "report_digest": release.digest(report_path),
                "inventory_file_digest": inventory_binding["file_digest"],
                "inventory_semantic_digest": inventory_binding["semantic_digest"],
                "review_path": review_path.name,
                "review_digest": report["review"]["digest"],
                "callable_count": inventory_binding["callable_count"],
                "doxygen_count": report["doxygen"]["count"],
                "result": report["result"],
                "revision": git["revision"],
                "tree": git["tree"],
            }
        }
        return manifest, readiness, git, report_path, review_path

    def test_repository_contract_is_implemented(self) -> None:
        manifest = release.validate_documents(ROOT)
        self.assertEqual(manifest["binding"]["package_version"], "1.0.0")
        self.assertEqual(manifest["prerequisites"]["gates"], [f"gate.g{i}" for i in range(6)])
        self.assertEqual(
            manifest["prerequisites"]["public_callable_evidence"],
            "cxxlens.ng-public-callable-inventory-report.v1",
        )

    def test_pending_static_tuple_cannot_grant_production(self) -> None:
        original = release.load
        support = original(ROOT / release.SUPPORT)
        changed = copy.deepcopy(support)
        changed["entries"][0]["status"] = "production-supported"

        def load(path: pathlib.Path):
            return changed if path == ROOT / release.SUPPORT else original(path)

        with mock.patch.object(release, "load", side_effect=load):
            with self.assertRaisesRegex(release.ReleaseQualificationError, "static support matrix"):
                release.validate_documents(ROOT)

    def test_g5_cannot_be_bypassed(self) -> None:
        original = release.load
        acceptance = original(ROOT / release.ACCEPTANCE)
        changed = copy.deepcopy(acceptance)
        next(row for row in changed["entries"] if row["id"] == "gate.g5")["status"] = "deferred"

        def load(path: pathlib.Path):
            return changed if path == ROOT / release.ACCEPTANCE else original(path)

        with mock.patch.object(release, "load", side_effect=load):
            with self.assertRaisesRegex(release.ReleaseQualificationError, "G0-G5"):
                release.validate_documents(ROOT)

    def test_install_manifest_digest_is_fail_closed(self) -> None:
        self.assertNotEqual(
            release.canonical_digest({"files": []}),
            release.canonical_digest({"files": [{"path": "forged"}]}),
        )

    def test_install_junit_is_resolved_from_downloaded_artifact_root(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            artifact = evidence / "cxxlens-install-static-revision"
            (artifact / "tests" / "install-consumer").mkdir(parents=True)
            junit = artifact / "ctest-install-OFF.xml"
            junit.touch()

            self.assertEqual(release.find_one(evidence, "ctest-install-OFF.xml"), junit)

    def test_public_callable_json_and_markdown_are_exactly_readiness_bound(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, readiness, git, _, _ = self.make_callable_evidence(evidence)
            value = release.verify_public_callable_evidence(
                ROOT, manifest, evidence, git, readiness
            )
            self.assertEqual(
                value["report"]["digest"],
                readiness["public_callable_inventory"]["report_digest"],
            )
            self.assertEqual(
                value["doxygen"]["count"], value["inventory"]["callable_count"]
            )

    def test_public_callable_json_digest_must_match_readiness(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, readiness, git, _, _ = self.make_callable_evidence(evidence)
            readiness["public_callable_inventory"]["report_digest"] = (
                "sha256:" + "0" * 64
            )
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "exact JSON artifact digest"
            ):
                release.verify_public_callable_evidence(
                    ROOT, manifest, evidence, git, readiness
                )

    def test_public_callable_doxygen_count_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, readiness, git, report_path, _ = self.make_callable_evidence(
                evidence
            )
            report = release.load(report_path)
            report["doxygen"]["count"] -= 1
            report_path.write_text(
                json.dumps(report, ensure_ascii=False, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "Doxygen count"
            ):
                release.verify_public_callable_evidence(
                    ROOT, manifest, evidence, git, readiness
                )

    def test_public_callable_markdown_digest_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, readiness, git, _, review_path = self.make_callable_evidence(
                evidence
            )
            review_path.write_text(
                review_path.read_text(encoding="utf-8") + "forged review\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "Markdown review digest"
            ):
                release.verify_public_callable_evidence(
                    ROOT, manifest, evidence, git, readiness
                )

    def test_duplicate_public_callable_artifact_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, readiness, git, report_path, _ = self.make_callable_evidence(
                evidence
            )
            duplicate = evidence / "duplicate" / report_path.name
            duplicate.parent.mkdir()
            duplicate.write_bytes(report_path.read_bytes())
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "expected exactly one"
            ):
                release.verify_public_callable_evidence(
                    ROOT, manifest, evidence, git, readiness
                )


if __name__ == "__main__":
    unittest.main()
