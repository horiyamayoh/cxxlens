#!/usr/bin/env python3
"""Regression tests for immutable external workflow action references."""

from __future__ import annotations

import pathlib
import shutil
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ci_supply_chain import CiSupplyChainError, check  # noqa: E402


class CiActionPinTest(unittest.TestCase):
    def copied_workflows(self) -> pathlib.Path:
        temporary = pathlib.Path(tempfile.mkdtemp())
        workflow_root = temporary / ".github" / "workflows"
        workflow_root.mkdir(parents=True)
        for name in ("quality.yml", "release.yml"):
            shutil.copy2(ROOT / ".github" / "workflows" / name, workflow_root / name)
        self.addCleanup(shutil.rmtree, temporary)
        return temporary

    def test_current_workflows_are_pinned(self) -> None:
        check(ROOT)

    def test_unpinned_external_action_is_rejected(self) -> None:
        root = self.copied_workflows()
        quality = root / ".github" / "workflows" / "quality.yml"
        quality.write_text(
            quality.read_text(encoding="utf-8")
            + "\n      - uses: actions/checkout@main\n",
            encoding="utf-8",
        )
        with self.assertRaises(CiSupplyChainError):
            check(root)

    def test_local_action_is_not_required_to_have_a_commit_pin(self) -> None:
        root = self.copied_workflows()
        quality = root / ".github" / "workflows" / "quality.yml"
        quality.write_text(
            quality.read_text(encoding="utf-8")
            + "\n      - uses: ./.github/actions/setup-ci\n",
            encoding="utf-8",
        )
        check(root)

    def test_missing_workflow_is_rejected(self) -> None:
        root = self.copied_workflows()
        (root / ".github" / "workflows" / "release.yml").unlink()
        with self.assertRaises(CiSupplyChainError):
            check(root)


if __name__ == "__main__":
    unittest.main()
