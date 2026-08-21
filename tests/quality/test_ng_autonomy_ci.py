#!/usr/bin/env python3
"""Negative tests for two-layer CI freshness authority."""

from __future__ import annotations

import pathlib
import shutil
import sys
import tempfile
import unittest
import json
from unittest import mock

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))
from check_ng_autonomy_ci import (  # noqa: E402
    AutonomyCiError,
    CONTRACT,
    SCHEMA,
    release_evaluation,
    validate,
)


class AutonomyCiTest(unittest.TestCase):
    def copied_root(self, temporary: str) -> pathlib.Path:
        root = pathlib.Path(temporary)
        contract = yaml.safe_load((ROOT / CONTRACT).read_text(encoding="utf-8"))
        paths = {CONTRACT, SCHEMA}
        paths.update(
            pathlib.Path(contract[key]["workflow"])
            for key in ("fast", "heavy", "nightly", "gr", "terminal_scope", "release")
        )
        for relative in paths:
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        return root

    def test_repository_ci_authority_is_valid(self) -> None:
        validate(ROOT)

    def test_fast_cancellation_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / ".github/workflows/autonomy-fast.yml"
            value = yaml.safe_load(path.read_text(encoding="utf-8"))
            value["concurrency"] = {"group": "fast", "cancel-in-progress": True}
            path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")
            with self.assertRaisesRegex(AutonomyCiError, "fast workflow may not cancel"):
                validate(root)

    def test_heavy_without_latest_coalescing_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / ".github/workflows/autonomy-heavy.yml"
            value = yaml.safe_load(path.read_text(encoding="utf-8"))
            value["concurrency"]["cancel-in-progress"] = False
            path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")
            with self.assertRaisesRegex(AutonomyCiError, "heavy coalescing"):
                validate(root)

    def test_heavy_manual_dispatch_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / ".github/workflows/autonomy-heavy.yml"
            value = yaml.safe_load(path.read_text(encoding="utf-8"))
            trigger_key = True if True in value else "on"
            value[trigger_key]["workflow_dispatch"] = {}
            path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")
            with self.assertRaisesRegex(AutonomyCiError, "connected to Autonomy fast"):
                validate(root)

    def test_duplicate_workflow_key_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / ".github/workflows/autonomy-fast.yml"
            path.write_text(
                path.read_text(encoding="utf-8") + "\nname: duplicate\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(AutonomyCiError, "duplicate mapping key"):
                validate(root)

    def test_heavy_provisional_upload_without_postflight_guard_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / ".github/workflows/autonomy-heavy.yml"
            value = yaml.safe_load(path.read_text(encoding="utf-8"))
            uploads = value["jobs"]["exact-latest-heavy"]["steps"]
            provisional = next(
                step for step in uploads if "provisional" in step.get("with", {}).get("name", "")
            )
            provisional["if"] = "success()"
            path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")
            with self.assertRaisesRegex(AutonomyCiError, "provisional artifact guard"):
                validate(root)

    def test_heavy_preflight_without_fresh_origin_main_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / ".github/workflows/autonomy-heavy.yml"
            value = yaml.safe_load(path.read_text(encoding="utf-8"))
            steps = value["jobs"]["freshness"]["steps"]
            steps[:] = [
                step for step in steps if step.get("name") != "Refresh current main authority"
            ]
            path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")
            with self.assertRaisesRegex(AutonomyCiError, "missing or ambiguous"):
                validate(root)

    def test_heavy_without_canonical_fast_workflow_authentication_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / ".github/workflows/autonomy-heavy.yml"
            value = yaml.safe_load(path.read_text(encoding="utf-8"))
            steps = value["jobs"]["freshness"]["steps"]
            steps[:] = [
                step
                for step in steps
                if step.get("name") != "Authenticate canonical fast workflow"
            ]
            path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")
            with self.assertRaisesRegex(AutonomyCiError, "missing or ambiguous"):
                validate(root)

    def test_release_push_trigger_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / ".github/workflows/autonomy-release-evaluation.yml"
            value = yaml.safe_load(path.read_text(encoding="utf-8"))
            trigger_key = True if True in value else "on"
            value[trigger_key]["push"] = {"branches": ["main"]}
            path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")
            with self.assertRaisesRegex(AutonomyCiError, "dispatch-only"):
                validate(root)

    def test_nightly_unpinned_job_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / ".github/workflows/nightly.yml"
            value = yaml.safe_load(path.read_text(encoding="utf-8"))
            value["jobs"]["sanitizers"].pop("needs")
            path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")
            with self.assertRaisesRegex(AutonomyCiError, "exact-candidate bound"):
                validate(root)

    def test_nightly_reusable_compatibility_producer_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / ".github/workflows/nightly.yml"
            text = path.read_text(encoding="utf-8")
            marker = (
                "if: github.event_name == 'schedule' || "
                "github.event_name == 'workflow_dispatch' || "
                "github.event_name == 'workflow_call'"
            )
            self.assertIn(marker, text)
            path.write_text(text.replace(marker, marker.rsplit(" || ", 1)[0], 1), encoding="utf-8")
            with self.assertRaisesRegex(
                AutonomyCiError, "legacy workflow_call compatibility producer"
            ):
                validate(root)

    def test_release_role_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / CONTRACT
            value = yaml.safe_load(path.read_text(encoding="utf-8"))
            value["release"]["composition"]["gr_execution_contract"] = "#173"
            path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")
            with self.assertRaisesRegex(AutonomyCiError, "role composition"):
                validate(root)

    def test_release_checkout_head_instead_of_candidate_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / ".github/workflows/autonomy-release-evaluation.yml"
            value = yaml.safe_load(path.read_text(encoding="utf-8"))
            value["jobs"]["exact-current-evaluation"]["steps"][0]["with"]["ref"] = "main"
            path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")
            with self.assertRaisesRegex(AutonomyCiError, "exact-candidate bound"):
                validate(root)

    def test_owner_workflow_missing_direct_artifact_download_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / ".github/workflows/autonomy-gr.yml"
            text = path.read_text(encoding="utf-8").replace(
                'actions/artifacts/${SOURCE_ARTIFACT_ID}/zip',
                'actions/artifacts/${SOURCE_ARTIFACT_ID}/wrong',
            )
            path.write_text(text, encoding="utf-8")
            with self.assertRaisesRegex(AutonomyCiError, "owner workflow marker"):
                validate(root)

    def test_release_authentication_failure_cannot_promote_evaluation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / ".github/workflows/autonomy-release-evaluation.yml"
            value = yaml.safe_load(path.read_text(encoding="utf-8"))
            authenticate = next(
                step
                for step in value["jobs"]["exact-current-evaluation"]["steps"]
                if step.get("id") == "authenticate"
            )
            authenticate.pop("continue-on-error")
            path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")
            with self.assertRaisesRegex(AutonomyCiError, "fail closed"):
                validate(root)

    def test_release_evaluation_without_authenticated_bundle_stays_not_qualified(self) -> None:
        candidate = "a" * 40
        with tempfile.TemporaryDirectory() as temporary:
            report = pathlib.Path(temporary) / "release-evaluation.json"
            with mock.patch("check_ng_autonomy_ci.git", return_value=candidate):
                release_evaluation(ROOT, candidate, report)
            value = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(value["status"], "not-qualified")
            self.assertFalse(value["gr_issued"])
            self.assertEqual(value["production_qualification"], "not-claimed")


if __name__ == "__main__":
    unittest.main()
