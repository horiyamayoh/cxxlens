#!/usr/bin/env python3
"""Positive and fail-closed tests for the composed Wave 0/#291 contract."""

from __future__ import annotations

import copy
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import types
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
BASELINE_PATH = "tests/quality/test_ng_api_development_readiness_wave0_baseline.py"
BASELINE_DIGEST = "sha256:23db2ab6ae3ae011d199cf25e59685caff35a44675a956b0911d7266af012b75"
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_api_development_readiness as readiness  # noqa: E402


def _load_baseline_tests() -> types.ModuleType:
    baseline_path = ROOT / BASELINE_PATH
    if not baseline_path.is_file():
        raise RuntimeError(f"the tracked readiness test corpus is unavailable: {BASELINE_PATH}")
    actual_digest = "sha256:" + hashlib.sha256(baseline_path.read_bytes()).hexdigest()
    if actual_digest != BASELINE_DIGEST:
        raise RuntimeError("the tracked readiness test corpus digest differs")
    module = types.ModuleType("_cxxlens_readiness_tests_baseline")
    module.__file__ = str(baseline_path)
    module.__package__ = None
    exec(compile(baseline_path.read_text(encoding="utf-8"), module.__file__, "exec"), module.__dict__)
    return module


_baseline = _load_baseline_tests()


