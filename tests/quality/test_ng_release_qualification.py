#!/usr/bin/env python3
"""Fail-closed tests for distribution 1.0 GR qualification."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_release_qualification as release  # noqa: E402


class NgReleaseQualificationTests(unittest.TestCase):
    def test_repository_contract_is_implemented(self) -> None:
        manifest = release.validate_documents(ROOT)
        self.assertEqual(manifest["binding"]["package_version"], "1.0.0")
        self.assertEqual(manifest["prerequisites"]["gates"], [f"gate.g{i}" for i in range(6)])

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


if __name__ == "__main__":
    unittest.main()
