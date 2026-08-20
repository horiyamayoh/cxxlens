#!/usr/bin/env python3
"""Repository provenance assertions for normalized WIP."""

from __future__ import annotations

import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))
from check_ng_wip_inventory import validate  # noqa: E402


class WipInventoryTest(unittest.TestCase):
    def test_repository_inventory_is_valid(self) -> None:
        inventory = validate(ROOT)
        by_head = {entry["head"]: entry for entry in inventory["worktrees"]}
        self.assertEqual(by_head["9e8c30fe7c024b67e5b35a8b563c45be820f9e48"]["disposition"], "selected-provenance")
        self.assertTrue(any(entry["disposition"] == "prunable-registration" for entry in inventory["worktrees"]))
        self.assertEqual(inventory["normalization"]["branch_deletion"], "forbidden")
        self.assertEqual(inventory["normalization"]["live_worktree_deletion"], "forbidden")


if __name__ == "__main__":
    unittest.main()