class NgApiDevelopmentReadinessTest(_baseline.NgApiDevelopmentReadinessTest):
    """Retain the complete previous corpus and replace only superseded CI tests."""


    def test_authorization_protected_main_workflow_is_required(self) -> None:
        "The composed contract requires the direct-main workflow marker."
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / readiness.DIRECT_MAIN_GOAL_CONTRACT
            goal.write_text(
                goal.read_text(encoding="utf-8").replace(
                    "`direct-main: issue-scoped-fast-forward-push-post-push-integration`",
                    "direct-main marker removed",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(readiness.ReadinessError, "direct-main"):
                readiness.validate_documents(root)

    def test_authorization_direct_main_prohibition_is_required(self) -> None:
        "The composed contract requires PRs to remain optional."
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / readiness.DIRECT_MAIN_GOAL_CONTRACT
            goal.write_text(
                goal.read_text(encoding="utf-8").replace(
                    "`pull-request: optional-for-risk-review-or-external-contribution`",
                    "pull-request marker removed",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(readiness.ReadinessError, "pull-request"):
                readiness.validate_documents(root)

    def test_legacy_direct_main_workflow_is_rejected(self) -> None:
        "The composed contract rejects restoration of the old PR-only marker."
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / readiness.DIRECT_MAIN_GOAL_CONTRACT
            goal.write_text(
                goal.read_text(encoding="utf-8")
                + "\n`protected-main: "
                "unit-branch-pr-exact-head-review-merge-exact-merged-main`\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(readiness.ReadinessError, "protected-main"):
                readiness.validate_documents(root)

    def complete_evidence(
        self, evidence_dir: pathlib.Path, git_state: dict[str, object]
    ) -> pathlib.Path:
        result = super().complete_evidence(evidence_dir, git_state)
        packet = readiness.build_agent_context_packet(
            ROOT,
            self.manifest,
            str(git_state["revision"]),
            str(git_state["tree"]),
        )
        self.write_json(evidence_dir / readiness.PACKET_JSON_NAME, packet)
        (evidence_dir / readiness.PACKET_MARKDOWN_NAME).write_text(
            readiness.render_agent_context_markdown(packet), encoding="utf-8"
        )
        return result

    def test_nightly_main_push_trigger_is_required(self) -> None:
        with self.subTest(contract="workflow-call"):
            import tempfile

            with tempfile.TemporaryDirectory() as temporary:
                root = self.copied_root(temporary)
                workflow = root / ".github/workflows/nightly.yml"
                text = workflow.read_text(encoding="utf-8")
                marker = "  workflow_call:\n"
                self.assertIn(marker, text)
                workflow.write_text(text.replace(marker, "", 1), encoding="utf-8")
                with self.assertRaisesRegex(
                    readiness.ReadinessError, "reusable, scheduled, and manually"
                ):
                    readiness.validate_documents(root)

    def test_nightly_rolling_concurrency_is_required(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/nightly.yml"
            text = workflow.read_text(encoding="utf-8")
            marker = "  cancel-in-progress: false\n"
            self.assertIn(marker, text)
            workflow.write_text(
                text.replace(marker, "  cancel-in-progress: true\n", 1),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                readiness.ReadinessError, "retain every exact-SHA"
            ):
                readiness.validate_documents(root)

    def test_release_evaluation_waits_for_push_nightly(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/quality.yml"
            text = workflow.read_text(encoding="utf-8")
            self.assertNotIn("sleep 30", text)
            self.assertNotIn("for attempt in", text)
            marker = readiness._NEW_RELEASE_NEEDS
            self.assertIn(marker, text)
            workflow.write_text(
                text.replace(marker, readiness._LEGACY_RELEASE_NEEDS, 1),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                readiness.ReadinessError, "depend on exact-SHA stress completion"
            ):
                readiness.validate_documents(root)

    def test_release_evaluation_requires_sqlite_qualification(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/quality.yml"
            text = workflow.read_text(encoding="utf-8")
            workflow.write_text(
                text.replace(
                    readiness._NEW_RELEASE_NEEDS,
                    "needs: [nightly-quality, g5-qualification]",
                    1,
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                readiness.ReadinessError, "depend on exact-SHA stress completion"
            ):
                readiness.validate_documents(root)

    def test_quality_workflow_requires_unrestricted_push_trigger(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/quality.yml"
            text = workflow.read_text(encoding="utf-8")
            marker = "  push:\n    branches:\n      - main\n"
            self.assertIn(marker, text)
            workflow.write_text(
                text.replace(marker, "  push:\n", 1), encoding="utf-8"
            )
            with self.assertRaisesRegex(
                readiness.ReadinessError, "PR events and main-only push"
            ):
                readiness.validate_documents(root)

    def test_demand_graph_is_dependency_ordered_and_blocked(self) -> None:
        use_case, packet = readiness._product_contract(self.manifest)
        self.assertEqual(use_case["use_case_id"], readiness.USE_CASE_ID)
        self.assertEqual(use_case["disposition"], "blocked")
        self.assertEqual(use_case["tracked_gap"]["owner_issue"], "#261")
        self.assertEqual(packet["binding"]["stale_policy"], "reject")

    def test_unknown_demand_capability_is_rejected(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        use_case, _ = readiness._product_contract(manifest)
        use_case["capability_path"][1]["requires"] = ["input.synthetic.v1"]
        with self.assertRaisesRegex(
            readiness.ReadinessError, "unknown #261 capability dependency"
        ):
            readiness.validate_demand_closure(ROOT, manifest)

    def test_noncanonical_agent_write_path_is_rejected(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        _, packet = readiness._product_contract(manifest)
        packet["allowed_write_paths"][0] = "src/../escape"
        with self.assertRaisesRegex(
            readiness.ReadinessError, "path is not canonical"
        ):
            readiness.validate_demand_closure(ROOT, manifest)

    def test_agent_packet_is_exact_bound_and_digested(self) -> None:
        packet = readiness.build_agent_context_packet(
            ROOT, self.manifest, "1" * 40, "2" * 40
        )
        readiness.validate_agent_context_packet(
            ROOT, self.manifest, packet, "1" * 40, "2" * 40
        )
        self.assertEqual(packet["binding"]["revision"], "1" * 40)
        self.assertTrue(packet["canonical_digest"].startswith("sha256:"))
        markdown = readiness.render_agent_context_markdown(packet)
        self.assertIn(packet["canonical_digest"], markdown)
        self.assertIn("source-closure-unavailable", markdown)
        self.assertIn(packet["schema"], markdown)
        self.assertIn(packet["issue"], markdown)
        self.assertIn(packet["consumer"], markdown)
        self.assertIn(packet["goal"], markdown)
        for value in packet["expected_result_states"]:
            self.assertIn(value, markdown)
        for field in (
            "exact_contract_ids",
            "authority_reading_set",
            "allowed_write_paths",
            "required_evidence",
            "known_design_feedback",
            "forbidden_shortcuts",
            "completion_commands",
            "completion_plan",
        ):
            for value in packet[field]:
                self.assertIn(value, markdown)
        for value in packet["constructibility"].values():
            self.assertIn(str(value), markdown)
        for value in packet["binding"].values():
            self.assertIn(str(value), markdown)

    def test_check_tier_is_required_and_sqlite_bound(self) -> None:
        self.assertIn("check-tier", self.manifest["required_status_checks"]["contexts"])
        quality = yaml.safe_load(
            (ROOT / ".github/workflows/quality.yml").read_text(encoding="utf-8")
        )
        self.assertIn(
            "sqlite-store-v3-qualification",
            quality["jobs"]["check-tier"]["needs"],
        )

    def test_public_callable_workflow_requires_parent_history(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/quality.yml"
            text = workflow.read_text(encoding="utf-8")
            self.assertEqual(text.count("fetch-depth: 2"), 1)
            workflow.write_text(
                text.replace("fetch-depth: 2", "fetch-depth: 1", 1),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                readiness.ReadinessError,
                "public callable stable-ID check requires fetch-depth: 2",
            ):
                readiness.validate_documents(root)

    def test_required_check_tier_cannot_be_removed(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            checker = root / "tools/quality/check_ng_api_development_readiness.py"
            baseline = root / readiness.BASELINE_PATH
            checker.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / checker.relative_to(root), checker)
            shutil.copy2(ROOT / readiness.BASELINE_PATH, baseline)
            manifest_path = root / "schemas/cxxlens_ng_api_development_readiness.yaml"
            text = manifest_path.read_text(encoding="utf-8")
            self.assertIn("    - check-tier\n", text)
            manifest_path.write_text(text.replace("    - check-tier\n", "", 1), encoding="utf-8")
            with self.assertRaisesRegex(readiness.ReadinessError, "check-tier"):
                readiness.validate_documents(root)

    def test_readiness_checker_runs_without_git_history(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            shutil.copy2(
                ROOT / "tools/quality/check_ng_api_development_readiness.py",
                root / "tools/quality/check_ng_api_development_readiness.py",
            )
            shutil.copy2(
                ROOT / readiness.BASELINE_PATH,
                root / readiness.BASELINE_PATH,
            )
            self.assertFalse((root / ".git").exists())
            completed = subprocess.run(
                [
                    sys.executable,
                    str(root / "tools/quality/check_ng_api_development_readiness.py"),
                    "check",
                    "--root",
                    str(root),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_stale_agent_packet_is_rejected(self) -> None:
        packet = readiness.build_agent_context_packet(
            ROOT, self.manifest, "1" * 40, "2" * 40
        )
        packet["binding"]["revision"] = "3" * 40
        with self.assertRaisesRegex(
            readiness.ReadinessError, "stale, malformed, or not machine-derived"
        ):
            readiness.validate_agent_context_packet(
                ROOT, self.manifest, packet, "1" * 40, "2" * 40
            )

    def test_release_polling_tokens_are_forbidden(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/quality.yml"
            text = workflow.read_text(encoding="utf-8")
            marker = "      - name: Evaluate exact-SHA distribution 1.0 qualification\n"
            self.assertIn(marker, text)
            workflow.write_text(
                text.replace(
                    marker,
                    "      - name: Synthetic polling\n"
                    "        run: sleep 30\n"
                    + marker,
                    1,
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                readiness.ReadinessError, "qualification polling is forbidden"
            ):
                readiness.validate_documents(root)

    def test_bounded_completion_contract_is_required_in_activated_goal(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / readiness.AGENT_GOAL_PATH
            goal.write_text(
                goal.read_text(encoding="utf-8").replace(
                    "`completion-class: bounded-implementation`",
                    "completion class marker removed",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                readiness.ReadinessError, "bounded completion marker"
            ):
                readiness.validate_documents(root)

    def test_legacy_issue_close_qualification_is_rejected_from_goal(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / readiness.AGENT_GOAL_PATH
            goal.write_text(
                goal.read_text(encoding="utf-8")
                + "\nmerged-main qualification と learning checkpoint 後の active issue close\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                readiness.ReadinessError, "legacy issue-close requirement"
            ):
                readiness.validate_documents(root)

    def test_bounded_goal_keeps_aggregate_exact_sha_qualification(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / readiness.AGENT_GOAL_PATH
            goal.write_text(
                goal.read_text(encoding="utf-8").replace(
                    "それらの aggregate gate は exact main SHA の required checks と fail-closed evidence を引き続き検証します。",
                    "aggregate gate wording removed",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                readiness.ReadinessError, "bounded completion contract text"
            ):
                readiness.validate_documents(root)


if __name__ == "__main__":
    unittest.main()
