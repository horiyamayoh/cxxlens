#!/usr/bin/env python3
"""Fail-closed tests for G5 closure/incrementality qualification."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_g5_qualification as g5  # noqa: E402


class NgG5QualificationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = g5.validate_documents(ROOT)

    def test_repository_has_implemented_g5_contract(self) -> None:
        self.assertEqual(self.manifest["maturity"], "implemented")
        self.assertEqual(self.manifest["binding"]["release_migration"], "R4")

    def test_intermediate_release_state_is_non_production_and_fail_closed(self) -> None:
        original_load = g5.load
        release = original_load(ROOT / g5.RELEASE_BUNDLE)
        distribution = next(
            row for row in release["releases"] if row["id"] == "distribution-1.0"
        )
        self.assertEqual(distribution["state"], "qualification-in-progress")
        self.assertFalse(distribution["production_supported"])

        def replacement(path: pathlib.Path):
            return release if path == ROOT / g5.RELEASE_BUNDLE else original_load(path)

        distribution["production_supported"] = True
        with mock.patch.object(g5, "load", side_effect=replacement):
            with self.assertRaisesRegex(
                g5.G5QualificationError, "without claiming production support"
            ):
                g5.validate_documents(ROOT)

    def test_final_release_state_requires_production_support_and_exact_gr_binding(self) -> None:
        original_load = g5.load
        release = original_load(ROOT / g5.RELEASE_BUNDLE)
        distribution = next(
            row for row in release["releases"] if row["id"] == "distribution-1.0"
        )
        distribution["state"] = "qualified"
        distribution["production_supported"] = True

        def replacement(path: pathlib.Path):
            return release if path == ROOT / g5.RELEASE_BUNDLE else original_load(path)

        with mock.patch.object(g5, "load", side_effect=replacement):
            g5.validate_documents(ROOT)
            release["release_qualification"]["claim_scope"] = "wildcard"
            with self.assertRaisesRegex(
                g5.G5QualificationError, "lacks independent GR binding"
            ):
                g5.validate_documents(ROOT)

    def test_performance_envelope_is_fail_closed(self) -> None:
        value = {
            "schema": "cxxlens.g5-performance.v1",
            "fixture": copy.deepcopy(self.manifest["performance"]["fixture"]),
            "method": copy.deepcopy(self.manifest["performance"]["method"]),
            "budgets": copy.deepcopy(self.manifest["performance"]["budgets"]),
            "metrics_us": {
                "warm_zero_plan_median": self.manifest["performance"]["envelope_us"]["warm_zero_plan_median"] + 1,
                "bounded_closure_median": 1,
            },
            "environment": {
                "compiler": "test",
                "operating_system": "test-os",
                "architecture": "test-arch",
            },
        }
        with self.assertRaisesRegex(g5.G5QualificationError, "envelope exceeded"):
            g5.validate_performance(self.manifest, value)

    def test_gate_status_drift_is_rejected(self) -> None:
        original_load = g5.load
        acceptance = original_load(ROOT / g5.ACCEPTANCE)
        next(row for row in acceptance["entries"] if row["id"] == "gate.g5")["status"] = "deferred"

        def replacement(path: pathlib.Path):
            return acceptance if path == ROOT / g5.ACCEPTANCE else original_load(path)

        with mock.patch.object(g5, "load", side_effect=replacement):
            with self.assertRaisesRegex(g5.G5QualificationError, "not implemented"):
                g5.validate_documents(ROOT)

    def test_closure_kind_drift_is_rejected(self) -> None:
        original_load = g5.load
        store = original_load(ROOT / g5.STORE_CONTRACT)
        store["closure"]["candidate_binding"]["allowed_kinds"].pop()

        def replacement(path: pathlib.Path):
            return store if path == ROOT / g5.STORE_CONTRACT else original_load(path)

        with mock.patch.object(g5, "load", side_effect=replacement):
            with self.assertRaisesRegex(g5.G5QualificationError, "closure kinds differ"):
                g5.validate_documents(ROOT)


if __name__ == "__main__":
    unittest.main()
