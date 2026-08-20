#!/usr/bin/env python3
"""Repository provenance assertions for normalized WIP."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))
from check_ng_wip_inventory import INVENTORY, SCHEMA, validate  # noqa: E402


class WipInventoryTest(unittest.TestCase):
    def test_repository_inventory_is_valid(self) -> None:
        inventory = validate(ROOT)
        by_head = {entry["head"]: entry for entry in inventory["worktrees"]}
        self.assertEqual(by_head["9e8c30fe7c024b67e5b35a8b563c45be820f9e48"]["disposition"], "selected-provenance")
        self.assertTrue(any(entry["disposition"] == "prunable-registration" for entry in inventory["worktrees"]))
        self.assertEqual(inventory["normalization"]["branch_deletion"], "forbidden")
        self.assertEqual(inventory["normalization"]["live_worktree_deletion"], "forbidden")

    def test_hosted_single_worktree_clone_uses_snapshot_plus_connected_mode(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for relative in (INVENTORY, SCHEMA):
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / relative, destination)
            subprocess.run(["git", "init", "-b", "main", str(root)], check=True, capture_output=True)
            subprocess.run(["git", "-C", str(root), "config", "user.email", "test@example.com"], check=True)
            subprocess.run(["git", "-C", str(root), "config", "user.name", "test"], check=True)
            subprocess.run(["git", "-C", str(root), "add", "schemas"], check=True)
            subprocess.run(["git", "-C", str(root), "commit", "-m", "fixture"], check=True, capture_output=True)
            validate(root)


if __name__ == "__main__":
    unittest.main()
