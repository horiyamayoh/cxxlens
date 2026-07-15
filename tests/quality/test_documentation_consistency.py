#!/usr/bin/env python3
"""Tests for the generated documentation and asset migration contract."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tools/quality/check_documentation_consistency.py"
SPEC = importlib.util.spec_from_file_location("documentation_consistency", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class DocumentationConsistencyTests(unittest.TestCase):
    def test_generated_ledger_matches_repository(self) -> None:
        policy, expected = MODULE.generate_ledger(ROOT)
        actual = json.loads((ROOT / MODULE.LEDGER).read_text(encoding="utf-8"))
        self.assertEqual(expected, actual)
        self.assertEqual(actual["asset_count"], len(actual["assets"]))
        self.assertEqual(
            actual["asset_count"], len({row["path"] for row in actual["assets"]})
        )
        MODULE.validate_documentation(ROOT, policy, actual)

    def test_missing_replacement_is_rejected(self) -> None:
        record = {
            "disposition": "replace",
            "status": "planned",
            "owner": "steward.test",
            "profile": "migration",
            "authority": "implementation",
            "replacement": None,
            "removal_issue": "#72",
        }
        with self.assertRaisesRegex(MODULE.DocumentationError, "no replacement"):
            MODULE.validate_record("src/example.cpp", record)

    def test_archive_without_front_matter_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "legacy.md"
            path.write_text("# Legacy\n", encoding="utf-8")
            with self.assertRaisesRegex(MODULE.DocumentationError, "front matter"):
                MODULE.front_matter(path)

    def test_broken_active_link_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = root / "README.md"
            path.write_text("# Current\n\n[missing](docs/missing.md)\n", encoding="utf-8")
            with self.assertRaisesRegex(MODULE.DocumentationError, "broken"):
                MODULE.validate_links(root, ["README.md"])


if __name__ == "__main__":
    unittest.main()
