#!/usr/bin/env python3
"""Positive and fail-closed tests for the composed Wave 0/#291 contract."""

from __future__ import annotations

import copy
import json
import pathlib
import subprocess
import sys
import types
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
BASELINE_REVISION = "c4b8c9df6f7fa53656c39447b191ba723ebe2040"
BASELINE_PATH = "tests/quality/test_ng_api_development_readiness.py"
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_api_development_readiness as readiness  # noqa: E402


def _load_baseline_tests() -> types.ModuleType:
    completed = subprocess.run(
        ["git", "-C", str(ROOT), "show", f"{BASELINE_REVISION}:{BASELINE_PATH}"],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "the frozen readiness test corpus is unavailable; use a full-history "
            f"checkout containing {BASELINE_REVISION}: {completed.stderr.strip()}"
        )
    module = types.ModuleType("_cxxlens_readiness_tests_baseline")
    module.__file__ = str(ROOT / BASELINE_PATH)
    module.__package__ = None
    exec(compile(completed.stdout, module.__file__, "exec"), module.__dict__)
    return module


_baseline = _load_baseline_tests()


class NgApiDevelopmentReadinessTest(_baseline.NgApiDevelopmentReadinessTest):
    """Retain the complete previous corpus and replace only superseded CI tests."""

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


if __name__ == "__main__":
    unittest.main()
