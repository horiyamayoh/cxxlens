#!/usr/bin/env python3
"""Small denylist regression test for the retired repository-side evidence layer."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class OperationalEvidenceAbsentTest(unittest.TestCase):
    def test_retired_assets_are_not_in_the_current_tree(self) -> None:
        retired = (
            ".github/workflows/nightly.yml",
            ".github/workflows/autonomy-fast.yml",
            ".github/workflows/autonomy-heavy.yml",
            ".github/workflows/autonomy-release-evaluation.yml",
            ".github/workflows/pr-integration.yml",
            "schemas/cxxlens_ng_release_bundle.yaml",
            "schemas/cxxlens_ng_release_bundle.schema.yaml",
            "schemas/cxxlens_ng_api_development_readiness.yaml",
            "schemas/cxxlens_ng_api_development_readiness.schema.yaml",
            "schemas/cxxlens_ng_install_artifact_manifest.schema.yaml",
            "schemas/cxxlens_ng_release_qualification_report.schema.yaml",
            "schemas/cxxlens_ng_sqlite_store_v3_qualification_report.schema.yaml",
            "schemas/cxxlens_ng_provider_ng1_qualification_report.schema.yaml",
            "tools/quality/run_gate.py",
            "tools/quality/collect_toolchain_provenance.py",
            "tools/quality/install_artifact_manifest.py",
            "tools/quality/check_ng_release_qualification.py",
            "tools/quality/check_ng_provider_ng1_qualification.py",
            "docs/development/implementation-learning",
        )
        for relative in retired:
            with self.subTest(relative=relative):
                self.assertFalse((ROOT / relative).exists())

    def test_workflows_have_no_repository_side_artifact_pipeline(self) -> None:
        for relative in (".github/workflows/quality.yml", ".github/workflows/release.yml"):
            text = (ROOT / relative).read_text(encoding="utf-8")
            for marker in (
                "actions/upload-artifact",
                "actions/download-artifact",
                "run_gate.py",
                "collect_toolchain_provenance.py",
                "qualification-report",
                "evidence-output",
                "timing.json",
                "junit.xml",
            ):
                with self.subTest(workflow=relative, marker=marker):
                    self.assertNotIn(marker, text)

    def test_retired_governance_markers_are_not_active_contract_authority(self) -> None:
        """Keep repository-operation gates from returning through a schema/doc edit.

        Product words such as ``evidence`` and ``receipt`` are intentionally not
        denied here: runtime provenance and safety receipts are part of the
        public product contract.  The denylist is limited to the old
        repository-side review/checkpoint/report identifiers.
        """
        paths = [
            *sorted((ROOT / "docs/design/adr").glob("*.md")),
            ROOT / "docs/design/cxxlens_next_generation_integrated_design_ja.md",
            ROOT / "schemas/cxxlens_ng_sqlite_store_contract.yaml",
            ROOT / "schemas/cxxlens_ng_sqlite_store_contract.schema.yaml",
            ROOT / "schemas/cxxlens_ng_snapshot_store_contract.yaml",
            ROOT / "schemas/cxxlens_ng_snapshot_store_contract.schema.yaml",
            ROOT / "schemas/cxxlens_ng_clang22_materialization_contract.yaml",
            ROOT / "schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml",
            ROOT / "tools/quality/check_ng_snapshot_store_contract.py",
            ROOT / "tools/quality/check_ng_clang22_materialization.py",
            ROOT / "tools/quality/check_ng_sqlite_store_contract.py",
        ]
        retired_markers = (
            "acceptance_review_receipt",
            "acceptance-review-receipt",
            "issuecomment",
            "exact-proposal-commit",
            "independent-review-and-new-frozen-digest",
            "candidate-report-ID",
            "candidate-report-id",
            "qualification-run-id",
            "production-profile-review",
            "report-set-digest",
            "qualification-evidence-only",
            "qualification-only-raw",
            "required-for-bounded-qualification-only",
            "harness-build-toolchain-run-plan",
            "no-release-evidence",
            "Learning checkpoint",
            "learning checkpoint",
            "release_evidence",
            "pending-implementation-and-qualification",
            "independent-bounded-resident-reference-during-qualification",
            "accepted-df-0200-review-and-sqlite-option-a-authority-binding",
            "not-qualified-until-required-evidence",
            "reopen-compaction-pin-and-backend-parity-qualification",
            "same-cap-for-memory-and-sqlite-qualification",
            "publish_and_reopen_qualification",
        )
        for path in paths:
            if path.name == "0106-test-only-development-and-release-policy.md":
                continue
            text = path.read_text(encoding="utf-8")
            for marker in retired_markers:
                with self.subTest(file=path.relative_to(ROOT), marker=marker):
                    self.assertNotIn(marker, text)

    def test_scale_and_bootstrap_do_not_write_operational_reports(self) -> None:
        scale = (ROOT / "tests/install/clang22_materializer_scale_test.py").read_text(
            encoding="utf-8"
        )
        bootstrap = (ROOT / "tools/ci/bootstrap_supply_chain.py").read_text(
            encoding="utf-8"
        )
        action = (ROOT / ".github/actions/setup-ci/action.yml").read_text(
            encoding="utf-8"
        )
        lock = (ROOT / "tools/ci/llvm22-noble.lock.json").read_text(encoding="utf-8")
        for marker in (
            "SCALE_SCHEMA",
            "RUN_MARKER_SCHEMA",
            "failure-marker",
            "preserve-inputs",
            "release_qualification",
            "resource_qualification",
            "write_run_marker",
            "--output $RUNNER_TEMP",
        ):
            with self.subTest(file="scale", marker=marker):
                self.assertNotIn(marker, scale)
        for marker in (
            "write_package_cache_receipt",
            "PACKAGE_CACHE_RECEIPT",
            "verify-artifact",
            "install_artifact_manifest",
        ):
            with self.subTest(file="bootstrap", marker=marker):
                self.assertNotIn(marker, bootstrap)
        self.assertNotIn("CXXLENS_PACKAGE_CACHE_RECEIPT", action)
        for marker in (
            "actions/upload-artifact",
            "actions/download-artifact",
            "provenance_environment",
            "package_cache_receipt",
        ):
            with self.subTest(file="lock", marker=marker):
                self.assertNotIn(marker, lock)

    def test_release_package_waits_for_every_heavy_test(self) -> None:
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        needs = workflow.split("    needs:", 1)[1].split("\n    runs-on", 1)[0]
        for job in (
            "main-tests",
            "contract-and-docs",
            "gcc-public-headers",
            "asan-ubsan",
            "tsan",
            "static-analysis",
            "stress-and-repeat",
            "maximum-scale",
            "real-projects",
        ):
            self.assertIn(job, needs)


if __name__ == "__main__":
    unittest.main()
