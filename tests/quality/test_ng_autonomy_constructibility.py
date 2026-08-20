#!/usr/bin/env python3
"""Negative tests for remaining high-risk constructibility authority."""

from __future__ import annotations

import pathlib
import shutil
import sys
import tempfile
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))
from check_ng_autonomy_constructibility import ConstructibilityError, MODEL, SCHEMA, validate  # noqa: E402


class ConstructibilityTest(unittest.TestCase):
    def copied_root(self, temporary: str) -> pathlib.Path:
        root = pathlib.Path(temporary)
        model = yaml.safe_load((ROOT / MODEL).read_text(encoding="utf-8"))
        paths = {MODEL, SCHEMA}
        paths.update(pathlib.Path(machine["authority"]) for machine in model["machines"].values())
        for relative in paths:
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        return root

    @staticmethod
    def rewrite(root: pathlib.Path, mutate) -> None:
        path = root / MODEL
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
        mutate(value)
        path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")

    def test_repository_model_is_valid(self) -> None:
        validate(ROOT)

    def test_message_id_collision_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["source_closure"]["message_ids"].__setitem__("source_closure", [23, 24, 25, 26, 27, 28]))
            with self.assertRaisesRegex(ConstructibilityError, "message registry"):
                validate(root)

    def test_report_attempt_before_reservation_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            def mutate(value) -> None:
                states = value["machines"]["store_candidate_report"]["report_states"]
                states[2], states[3] = states[3], states[2]
            self.rewrite(root, mutate)
            with self.assertRaisesRegex(ConstructibilityError, "Store report states"):
                validate(root)

    def test_store_projection_sources_cannot_alias(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["store_candidate_report"]["projections"].__setitem__("expected_source", "backend-staging-canonical-physical-order"))
            with self.assertRaisesRegex(ConstructibilityError, "dual projection"):
                validate(root)

    def test_publication_unknown_cannot_be_removed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["store_candidate_report"]["publication_outcomes"].remove("publication-outcome-unknown"))
            with self.assertRaisesRegex(ConstructibilityError, "publication outcomes"):
                validate(root)

    def test_store_outcome_policy_cannot_be_weakened(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["machines"]["store_candidate_report"][
                    "outcome_policy"
                ]["publication-outcome-unknown"].update({"exit": 0}),
            )
            with self.assertRaisesRegex(ConstructibilityError, "outcome policy"):
                validate(root)

    def test_store_resident_bound_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["machines"]["store_candidate_report"]["bounds"].update(
                    {"resident_window_limit_bytes": 1}
                ),
            )
            with self.assertRaisesRegex(ConstructibilityError, "numeric bounds|bounded-window"):
                validate(root)

    def test_store_candidate_graph_and_attempt_coupling_cannot_collapse(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["machines"]["store_candidate_report"][
                    "candidate_transition_graph"
                ].__setitem__("validation-sealed", "committed-verified"),
            )
            with self.assertRaisesRegex(ConstructibilityError, "candidate transition graph"):
                validate(root)
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["machines"]["store_candidate_report"][
                    "attempt_coupling"
                ].update({"attempt_count": 2}),
            )
            with self.assertRaisesRegex(ConstructibilityError, "attempt coupling"):
                validate(root)

    def test_store_full_record_grammar_and_component_census_are_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["machines"]["store_candidate_report"][
                    "projections"
                ]["record_grammar"].remove("unresolved"),
            )
            with self.assertRaisesRegex(ConstructibilityError, "dual projection"):
                validate(root)
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["machines"]["store_candidate_report"][
                    "bounds"
                ].update({"backend_cursor_bytes": 0}),
            )
            with self.assertRaisesRegex(ConstructibilityError, "numeric bounds"):
                validate(root)

    def test_reader_without_predelegation_lease_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["sqlite_read_mapping"]["predelegation_authority"].__setitem__("reader", "post-native"))
            with self.assertRaisesRegex(ConstructibilityError, "predelegation authority"):
                validate(root)

    def test_physical_census_normalization_entry_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["sqlite_normalization_effect"].__setitem__("entry", "physical-census"))
            with self.assertRaisesRegex(ConstructibilityError, "normalization entry"):
                validate(root)

    def test_missing_zero_effect_barrier_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["sqlite_read_mapping"].__setitem__("read_receipt_barrier", ["connection-closed"]))
            with self.assertRaisesRegex(ConstructibilityError, "read receipt barrier"):
                validate(root)

    def test_flat_partition_census_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["sqlite_normalization_effect"].__setitem__("fixture_partition_machine", {"F0": "only"}))
            with self.assertRaisesRegex(ConstructibilityError, "partition machine|schema validation"):
                validate(root)

    def test_reader_writer_products_cannot_merge(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["sqlite_read_mapping"]["reader_states"].__setitem__(6, "mapping-lease-promoted"))
            with self.assertRaisesRegex(ConstructibilityError, "reader states"):
                validate(root)

    def test_reader_retirement_cannot_claim_writer_retirement(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["machines"]["sqlite_read_mapping"]["retirement_join"].update(
                    {"reader_terminal": "reader-and-writer-retired"}
                ),
            )
            with self.assertRaisesRegex(ConstructibilityError, "retirement join"):
                validate(root)

    def test_outer_join_pending_edge_and_censuses_cannot_be_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["machines"]["sqlite_read_mapping"][
                    "outer_transition_graph"
                ].__setitem__("connection-revoking", "outer-custody-join-sealed"),
            )
            with self.assertRaisesRegex(ConstructibilityError, "outer transition graph"):
                validate(root)
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["machines"]["sqlite_read_mapping"][
                    "retirement_join"
                ].update({"writer_census": "reader-census-only"}),
            )
            with self.assertRaisesRegex(ConstructibilityError, "retirement join"):
                validate(root)

    def test_outer_join_receipts_and_unload_edge_are_authenticated(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["machines"]["sqlite_read_mapping"][
                    "outer_join_receipt_profile"
                ].update({"join_predicate": "one-row-per-kind"}),
            )
            with self.assertRaisesRegex(ConstructibilityError, "outer join receipt profile"):
                validate(root)
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["machines"]["sqlite_read_mapping"][
                    "unload_transition_graph"
                ].__setitem__("connection-revoking", "unload-permitted"),
            )
            with self.assertRaisesRegex(ConstructibilityError, "unload transition graph"):
                validate(root)
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["machines"]["sqlite_read_mapping"][
                    "revocation_events"
                ]["vfs-unload-request"].update({"continuation": "vfs-unloaded"}),
            )
            with self.assertRaisesRegex(ConstructibilityError, "revocation event"):
                validate(root)

    def test_revocation_event_cannot_jump_to_success(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["machines"]["sqlite_read_mapping"][
                    "revocation_events"
                ].update({"aba": "close-confirmed"}),
            )
            with self.assertRaisesRegex(ConstructibilityError, "revocation event"):
                validate(root)

    def test_unmap_failure_cannot_close(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["sqlite_read_mapping"]["teardown_transition_graph"].__setitem__("terminal-opaque-quarantine-zero-close", ["consume-distinct-close-owner-and-native-close-once"]))
            with self.assertRaisesRegex(ConstructibilityError, "teardown transition graph"):
                validate(root)

    def test_fz_pre_cannot_collapse_to_unbound_wal_delete(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["sqlite_normalization_effect"]["fixture_partition_machine"]["FZ-pre"].__setitem__("route", ["delete-WAL"]))
            with self.assertRaisesRegex(ConstructibilityError, "FZ-pre route|schema validation"):
                validate(root)

    def test_fz_pre_delete_must_precede_parent_fsync(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            def mutate(value) -> None:
                route = value["machines"]["sqlite_normalization_effect"][
                    "fixture_partition_machine"
                ]["FZ-pre"]["route"]
                route[2], route[3] = route[3], route[2]
            self.rewrite(root, mutate)
            with self.assertRaisesRegex(ConstructibilityError, "FZ-pre route"):
                validate(root)

    def test_production_predicate_cannot_be_weakened(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["sqlite_normalization_effect"].__setitem__("production_activation_predicate", ["review-only"]))
            with self.assertRaisesRegex(ConstructibilityError, "production predicate"):
                validate(root)

    def test_reader_cleanup_path_cannot_be_removed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["sqlite_read_mapping"].pop("reader_teardown_transition_graph"))
            with self.assertRaises((ConstructibilityError,)):
                validate(root)

    def test_family_recrash_stage_cannot_be_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["sqlite_normalization_effect"]["fixture_partition_machine"]["FP"]["recrash_graph"].remove("recoverable-interruption-to-recrash-classified"))
            with self.assertRaisesRegex(ConstructibilityError, "FP recrash graph|schema validation"):
                validate(root)

    def test_effect_interruption_must_cold_reclassify_without_success_edge(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["machines"]["sqlite_normalization_effect"][
                    "transition_graph"
                ].__setitem__("recoverable-interruption", "normalization-receipt"),
            )
            with self.assertRaisesRegex(ConstructibilityError, "normalization transition graph"):
                validate(root)

    def test_mapping_activation_matrix_cannot_be_shortened(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["sqlite_read_mapping"]["production_activation_predicate"].remove("two-live-store-cas"))
            with self.assertRaisesRegex(ConstructibilityError, "mapping production predicate"):
                validate(root)


if __name__ == "__main__":
    unittest.main()
