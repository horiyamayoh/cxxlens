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
            "source": "synthetic-planner",
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

    def test_production_coordinator_evidence_is_required(self) -> None:
        with self.assertRaisesRegex(
            g5.G5QualificationError, "production-coordinator evidence input is required"
        ):
            g5.validate_production_coordinator_evidence(ROOT, None)

    def test_missing_production_coordinator_evidence_file_is_rejected(self) -> None:
        missing = ROOT / "build" / "does-not-exist-g5-production-evidence.json"
        with self.assertRaisesRegex(
            g5.G5QualificationError, "production-coordinator evidence input is missing"
        ):
            g5.validate_production_coordinator_evidence(ROOT, missing)

    def test_forged_production_evidence_with_revision_drift_is_rejected(self) -> None:
        evidence = self._forged_production_evidence()
        evidence["git"]["revision"] = "0" * 40
        with self._temporary_evidence(evidence) as path:
            with self.assertRaisesRegex(
                g5.G5QualificationError,
                "not bound to the exact local SHA/tree/state",
            ):
                g5.validate_production_coordinator_evidence(ROOT, path)

    def test_synthetic_planner_cannot_be_used_as_production_evidence(self) -> None:
        evidence = self._forged_production_evidence()
        evidence["producer"]["synthetic_planner_evidence"] = True
        with self._temporary_evidence(evidence) as path:
            with self.assertRaisesRegex(g5.G5QualificationError, "schema validation failed"):
                g5.validate_production_coordinator_evidence(ROOT, path)

    def _forged_production_evidence(self) -> dict:
        state = g5.git_state(ROOT)
        state["clean"] = True
        digest = "sha256:" + "0" * 64
        artifact_digest = "materialization.incremental-sealed-artifact:sha256:" + "1" * 64
        partition_set_digest = (
            "materialization.incremental-task-partition-set:sha256:" + "2" * 64
        )
        empty = {
            "planned_provider_executions": 0,
            "actual_provider_executions": 0,
            "actual_recomputed_partition_count": 0,
            "warm_zero": True,
            "affected_only": False,
            "exact_inputs_unchanged": True,
            "executed_partition_ids": [],
            "executed_provider_task_ids": [],
            "executed_provider_execution_ids": [],
            "executed_artifact_digests": [],
            "executed_task_partition_set_digests": [],
        }
        affected = {
            "planned_provider_executions": 1,
            "actual_provider_executions": 1,
            "actual_recomputed_partition_count": 1,
            "warm_zero": False,
            "affected_only": True,
            "exact_inputs_unchanged": False,
            "executed_partition_ids": ["partition:forged"],
            "executed_provider_task_ids": ["task:forged"],
            "executed_provider_execution_ids": ["execution:forged"],
            "executed_artifact_digests": [artifact_digest],
            "executed_task_partition_set_digests": [partition_set_digest],
        }
        return {
            "schema": "cxxlens.ng-g5-production-coordinator-evidence.v1",
            "evidence_status": "observed",
            "producer": {
                "kind": "production-coordinator",
                "synthetic_planner_evidence": False,
                "interface": "run_materialization_incremental_coordinator_and_publish",
                "binary_digest": digest,
                "report_digest": digest,
            },
            "git": state,
            "execution_census": {
                "schema": "cxxlens.ng-g5-production-execution-census.v1",
                "total_planned_provider_executions": 1,
                "total_actual_provider_executions": 1,
                "total_actual_recomputed_partition_count": 1,
                "warm_zero": empty,
                "affected_only": affected,
            },
            "publication": {
                "backend": "sqlite",
                "attempted": True,
                "outcome": "committed_verified",
                "verified": True,
                "publish_call_count": 1,
                "committed_transaction_count": 1,
            },
            "reopen": {"attempted": True, "outcome": "opened", "verified": True},
            "independent_recompute": {"status": "passed", "canonical_parity": "passed"},
        }

    @staticmethod
    def _temporary_evidence(evidence: dict):
        import json
        import tempfile

        class EvidenceFile:
            def __enter__(self):
                self.directory = tempfile.TemporaryDirectory()
                self.path = pathlib.Path(self.directory.name) / "production-evidence.json"
                self.path.write_text(json.dumps(evidence), encoding="utf-8")
                return self.path

            def __exit__(self, *exc):
                return self.directory.__exit__(*exc)

        return EvidenceFile()


if __name__ == "__main__":
    unittest.main()
