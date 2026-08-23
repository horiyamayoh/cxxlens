#!/usr/bin/env python3
"""Positive and fail-closed tests for the NG SQLite physical store contract."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest
from collections.abc import Callable
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_sqlite_store_contract import (  # noqa: E402
    CONTRACT,
    CONTRACT_SCHEMA,
    EXPECTED_CONTRACT_DIGEST,
    EXPECTED_READER_LATE_CLOSE_CLEANUP_AMENDMENT_DIGEST,
    EXPECTED_READER_NATIVE_ATTACHMENT_AMENDMENT_DIGEST,
    EXPECTED_SAME_PROCESS_WRITER_MAPPING_LEASE_PROPOSAL_DIGEST,
    EXPECTED_SCHEMA_DIGEST,
    EXPECTED_WRITER_GATE_OUTCOME_EVIDENCE_AMENDMENT_DIGEST,
    EXPECTED_WRITER_NATIVE_ATTACHMENT_AMENDMENT_DIGEST,
    SNAPSHOT_CONTRACT,
    SQLITE_OPEN_PROFILE_NAMES,
    SQLiteStoreContractError,
    authority_state_bytes_equal,
    canonical_json,
    canonical_replacement_order,
    committed_generation_maximum,
    compaction_recovery_verdict,
    compressed_compaction_schedule,
    document_digest,
    effective_sqlite_sector_size,
    forced_population_reachable,
    format_reset_projection,
    load_yaml,
    normalization_large_sector_record_set,
    normalization_journal_layout,
    normalization_reconstruct_pager_nonce,
    normalization_record_pages_match,
    option_a_projection,
    parse_normalization_journal_header_sector,
    readwrite_open_observation,
    scalar_authorized_descendant_summary,
    schema_validate,
    validate_all,
    validate_authorities,
    validate_exact_contract,
    validate_exact_schema,
    validate_option_a_contract,
    validate_option_a_vectors,
    validate_committed_generation_maximum,
    validate_schema_document,
    validate_snapshot_binding,
    sqlite_open_non_ok_cleanup,
)


Mutation = Callable[[dict[str, Any]], None]


class NgSQLiteStoreContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = load_yaml(ROOT / CONTRACT)
        cls.schema = load_yaml(ROOT / CONTRACT_SCHEMA)
        cls.snapshot = load_yaml(ROOT / SNAPSHOT_CONTRACT)

    def test_exact_contract_schema_and_snapshot_binding_pass(self) -> None:
        contract = validate_all(ROOT)
        self.assertEqual(document_digest(contract), EXPECTED_CONTRACT_DIGEST)
        self.assertEqual(document_digest(self.schema), EXPECTED_SCHEMA_DIGEST)
        self.assertEqual(contract["maturity"], "accepted")
        capacity_decision = self.snapshot["df_0200_materialization_ingress"][
            "sqlite_capacity_decision"
        ]
        self.assertEqual(capacity_decision["selected_alternative"], "A")
        self.assertEqual(contract["physical_format"]["current"], "3.0.0")
        self.assertEqual(
            contract["compatibility"]["predecessor_v2"]["readable_format"],
            "2.6.0",
        )
        lease = contract["compatibility"]["predecessor_v2"][
            "read_path_strategy"
        ]["active_wal"]["source_shm_readonly_capability"][
            "shm_map_state_machine"
        ][
            "same_process_writer_mapping_lease_proposal"
        ]
        self.assertEqual(
            document_digest(lease),
            EXPECTED_SAME_PROCESS_WRITER_MAPPING_LEASE_PROPOSAL_DIGEST,
        )
        attachment = lease["writer_native_attachment_amendment_proposal"]
        self.assertEqual(
            document_digest(attachment),
            EXPECTED_WRITER_NATIVE_ATTACHMENT_AMENDMENT_DIGEST,
        )
        reader_attachment = lease[
            "reader_native_attachment_amendment_proposal"
        ]
        self.assertEqual(
            document_digest(reader_attachment),
            EXPECTED_READER_NATIVE_ATTACHMENT_AMENDMENT_DIGEST,
        )
        self.assertEqual(
            reader_attachment["id"],
            "cxxlens.sqlite.reader-shm-native-attachment.v1",
        )
        self.assertEqual(
            reader_attachment["map_attempt"]["result_enum"],
            [
                "exact_predecessor_no_attachment_route",
                "exact_predecessor_mapped_route",
                "exact_no_attachment_change",
                "exact_protocol_invalid_no_attachment",
                "exact_protocol_invalid_mapped",
                "exact_new_member",
                "exact_existing_member_revalidation",
                "exact_mapped_validation_failure",
                "terminal_outcome_ambiguous",
            ],
        )
        self.assertIn(
            "writer-mapping-generation",
            reader_attachment["attachment_identity"]["binding"],
        )
        self.assertIn(
            "checked-observed-SHM-native-attachment-object-direct-entry-device-"
            "and-mount-receipt",
            reader_attachment["attachment_identity"]["binding"],
        )
        self.assertEqual(
            reader_attachment["group_state"]["reservation_phase_enum"],
            [
                "reserved",
                "predecessor_route_active",
                "predecessor_route_retired_confirmed",
                "observed_present",
                "retired_confirmed",
                "revoked_no_map",
                "unpublished_cleanup_admitted",
                "unpublished_cleanup_confirmed",
                "terminal_quarantined",
            ],
        )
        self.assertIn(
            "may-only-promote-once-to-live-group-custody-for-an-exact-valid-OK-"
            "nonnull-row-or-be-consumed-once",
            reader_attachment["attachment_identity"][
                "cleanup_only_observation_owner"
            ],
        )
        self.assertEqual(
            reader_attachment["ownership"]["custody_state_enum"],
            [
                "live",
                "consumed_with_exact_terminal_receipt",
                "transferred_to_exact_successor",
                "transferred_to_durable_tombstone",
            ],
        )
        self.assertIn(
            "every-proposal_group-pointer-producing-callback-receipt-and-every-"
            "cached-member-pointer-use-is-covered-by-exactly-one",
            reader_attachment["eager_transaction_lifetime"][
                "pointer_coverage_relation"
            ],
        )
        self.assertEqual(
            reader_attachment["eager_transaction_lifetime"][
                "session_reservation_state_enum"
            ],
            [
                "reserved_before_sqlite",
                "promoted_to_group_owner",
                "transferred_to_existing_predecessor",
                "consumed_no_pointer",
                "terminal_quarantined",
            ],
        )
        self.assertIn(
            "Wal-apWiData-without-xShmMap",
            reader_attachment["eager_transaction_lifetime"][
                "cached_pointer_session_admission"
            ],
        )
        self.assertIn(
            "existing-or-ordinary-predecessor-with-no-local-generation",
            reader_attachment["eager_transaction_lifetime"][
                "pre_mint_route_partition"
            ],
        )
        self.assertIn(
            "map-terminal-first-member-or-revalidation-audit-owner-mint-or-"
            "exact-owner-reuse-pointer-coverage-edge-and-outward-eligibility-"
            "publish-atomically",
            reader_attachment["eager_transaction_lifetime"][
                "pointer_publication_commit"
            ],
        )
        self.assertIn(
            "predecessor_route_retired_confirmed",
            reader_attachment["group_state"]["predecessor_route_boundary"],
        )
        self.assertIn(
            "family-exclusion-custody-count",
            reader_attachment["writer_generation_boundary"]["successor"],
        )
        self.assertEqual(
            reader_attachment["cleanup_dispatch"]["logical_ack_phase_enum"],
            [
                "not_applicable",
                "awaiting_sqlite_ack",
                "consumed_by_exact_unmap",
                "consumed_by_close",
            ],
        )
        self.assertIn(
            "first-map-attempt-when-no-group-yet-exists",
            reader_attachment["ordering"]["close_cut"],
        )
        self.assertIn(
            "zero-published-members-and-zero-active-use-owners",
            reader_attachment["cleanup_dispatch"][
                "failure_exact_mapped_unpublished_first"
            ],
        )
        self.assertIn(
            "call-native-zero-times-until-the-active-use-owner-set-is-empty",
            reader_attachment["cleanup_dispatch"][
                "failure_exact_mapped_existing_group"
            ],
        )
        self.assertEqual(reader_attachment["public_api"], "unchanged")
        self.assertGreaterEqual(
            len(reader_attachment["fail_closed_matrix"]["required"]), 40
        )
        self.assertGreaterEqual(
            len(reader_attachment["fail_closed_matrix"]["positive"]), 24
        )
        late_close = lease[
            "reader_late_close_cleanup_amendment_proposal"
        ]
        self.assertEqual(
            document_digest(late_close),
            EXPECTED_READER_LATE_CLOSE_CLEANUP_AMENDMENT_DIGEST,
        )
        self.assertEqual(
            late_close["drain_subledger"]["retained_pins"],
            [
                "proposal-candidate",
                "reader-map-predelegate",
                "runtime-vfs-registration-and-callback-cohort",
                "connection-open-epoch",
                "file-family-and-mapping-generation",
            ],
        )
        self.assertIn(
            "original-owned-SQLite-call-and-session-context",
            late_close["outer_unwind_authority"]["issuance_and_cut_transfer"],
        )
        self.assertIn(
            "moves-the-one-caller-owner-under-the-registry-mutex",
            late_close["outer_unwind_authority"]["presentation_transport"],
        )
        self.assertEqual(
            late_close["close_terminal_provenance"]["kind_enum"],
            [
                "same_thread_or_reentrant_precleanup_quarantine",
                "bounded_other_thread_timeout_precleanup_quarantine",
                "bounded_other_thread_unknown_precleanup_quarantine",
            ],
        )
        self.assertIn(
            "native-xClose-call-count-zero",
            late_close["close_terminal_provenance"]["tuple_fields"],
        )
        self.assertEqual(
            late_close["drain_subledger"]["transition_graph"]
            ["cleanup_confirmed_awaiting_sqlite_ack"],
            ["consumed_by_exact_outer_unmap", "terminal_quarantined"],
        )
        self.assertIn(
            "preserves-the-valid-awaiting-ack",
            late_close["acknowledgement"]["wrong_outer_owner"],
        )
        self.assertIn(
            "transitions-cleanup_confirmed_awaiting_sqlite_ack-to-"
            "terminal_quarantined",
            late_close["acknowledgement"]["exact_outer_owner_indeterminate"],
        )
        self.assertIn(
            "same-callback-open-epoch",
            late_close["acknowledgement"]["consumption"],
        )
        self.assertIn(
            "completed-native-xClose-is-distinct-from-the-pre-cleanup-terminal-"
            "close-quarantine-row",
            late_close["acknowledgement"]["confirmed_after_close_replay"],
        )
        self.assertIn(
            "does-not-authorize-any-reader-writer-close-group",
            late_close["scope_boundary"]["transitive_authorization"],
        )
        gate_outcome = lease[
            "writer_gate_outcome_evidence_amendment_proposal"
        ]
        self.assertEqual(
            document_digest(gate_outcome),
            EXPECTED_WRITER_GATE_OUTCOME_EVIDENCE_AMENDMENT_DIGEST,
        )
        self.assertEqual(
            gate_outcome["id"],
            "cxxlens.sqlite.writer-gate-outcome-evidence.v1",
        )
        self.assertEqual(
            gate_outcome["gate_attempt_owner"]["lifecycle"],
            ["open", "issuer_sealed_kind", "transferred_to_registry"],
        )
        gate_profile = gate_outcome["gate_profile"]
        self.assertEqual(
            gate_profile["canonical_domain"],
            "cxxlens.sqlite.current-v3-writer-gate.v1",
        )
        self.assertEqual(
            gate_profile["canonical_encoding"],
            "cxxlens-canonical-tuple-v1-length-framed",
        )
        expected_stages = [
            "writer-readwrite-mode",
            "runtime-version-and-locator",
            "runtime-vfs-file-family-and-open-epoch",
            "synchronous-full-and-wal-mode",
            "current-v3-format-schema-head-counter-authority",
            "store-writer-open-before-publication-effect",
        ]
        self.assertEqual(gate_profile["ordered_stage_enum"], expected_stages)
        expected_stage_results = [
            "passed",
            "typed_determinate_failure",
            "terminal_indeterminate",
        ]
        expected_failures = [
            "writer-mode-rejected",
            "runtime-or-locator-rejected",
            "runtime-vfs-or-file-family-rejected",
            "synchronous-or-wal-mode-rejected",
            "current-v3-authority-rejected",
            "store-writer-open-rejected",
        ]
        self.assertEqual(
            gate_profile["closed_stage_result_enum"], expected_stage_results
        )
        self.assertEqual(
            list(gate_profile["ordered_stage_failure_bijection"]),
            expected_stages,
        )
        self.assertEqual(
            list(gate_profile["ordered_stage_failure_bijection"].values()),
            expected_failures,
        )
        self.assertIn(
            "no-key-or-value-is-missing-extra-duplicated-or-reused",
            gate_profile["ordered_stage_failure_bijection_rule"],
        )
        consistency = gate_profile["stage_result_outcome_consistency"]
        self.assertEqual(
            set(consistency),
            {
                "positive_success_equation",
                "typed_determinate_failure_equation",
                "terminal_indeterminate_equation",
                "rejection",
            },
        )
        self.assertIn(
            "exactly-six-ordered-stage-bundles",
            consistency["positive_success_equation"],
        )
        self.assertIn(
            "failure-enum-exactly-equals-the-bijection-value-for-that-stage",
            consistency["typed_determinate_failure_equation"],
        )
        self.assertIn(
            "exactly-matching-locus",
            consistency["terminal_indeterminate_equation"],
        )
        self.assertIn(
            "unknown-or-omitted-stage-result",
            consistency["rejection"],
        )
        for stage in expected_stages:
            stage_projection = gate_profile["stage_value_projections"][stage]
            self.assertEqual(
                set(stage_projection),
                {
                    "authority_paths",
                    "exact_value_projection",
                    "exact_effect_projection",
                    "success",
                },
            )
            for authority_path in stage_projection["authority_paths"]:
                authority: Any = contract
                for component in authority_path.split("."):
                    self.assertIsInstance(authority, dict)
                    self.assertIn(component, authority)
                    authority = authority[component]
        self.assertEqual(
            gate_profile["terminal_evidence_locus"]["closed_kind_enum"],
            [
                "before-first-stage",
                "at-immediate-next-stage",
                "after-sixth-stage-before-outcome-seal",
            ],
        )
        self.assertIn(
            "failure-or-terminal-stage-is-the-immediate-next-enum-member",
            gate_profile["first_nonpass_adjacency"],
        )
        native_attachment = gate_outcome["native_attachment_binding"]
        self.assertEqual(
            native_attachment["reservation_lifecycle"],
            [
                "reserved",
                "claimed_inflight",
                "consumed_to_present",
                "revoked",
                "quarantined",
            ],
        )
        self.assertEqual(
            native_attachment["reservation_transition_graph"],
            {
                "reserved": ["claimed_inflight", "revoked"],
                "claimed_inflight": [
                    "consumed_to_present",
                    "revoked",
                    "quarantined",
                ],
                "consumed_to_present": [],
                "revoked": [],
                "quarantined": [],
            },
        )
        self.assertEqual(native_attachment["observed_state"], ["absent", "present"])
        self.assertIn(
            "consumed-reservation-receipt",
            native_attachment["map_before_gate_present"],
        )
        self.assertIn(
            "still-reserved-expected-reservation-and-exact-live-connection-open-"
            "epoch-close-owner",
            native_attachment["empty_positive"],
        )
        claim_owner = native_attachment["callback_claim_owner"]
        self.assertIn(
            "substep-claim_and_form_dual",
            claim_owner["acquisition"],
        )
        self.assertEqual(
            claim_owner["reachable_formation_kind_enum"],
            ["atomically_dual_bound"],
        )
        self.assertIn(
            "rejects-every-second-claim-and-native-delegation-before-native",
            claim_owner["exclusivity"],
        )
        self.assertIn(
            "reject-before-native-without-waiting",
            native_attachment["claimed_later_map_same_thread_or_reentrant"],
        )
        self.assertIn(
            "bounded-wait-outside-the-registry-mutex",
            native_attachment["claimed_later_map_other_thread"],
        )
        self.assertIn(
            "leave-the-original-claim-owner-reservation-state-and-late-resolution-"
            "obligation-unchanged",
            native_attachment["claimed_later_map_timeout"],
        )
        self.assertIn(
            "postcut-nonpromotion-cleanup-decision-state-machine-for-the-proven-"
            "native-mapped-effect-never-as-no-map",
            native_attachment["claim_mapped_identity_mismatch"],
        )
        registry_cut = gate_outcome["registry_cut"]
        self.assertEqual(
            registry_cut["initial_member_classification"],
            [
                "exact_no_native_mapping",
                "exact_native_mapping",
                "native_outcome_unresolved",
            ],
        )
        self.assertEqual(
            registry_cut["terminal_member_classification"],
            [
                "exact_no_native_mapping",
                "exact_native_mapping",
                "terminal_native_outcome_unresolved",
            ],
        )
        self.assertIn(
            "records-only-the-exact-native-callback-state-and-never-contains-"
            "mints-or-implies-attachment-cleanup-or-connection-close-authority",
            registry_cut["member_classification_scope"],
        )
        self.assertEqual(
            registry_cut["attachment_cleanup_authority_census"][
                "closed_state_enum"
            ],
            [
                "not_applicable_no_mapping",
                "exact_live_attachment_owner",
                "absent",
                "ambiguous",
            ],
        )
        self.assertEqual(
            registry_cut["connection_close_authority_census"][
                "closed_state_enum"
            ],
            [
                "exact_live_connection_open_epoch_owner",
                "absent",
                "ambiguous",
            ],
        )
        self.assertIn(
            "no-member-map-receipt-pending-anchor-callback-result-or-caller-span-"
            "can-mint",
            registry_cut["member_receipt_nonownership"],
        )
        self.assertIn(
            "shared-by-every-member-admission-and-gate-cut",
            registry_cut["sequence_domain"],
        )
        self.assertIn(
            "preserve-existing-member-owners",
            registry_cut["sequence_exhaustion"],
        )
        self.assertIn(
            "terminal_token_and_tag_rederivation-one_to_one_coverage_proof",
            registry_cut["final_group_rederivation"],
        )
        self.assertIn(
            "mapped-with-exactly-one-closed-mapping-identity-tag-evidence-slot",
            registry_cut["one_to_one_coverage_proof"],
        )
        self.assertIn(
            "only-proves-token-accounting-and-never-confers-effect-eligible-final-"
            "group",
            registry_cut["coverage_accounting"],
        )
        self.assertEqual(
            set(registry_cut["effect_ready_precondition"]),
            {
                "coverage",
                "native_resolution",
                "mapping_identity_integrity",
                "authority_cell",
                "conjunction",
                "failure",
            },
        )
        self.assertIn(
            "terminal_native_outcome_unresolved-count-equals-zero",
            registry_cut["effect_ready_precondition"]["native_resolution"],
        )
        self.assertIn(
            "one-or-more-terminal_native_outcome_unresolved-members-exist-forbid-"
            "effect_ready",
            registry_cut["unresolved_fence"],
        )
        self.assertEqual(
            registry_cut["cut_execution_transition_graph"],
            {
                "cut_open": [
                    "resolving_cut_universe",
                    "cut_execution_indeterminate",
                ],
                "resolving_cut_universe": [
                    "effect_ready",
                    "cut_execution_indeterminate",
                ],
                "effect_ready": ["completed", "cut_execution_indeterminate"],
                "completed": [],
                "cut_execution_indeterminate": [],
            },
        )
        self.assertIn(
            "selected-independent-cleanup-or-unresolved-lineage-or-the-separate-"
            "invalid-coverage-terminal-sink-continuation-and-cannot-change-either-"
            "cut-terminal",
            registry_cut["cleanup_lineage_boundary"]["later_effects"],
        )
        self.assertIn(
            "every-existing-accepted-live-owner-remains-unchanged",
            registry_cut["live_positive_lifecycle_contradiction_carveout"],
        )
        self.assertEqual(
            gate_outcome["closed_outcome_union"]["typed_determinate_failure"][
                "open_epoch_drift"
            ],
            "forbidden-in-this-variant-and-always-terminal-indeterminate",
        )
        self.assertIn(
            "every-existing-DF-0206-pending-post-native-or-caller-owned-member-"
            "owner-and-exact-cleanup-obligation-remains-live-unchanged",
            gate_outcome["gate_attempt_owner"][
                "existing_member_ownership_after_preacceptance_drop"
            ],
        )
        self.assertIn(
            "each-local-member-contributes-exactly-one-of-exact_no_native_mapping-"
            "exact_native_mapping-or-terminal_native_outcome_unresolved",
            gate_outcome["native_state_resolution"]["member_native_state_only"],
        )
        self.assertIn(
            "retains-the-exact-live-attachment-owner-and-exact-live-close-owner",
            gate_outcome["native_state_resolution"][
                "positive_mapped_owner_retention"
            ],
        )
        self.assertIn(
            "all-exact-mapped-members-fold-into-the-same-single-attachment-wide-"
            "lineage",
            gate_outcome["native_state_resolution"]["mixed_no_map_and_mapped"],
        )
        late_lineage = gate_outcome["late_resolution_lineage"]
        self.assertEqual(
            late_lineage["observed_attachment_resolution"][
                "closed_state_enum"
            ],
            ["absent", "present_exact", "unresolved"],
        )
        self.assertIn(
            "exact-bound-terminal-native-no-map-receipt-or-the-exact-same-owner-"
            "control-epoch-prestart-cancellation-receipt",
            late_lineage["observed_attachment_resolution"]["absent"],
        )
        self.assertIn(
            "same-postcut-callback-owner-and-its-acquisition-bound-control-epoch",
            late_lineage["exact_cancellation_receipt"],
        )
        self.assertIn(
            "before-native-callback-start-with-proof-that-future-native-effect-"
            "is-impossible",
            late_lineage["exact_cancellation_receipt"],
        )
        self.assertIn(
            "atomically-consumes-the-exact-claimed_inflight-reservation-owner-to-"
            "revoked-and-the-claim-owner-with-zero-effect-under-the-registry-mutex",
            late_lineage["exact_cancellation_receipt"],
        )
        self.assertIn(
            "callback-started-unknown-caller-cancellation-timeout-abandonment",
            late_lineage["invalid_cancellation"],
        )
        self.assertIn(
            "issue-zero-native-invocation-permits-until-every-bound-local-precut-"
            "callback",
            late_lineage["firing_fence_by_installation_kind"][
                "precut_unresolved_cut"
            ],
        )
        self.assertIn(
            "issues-zero-cleanup-permits-until-the-same-bound-callback-has-an-"
            "exact-terminal-no-map-or-mapped-result",
            late_lineage["firing_fence_by_installation_kind"][
                "postcut_claim_derived_nonpromotion_cleanup_decision"
            ],
        )
        self.assertIn(
            "performs-zero-invocation-cleanup-close-or-public-authority-effect",
            late_lineage["precut_resolution"]["permanent_unresolved"],
        )
        self.assertIn(
            "never-mints-splits-replicates-fires-or-submits-a-second-owner",
            late_lineage["late_mapped_join"],
        )
        dispatch = gate_outcome["native_effect_dispatch_matrix"]
        self.assertEqual(
            dispatch["precedence"],
            [
                "permanent_unresolved_fence",
                "live_positive_lifecycle_contradiction_carveout",
                "mapping_identity_integrity_fence",
                "coverage_integrity_fence",
                "unresolved_fence",
                "cut_execution_indeterminate_operational_matrix",
                "sealed_outcome_matrix",
            ],
        )
        self.assertEqual(
            dispatch["dispatch_consumption_state"],
            ["unfired", "consumed_tombstone"],
        )
        self.assertEqual(
            dispatch["dispatch_consumption_transition_graph"],
            {
                "unfired": ["consumed_tombstone"],
                "consumed_tombstone": [],
            },
        )
        self.assertIn(
            "including-zero-native-call-and-zero-authority-effect-rows-"
            "atomically-transitions-unfired-to-consumed_tombstone",
            dispatch["row_consumption"],
        )
        self.assertEqual(
            set(dispatch["row_selection_domain"]),
            {
                "waiting_no_effect",
                "decision_unfired",
                "terminal_or_consumed_reentry",
                "partition",
            },
        )
        self.assertIn(
            "selects-zero-rows-performs-zero-native-or-authority-effect-and-rejects-"
            "replay",
            dispatch["row_selection_domain"]["terminal_or_consumed_reentry"],
        )
        self.assertIn(
            "effect_ready-and-mapping_identity_integrity_census-"
            "safe_no_mapped_or_all_exact_match",
            dispatch["row_selection_domain"]["decision_unfired"],
        )
        self.assertIn(
            "selects-zero-rows-performs-zero-native-or-authority-effect",
            dispatch["row_selection_domain"]["waiting_no_effect"],
        )
        self.assertIn(
            "current-cut-execution-state-or-terminal",
            dispatch["row_selection"],
        )
        operational_rows = dispatch[
            "cut_execution_indeterminate_operational_matrix"
        ]
        sealed_rows = dispatch["sealed_outcome_matrix"]
        self.assertEqual(len(operational_rows), 6)
        self.assertEqual(len(sealed_rows), 15)
        self.assertIn(
            "selects-exactly-one-of-permanent_unresolved_fence",
            dispatch["totality_and_disjointness"],
        )
        self.assertIn(
            "retain-both-exact-live-owners",
            sealed_rows[
                "positive_mapped_consumed_exact_attachment_and_close"
            ],
        )
        self.assertIn(
            "one-drain-and-no-close",
            sealed_rows[
                "determinate_failure_mapped_exact_attachment_close_absent_or_"
                "ambiguous"
            ],
        )
        no_map_rows = [
            operational_rows["all_no_map_exact_close"],
            operational_rows["all_no_map_close_absent"],
            operational_rows["all_no_map_close_ambiguous"],
            sealed_rows["determinate_failure_all_no_map_exact_close"],
            sealed_rows[
                "determinate_failure_all_no_map_close_absent_or_ambiguous"
            ],
            sealed_rows["terminal_indeterminate_all_no_map_exact_close"],
            sealed_rows["terminal_indeterminate_all_no_map_close_absent"],
            sealed_rows["terminal_indeterminate_all_no_map_close_ambiguous"],
            late_lineage["precut_resolution"]["all_no_map_exact_close"],
            late_lineage["precut_resolution"][
                "all_no_map_close_absent_or_ambiguous"
            ],
        ]
        for row in no_map_rows:
            self.assertTrue(
                "revoke-any-still-reserved-expected-attachment" in row
                or "revokes-any-still-reserved-expected-attachment" in row,
                row,
            )
        empty_group = gate_outcome["empty_and_mixed_group"]
        self.assertIn(
            "complete-atomically_dual_bound-prestart-projections",
            empty_group["positive_success_empty_first_map"],
        )
        self.assertIn(
            "exact_live_connection_open_epoch_owner-once",
            empty_group["terminal_indeterminate_empty_with_exact_close_owner"],
        )
        self.assertIn(
            "zero-native-unmap-zero-close-and-zero-other-native-calls",
            empty_group["terminal_indeterminate_empty_without_close_owner"],
        )
        self.assertIn(
            "mark-only-cut_execution_indeterminate",
            empty_group["live_positive_and_same_attempt_failure"],
        )
        self.assertIn(
            "never-converts-completed-or-cut_execution_indeterminate",
            empty_group["empty_close_axis_invariance"],
        )
        cleanup_lineage = gate_outcome["composite_cleanup_lineage"]
        self.assertEqual(
            cleanup_lineage["closed_obligation_union"],
            ["no_mapping_close_only", "mapped_unmap_then_close"],
        )
        self.assertIn(
            "zero-unmap-authority", cleanup_lineage["no_mapping_close_only"]
        )
        terminal_binding = registry_cut[
            "reservation_bearing_member_terminal_binding"
        ]
        self.assertIn(
            "reserved-to-claimed_inflight-member-sequence-allocation-dual-"
            "native_not_started-and-prestart-state-initialization",
            terminal_binding["formation_commit"],
        )
        self.assertIn(
            "reservation_claim_only-record-or-any-claim-record-missing",
            native_attachment["callback_result_totality"][
                "invalid_partial_dual_formation_guard"
            ],
        )
        admission = registry_cut["attachment_cut_exclusivity"][
            "admission_kind_partition"
        ]
        self.assertEqual(
            admission["origin_binding"]["reservation_bearing_claim_or_member_start"][
                "substep_enum"
            ],
            ["claim_and_form_dual", "start_existing_dual"],
        )
        self.assertEqual(
            admission["frozen_continuation_step_enum"],
            [
                "fresh_member_start",
                "fresh_terminal_resolution",
                "dual_shared_start",
                "dual_terminal_resolution",
            ],
        )
        self.assertEqual(
            registry_cut["mapping_identity_integrity_census"][
                "fresh_unsafe_custody_form_enum"
            ],
            ["live_origin_tombstone", "retired_identity_tombstone"],
        )
        mapping_fence = dispatch["mapping_identity_integrity_fence"]
        self.assertIn(
            "preserving-when-already-installed-the-mapping-integrity-typed-reason",
            mapping_fence["quarantine_binding"]["fresh_projection"],
        )
        self.assertIn(
            "when-retirement-occurs-first-the-later-fence-transaction",
            mapping_fence["quarantine_lifetime"],
        )
        self.assertIn(
            "nonpromotion_composite_pin_custody",
            mapping_fence["quarantine_binding"]["dual_projection"],
        )
        pin_custody = terminal_binding["mapped_pin_destination_partition"][
            "nonpromotion_composite_pin_custody"
        ]
        self.assertEqual(
            pin_custody["closed_state_enum"],
            [
                "live_owned",
                "integrity_quarantined_live",
                "cleanup_inflight",
                "cleanup_unknown_quarantined",
                "retired_tombstone",
            ],
        )
        self.assertEqual(
            pin_custody["retirement_receipt_kind_enum"],
            [
                "DF0206_same_invocation_confirmed",
                "ownerless_open_epoch_lifetime_retirement",
            ],
        )
        self.assertIn(
            "zero-live-pin-terminal-proof-binding",
            pin_custody["fence_ordering"]["totality"],
        )
        durable_gate = registry_cut["attachment_cut_exclusivity"][
            "durable_prior_cut_outcome_gate"
        ]
        self.assertIn(
            "checked-monotonic-nonreusable-terminal-record-generation",
            durable_gate["terminal_record_generation"],
        )
        self.assertIn(
            "before-any-current-slot-release-or-terminal-waiter-wakeup",
            durable_gate["terminal_install_or_replace_commit"],
        )
        self.assertIn(
            "later-exact-generation-dispatch-consumption-and-final-release-plus-"
            "wakeup-publication-are-one-transaction",
            durable_gate["release_and_wakeup_atomicity"],
        )
        second_revision_required = gate_outcome["fail_closed_matrix"][
            "second_revision_required"
        ]
        second_revision_positive = gate_outcome["fail_closed_matrix"][
            "second_revision_positive"
        ]
        for fragment in (
            "callback-claim-acquisition-publishes-claimed_inflight",
            "reservation_claim_only-or-a-missing-duplicate-or-inconsistent-dual",
            "reservation-bearing-normal-call-omits-or-selects-both-substeps",
            "formed-frozen-dual-continuation-claims-or-forms-again",
            "fresh-fence-first-retirement",
            "cleanup-first-reaches-retired_tombstone",
            "fence-first-moves-consumes-or-retires-the-composite-pins",
            "newer-negative-cut-releases-its-slot-or-wakes-a-waiter-before",
        ):
            self.assertTrue(
                any(fragment in entry for entry in second_revision_required),
                fragment,
            )
        for fragment in (
            "callback-claim-acquisition-selects-claim_and_form_dual",
            "reservation_claim_only-or-any-partial-peer-formation-selects-only",
            "closed-admission-product-allows-fresh_nonreservation_member",
            "frozen-fresh-origin-selects-only-fresh-start-or-terminal-resolution",
            "sole-registry-mutex-ordered-live_origin_tombstone-to-"
            "retired_identity_tombstone",
            "nonpromotion-composite-pin-custody-has-one-mutex-linearized-state",
            "fence-first-retains-the-independent-cleanup-owner",
            "new-cut-terminal-atomically-installs-or-replaces-the-checked-latest-"
            "generation-durable-record",
        ):
            self.assertTrue(
                any(fragment in entry for entry in second_revision_positive),
                fragment,
            )
        positive_matrix = set(gate_outcome["fail_closed_matrix"]["positive"])
        self.assertTrue(
            {
                "six-passed-stage-results-bijectively-seal-positive-adjacent-"
                "stage-specific-failure-or-closed-locus-terminal",
                "issuer-sealed-outcome-remains-unchanged-through-completed-or-"
                "indeterminate-cut-execution",
                "preacceptance-owner-drop-preserves-every-existing-DF-0206-member-"
                "owner-and-cleanup-obligation",
                "shared-sequence-exhaustion-produces-no-cut-native-call-or-"
                "existing-member-mutation",
                "map-before-gate-present-carries-one-consumed-reservation-and-"
                "never-reconsumes",
                "empty-positive-success-transfers-one-reserved-attachment-and-"
                "exact-close-owner-and-first-map-consumes-one-exact-claim",
                "complete-member-native-census-and-orthogonal-attachment-and-"
                "close-owner-census-select-one-cell",
                "coverage-with-any-unresolved-forces-cut-indeterminate-and-one-"
                "unfired-late-lineage",
                "positive-mapped-exact-attachment-and-close-promotes-whole-group-"
                "with-zero-cleanup-and-retains-owners",
                "determinate-mapped-exact-attachment-missing-close-routes-cut-"
                "indeterminate-one-drain-no-close",
                "live-positive-contradiction-preserves-the-sealed-kind-and-"
                "DF-0206-cleanup-only-lifetime-while-marking-only-cut-execution-"
                "indeterminate",
            }
            <= positive_matrix
        )
        self.assertGreaterEqual(len(second_revision_positive), 65)
        self.assertIn(
            "DF-0207-reader-grouping-transitive-authorization",
            gate_outcome["fail_closed_matrix"]["required"],
        )
        self.assertEqual(
            contract["compatibility"]["predecessor_v2"]["read_path_strategy"][
                "active_wal"
            ]["source_shm_readonly_capability"]["shm_map_state_machine"][
                "any_native_ok"
            ],
            "backend-protocol-violation-fail-closed-never-translate-to-readonly",
        )

    def test_option_a_structural_projection_and_vectors_pass_independently(self) -> None:
        validate_option_a_contract(self.contract)
        validate_option_a_vectors()
        projection = option_a_projection(self.contract)
        self.assertGreaterEqual(len(projection), 25)
        self.assertIn(
            "transaction.recovery_model.authorized_descendant_algebra.state_equality",
            projection,
        )
        self.assertIn(
            "compaction.commit_outcome_unknown.exact_descendant_classifier."
            "observable_compaction",
            projection,
        )

    def test_option_a_structural_semantic_drift_is_rejected(self) -> None:
        def normalization(value: dict[str, Any]) -> dict[str, Any]:
            return value["transaction"]["fresh_v3_initialization"]["guards"][
                "filesystem"
            ]["precreate_census"]["preauthority_sidecar_candidate"][
                "accepted_empty_original_normalization"
            ]

        def precreate_census(value: dict[str, Any]) -> dict[str, Any]:
            return value["transaction"]["fresh_v3_initialization"]["guards"][
                "filesystem"
            ]["precreate_census"]

        def writer_mapping_lease(value: dict[str, Any]) -> dict[str, Any]:
            return value["compatibility"]["predecessor_v2"][
                "read_path_strategy"
            ]["active_wal"]["source_shm_readonly_capability"][
                "shm_map_state_machine"
            ][
                "same_process_writer_mapping_lease_proposal"
            ]

        def writer_native_attachment(value: dict[str, Any]) -> dict[str, Any]:
            return writer_mapping_lease(value)[
                "writer_native_attachment_amendment_proposal"
            ]

        def reader_native_attachment(value: dict[str, Any]) -> dict[str, Any]:
            return writer_mapping_lease(value)[
                "reader_native_attachment_amendment_proposal"
            ]

        def reader_late_close_cleanup(value: dict[str, Any]) -> dict[str, Any]:
            return writer_mapping_lease(value)[
                "reader_late_close_cleanup_amendment_proposal"
            ]

        def writer_gate_outcome(value: dict[str, Any]) -> dict[str, Any]:
            return writer_mapping_lease(value)[
                "writer_gate_outcome_evidence_amendment_proposal"
            ]

        mutations: list[tuple[str, Mutation]] = [
            (
                "source-shm-runtime-symbol",
                lambda value: value["runtime"]["required_symbols"]
                ["source_shm_readonly"].remove("sqlite3_uri_key"),
            ),
            (
                "source-shm-runtime-symbol-precedence",
                lambda value: value["runtime"]["missing_runtime"].__setitem__(
                    "source_shm_readonly_symbols", "require-before-sidecar-census"
                ),
            ),
            (
                "source-shm-symbol-gate-order",
                lambda value: value["runtime"]["capability_preflight"]
                ["active_wal_source_shm_readonly_order"]["steps"].pop(0),
            ),
            (
                "source-shm-symbol-gate-leaks-to-quiescent",
                lambda value: value["runtime"]["capability_preflight"]
                ["active_wal_source_shm_readonly_order"].__setitem__(
                    "quiescent_exact_v2", "require-source-shm-readonly-symbols"
                ),
            ),
            (
                "source-shm-capability-removed",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"].pop(
                    "source_shm_readonly_capability"
                ),
            ),
            (
                "source-shm-capability-id",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"].__setitem__(
                    "id", "sqlite-source-shm-readonly-name-only"
                ),
            ),
            (
                "source-shm-uri-template",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["uri"].__setitem__(
                    "exact_template", "file:<path>?readonly_shm=1"
                ),
            ),
            (
                "source-shm-uri-forbidden-vfs",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["uri"]["forbidden"].remove(
                    "vfs-parameter"
                ),
            ),
            (
                "source-shm-open-uri-flag",
                lambda value: value["transaction"]["connection_lifecycle"]
                ["sqlite_open_profiles"]["active_existing_probe"]
                ["main_flags"].remove("SQLITE_OPEN_URI"),
            ),
            (
                "source-shm-vfs-admissibility",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"].__setitem__(
                    "vfs_admissibility", "vfs-name-is-unix"
                ),
            ),
            (
                "source-shm-capability-symbol-binding",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["required_runtime_symbols"].remove(
                    "sqlite3_uri_parameter"
                ),
            ),
            (
                "source-shm-pre-source-qualification",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]
                ["pre_source_behavioral_qualification"].__setitem__(
                    "timing", "after-source-xOpen"
                ),
            ),
            (
                "source-shm-qualification-removed",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"].pop(
                    "pre_source_behavioral_qualification"
                ),
            ),
            (
                "source-shm-first-map-proof-removed",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]
                ["pre_source_behavioral_qualification"]["proves"].remove(
                    "first-map-no-shm-initialize-truncate-extend-create-delete-or-resize"
                ),
            ),
            (
                "source-shm-scratch-namespace-binding",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]
                ["pre_source_behavioral_qualification"].__setitem__(
                    "scratch_namespace_binding", "host-path-reresolution"
                ),
            ),
            (
                "source-shm-target-filesystem-binding",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]
                ["pre_source_behavioral_qualification"].__setitem__(
                    "target_filesystem_binding", "infer-from-parent-only"
                ),
            ),
            (
                "source-shm-qualification-failure-tuple",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]
                ["qualification_unavailable_or_failed"]["result"].__setitem__(
                    "detail", "ambient-stage-label"
                ),
            ),
            (
                "source-shm-qualification-failure-result-removed",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]
                ["qualification_unavailable_or_failed"].pop("result"),
            ),
            (
                "source-shm-qualification-failure-code",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]
                ["qualification_unavailable_or_failed"]["result"].__setitem__(
                    "code", "store.sqlite-failure"
                ),
            ),
            (
                "source-shm-qualification-failure-field",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]
                ["qualification_unavailable_or_failed"]["result"].__setitem__(
                    "field", "sqlite-observation"
                ),
            ),
            (
                "source-shm-qualification-legacy-unavailable-tuple",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]
                ["qualification_unavailable_or_failed"].__setitem__(
                    "result",
                    {
                        "code": "store.backend-unavailable",
                        "field": "sqlite-observation",
                        "detail": "source-shm-readonly-qualification-unavailable",
                    },
                ),
            ),
            (
                "source-shm-qualification-failure-effects",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]
                ["qualification_unavailable_or_failed"].__setitem__(
                    "effects", "open-target-then-fail"
                ),
            ),
            (
                "source-shm-qualification-not-platform-tuple",
                lambda value: value["runtime"]["missing_runtime"]["cases"].__setitem__(
                    "unsupported_platform",
                    {
                        "code": "store.backend-unavailable",
                        "field": "sqlite",
                        "detail": "source-shm-readonly-qualification",
                    },
                ),
            ),
            (
                "source-shm-callback-receipt",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]
                ["source_open_callback_receipt"]["fields"].remove(
                    "exact-readonly_shm-1"
                ),
            ),
            (
                "source-shm-callback-receipt-removed",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"].pop(
                    "source_open_callback_receipt"
                ),
            ),
            (
                "source-shm-callback-anchored-locator-binding",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]
                ["source_open_callback_receipt"]["fields"].remove(
                    "retained-parent-fd-anchored-delegated-main-locator"
                ),
            ),
            (
                "source-shm-target-namespace-epoch",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"].pop("target_namespace_epoch"),
            ),
            (
                "source-shm-target-namespace-watch-loss",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["target_namespace_epoch"].__setitem__(
                    "loss_or_event", "accept-restored-endpoint"
                ),
            ),
            (
                "source-shm-target-direct-entry-kind",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["target_namespace_epoch"].pop(
                    "entry_kind"
                ),
            ),
            (
                "source-shm-target-direct-entry-gate",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["target_namespace_epoch"].pop(
                    "entry_kind_gate"
                ),
            ),
            (
                "source-shm-target-no-logical-reresolution",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["target_namespace_epoch"].pop(
                    "logical_host_reresolution"
                ),
            ),
            (
                "source-shm-later-map-delegation",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["shm_map_state_machine"].__setitem__(
                    "first_and_later_extend_zero", "suppress-after-cantinit"
                ),
            ),
            (
                "source-shm-pre-delegate-identity-binding",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["shm_map_state_machine"].__setitem__(
                    "pre_delegate_source_identity", "compare-current-identity-to-itself"
                ),
            ),
            (
                "source-shm-post-delegate-identity-binding",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["shm_map_state_machine"].__setitem__(
                    "post_delegate_source_identity", "accept-drift-without-unmap"
                ),
            ),
            (
                "source-shm-caller-extension-suppression",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["shm_map_state_machine"].__setitem__(
                    "caller_extend_one", "delegate-with-extend-one"
                ),
            ),
            (
                "source-shm-native-ok-translation",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["shm_map_state_machine"].__setitem__(
                    "any_native_ok", "translate-ok-nonnull-to-readonly"
                ),
            ),
            (
                "writer-mapping-lease-proposal-removed",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["shm_map_state_machine"].pop(
                    "same_process_writer_mapping_lease_proposal"
                ),
            ),
            (
                "writer-mapping-lease-status-self-accepted",
                lambda value: writer_mapping_lease(value).__setitem__(
                    "status", "accepted"
                ),
            ),
            (
                "writer-native-attachment-amendment-removed",
                lambda value: writer_mapping_lease(value).pop(
                    "writer_native_attachment_amendment_proposal"
                ),
            ),
            (
                "writer-native-attachment-pointer-only-identity",
                lambda value: writer_native_attachment(value).__setitem__(
                    "attachment_identity",
                    "native-pointer-and-connection-only",
                ),
            ),
            (
                "writer-native-attachment-cross-grouping-weakened",
                lambda value: writer_native_attachment(value).__setitem__(
                    "cross_attachment_grouping",
                    "allowed-when-the-native-pointer-and-generation-match",
                ),
            ),
            (
                "writer-native-attachment-page-support-removed",
                lambda value: writer_native_attachment(value).pop(
                    "generation_fresh_reader_page_set"
                ),
            ),
            (
                "writer-native-attachment-sole-page-inflight-blocker-removed",
                lambda value: writer_native_attachment(value).pop(
                    "nonlast_sole_page_reader_predelegate_blocker"
                ),
            ),
            (
                "writer-native-attachment-remaining-page-inflight-blocked",
                lambda value: writer_native_attachment(value).__setitem__(
                    "nonlast_remaining_page_reader_predelegate",
                    "every-reader-predelegation-blocks-nonlast-cleanup",
                ),
            ),
            (
                "writer-native-attachment-sole-page-same-thread-waits",
                lambda value: writer_native_attachment(value).__setitem__(
                    "nonlast_sole_page_same_thread",
                    "wait-on-the-cleanup-callback-thread-then-delegate-unmap",
                ),
            ),
            (
                "writer-native-attachment-sole-page-other-thread-retries",
                lambda value: writer_native_attachment(value).__setitem__(
                    "nonlast_sole_page_other_thread",
                    "retry-after-timeout-unknown-or-unconfirmed-reader-cleanup",
                ),
            ),
            (
                "writer-native-attachment-handoff-becomes-page-support",
                lambda value: writer_native_attachment(value).__setitem__(
                    "established_reader_handoff_during_writer_cleanup",
                    "block-writer-cleanup-and-mint-fresh-reader-page-support",
                ),
            ),
            (
                "writer-native-attachment-retired-evidence-transferable",
                lambda value: writer_native_attachment(value).__setitem__(
                    "retired_attachment_evidence",
                    "transfer-to-any-live-attachment-in-the-generation",
                ),
            ),
            (
                "writer-native-attachment-gate-total-order-removed",
                lambda value: writer_native_attachment(value).pop(
                    "gate_completion_total_order"
                ),
            ),
            (
                "writer-native-attachment-gate-partial-pending-allowed",
                lambda value: writer_native_attachment(value).__setitem__(
                    "successful_gate_postcondition",
                    "same-attachment-pending-members-may-survive-gate-success",
                ),
            ),
            (
                "writer-native-attachment-one-unmap-positive-removed",
                lambda value: writer_native_attachment(value)[
                    "fail_closed_matrix"
                ]["positive"].remove(
                    "one-connection-page-zero-and-page-one-one-native-unmap"
                ),
            ),
            (
                "writer-native-attachment-sole-page-negative-removed",
                lambda value: writer_native_attachment(value)[
                    "fail_closed_matrix"
                ]["required"].remove(
                    "nonlast-removal-sole-page-support-reused-by-fresh-reader"
                ),
            ),
            (
                "writer-native-attachment-sole-page-inflight-negative-removed",
                lambda value: writer_native_attachment(value)[
                    "fail_closed_matrix"
                ]["required"].remove(
                    "nonlast-sole-page-support-writer-unmap-before-reader-"
                    "predelegation-resolution"
                ),
            ),
            (
                "writer-native-attachment-same-thread-negative-removed",
                lambda value: writer_native_attachment(value)[
                    "fail_closed_matrix"
                ]["required"].remove(
                    "nonlast-sole-page-reader-predelegation-same-thread-wait-"
                    "or-native-unmap"
                ),
            ),
            (
                "writer-native-attachment-other-thread-negative-removed",
                lambda value: writer_native_attachment(value)[
                    "fail_closed_matrix"
                ]["required"].remove(
                    "nonlast-sole-page-reader-predelegation-other-thread-"
                    "timeout-unknown-drift-or-unconfirmed-cleanup-retry"
                ),
            ),
            (
                "writer-native-attachment-remaining-support-negative-removed",
                lambda value: writer_native_attachment(value)[
                    "fail_closed_matrix"
                ]["required"].remove(
                    "nonlast-remaining-page-support-reader-predelegation-"
                    "treated-as-cleanup-blocker"
                ),
            ),
            (
                "writer-native-attachment-handoff-negative-removed",
                lambda value: writer_native_attachment(value)[
                    "fail_closed_matrix"
                ]["required"].remove(
                    "established-reader-handoff-treated-as-writer-cleanup-"
                    "blocker-or-fresh-page-support"
                ),
            ),
            (
                "writer-native-attachment-sole-page-handoff-positive-removed",
                lambda value: writer_native_attachment(value)[
                    "fail_closed_matrix"
                ]["positive"].remove(
                    "nonlast-sole-page-reader-predelegation-promotes-to-"
                    "handoff-then-one-writer-unmap"
                ),
            ),
            (
                "writer-native-attachment-sole-page-resolution-positive-removed",
                lambda value: writer_native_attachment(value)[
                    "fail_closed_matrix"
                ]["positive"].remove(
                    "nonlast-sole-page-reader-predelegation-no-map-or-"
                    "confirmed-cleanup-then-one-writer-unmap"
                ),
            ),
            (
                "writer-native-attachment-remaining-support-positive-removed",
                lambda value: writer_native_attachment(value)[
                    "fail_closed_matrix"
                ]["positive"].remove(
                    "nonlast-reader-predelegation-with-other-exact-page-"
                    "support-does-not-block"
                ),
            ),
            (
                "writer-native-attachment-established-handoff-positive-removed",
                lambda value: writer_native_attachment(value)[
                    "fail_closed_matrix"
                ]["positive"].remove(
                    "nonlast-established-reader-handoff-does-not-block-and-"
                        "retains-only-its-sealed-lifetime"
                    ),
            ),
            (
                "reader-native-attachment-amendment-removed",
                lambda value: writer_mapping_lease(value).pop(
                    "reader_native_attachment_amendment_proposal"
                ),
            ),
            (
                "reader-native-attachment-mapping-generation-identity-removed",
                lambda value: reader_native_attachment(value)[
                    "attachment_identity"
                ]["binding"].remove("writer-mapping-generation"),
            ),
            (
                "reader-native-attachment-cleanup-observation-owner-removed",
                lambda value: reader_native_attachment(value)[
                    "attachment_identity"
                ].pop("cleanup_only_observation_owner"),
            ),
            (
                "reader-native-attachment-predecessor-retirement-removed",
                lambda value: reader_native_attachment(value)["group_state"][
                    "reservation_transition_graph"
                ].pop("predecessor_route_active"),
            ),
            (
                "reader-native-attachment-unpublished-cleanup-state-removed",
                lambda value: reader_native_attachment(value)["group_state"][
                    "reservation_phase_enum"
                ].remove("unpublished_cleanup_confirmed"),
            ),
            (
                "reader-native-attachment-later-failure-proactively-unmaps",
                lambda value: reader_native_attachment(value)[
                    "cleanup_dispatch"
                ].__setitem__(
                    "failure_exact_mapped_existing_group",
                    "proactively-unmap-the-live-group-before-use-owners-end",
                ),
            ),
            (
                "reader-native-attachment-use-owner-set-removed",
                lambda value: reader_native_attachment(value)[
                    "eager_transaction_lifetime"
                ].pop("owner_set"),
            ),
            (
                "reader-native-attachment-pointer-coverage-removed",
                lambda value: reader_native_attachment(value)[
                    "eager_transaction_lifetime"
                ].pop("pointer_coverage_relation"),
            ),
            (
                "reader-native-attachment-pointer-publication-atomicity-removed",
                lambda value: reader_native_attachment(value)[
                    "eager_transaction_lifetime"
                ].pop("pointer_publication_commit"),
            ),
            (
                "reader-native-attachment-session-start-issuer-removed",
                lambda value: reader_native_attachment(value)[
                    "eager_transaction_lifetime"
                ].pop("session_start_issuer"),
            ),
            (
                "reader-native-attachment-pre-mint-route-partition-removed",
                lambda value: reader_native_attachment(value)[
                    "eager_transaction_lifetime"
                ].pop("pre_mint_route_partition"),
            ),
            (
                "reader-native-attachment-cached-pointer-admission-removed",
                lambda value: reader_native_attachment(value)[
                    "eager_transaction_lifetime"
                ].pop("cached_pointer_session_admission"),
            ),
            (
                "reader-native-attachment-protocol-invalid-route-removed",
                lambda value: reader_native_attachment(value)[
                    "map_attempt"
                ].pop("exact_protocol_invalid_no_attachment"),
            ),
            (
                "reader-native-attachment-closed-custody-enum-removed",
                lambda value: reader_native_attachment(value)["ownership"].pop(
                    "custody_kind_enum"
                ),
            ),
            (
                "reader-native-attachment-opaque-successor-blocker-removed",
                lambda value: reader_native_attachment(value)[
                    "writer_generation_boundary"
                ].__setitem__(
                    "successor",
                    "only-live-group-count-blocks-successor-admission",
                ),
            ),
            (
                "reader-native-attachment-terminal-commit-success-leaks",
                lambda value: reader_native_attachment(value)[
                    "outward_projection"
                ].__setitem__(
                    "unmap_result",
                    "native-SQLITE-OK-is-always-outward-SQLITE-OK",
                ),
            ),
            (
                "reader-native-attachment-logical-ack-open-ended",
                lambda value: reader_native_attachment(value)[
                    "cleanup_dispatch"
                ]["logical_ack_phase_enum"].append("replayable"),
            ),
            (
                "reader-native-attachment-close-cut-removed",
                lambda value: reader_native_attachment(value)["ordering"].pop(
                    "close_cut"
                ),
            ),
            (
                "reader-native-attachment-OK-null-preserved",
                lambda value: reader_native_attachment(value)[
                    "outward_projection"
                ].__setitem__(
                    "exact_determinate_no_change",
                    "all-native-statuses-including-SQLITE-OK-plus-null-pass-through",
                ),
            ),
            (
                "reader-late-close-cleanup-proposal-removed",
                lambda value: writer_mapping_lease(value).pop(
                    "reader_late_close_cleanup_amendment_proposal"
                ),
            ),
            (
                "reader-late-close-cleanup-outer-ack-open-ended",
                lambda value: reader_late_close_cleanup(value)[
                    "acknowledgement"
                ].__setitem__(
                    "consumption", "any-later-unmap-may-consume-and-return-OK"
                ),
            ),
            (
                "reader-late-close-cleanup-outer-owner-binding-underbound",
                lambda value: reader_late_close_cleanup(value)[
                    "outer_unwind_authority"
                ]["binding"].remove("expected-outer-unwind-owner-token"),
            ),
            (
                "reader-late-close-cleanup-presentation-transport-registry-owns",
                lambda value: reader_late_close_cleanup(value)[
                    "outer_unwind_authority"
                ].__setitem__(
                    "presentation_transport",
                    "registry-owns-and-reconstructs-the-caller-owner",
                ),
            ),
            (
                "reader-late-close-cleanup-close-provenance-native-close-added",
                lambda value: reader_late_close_cleanup(value)[
                    "close_terminal_provenance"
                ]["kind_enum"].append("native_xClose_completed"),
            ),
            (
                "reader-late-close-cleanup-close-provenance-tuple-underbound",
                lambda value: reader_late_close_cleanup(value)[
                    "close_terminal_provenance"
                ]["tuple_fields"].remove("exact-close-owner-token"),
            ),
            (
                "reader-late-close-cleanup-ack-indeterminate-transition-removed",
                lambda value: reader_late_close_cleanup(value)[
                    "drain_subledger"
                ]["transition_graph"][
                    "cleanup_confirmed_awaiting_sqlite_ack"
                ].remove("terminal_quarantined"),
            ),
            (
                "reader-late-close-cleanup-wrong-owner-steals-ack",
                lambda value: reader_late_close_cleanup(value)[
                    "acknowledgement"
                ].__setitem__(
                    "wrong_outer_owner", "wrong-owner-consumes-the-valid-ack"
                ),
            ),
            (
                "reader-late-close-cleanup-exact-owner-indeterminate-replayable",
                lambda value: reader_late_close_cleanup(value)[
                    "acknowledgement"
                ].__setitem__(
                    "exact_outer_owner_indeterminate",
                    "preserve-awaiting-ack-and-retry-later",
                ),
            ),
            (
                "reader-late-close-cleanup-after-close-replay-accepted",
                lambda value: reader_late_close_cleanup(value)[
                    "acknowledgement"
                ].__setitem__(
                    "confirmed_after_close_replay",
                    "consume-after-native-close-and-return-OK",
                ),
            ),
            (
                "reader-late-close-cleanup-candidate-pin-released",
                lambda value: reader_late_close_cleanup(value)[
                    "drain_subledger"
                ]["retained_pins"].remove("proposal-candidate"),
            ),
            (
                "reader-late-close-cleanup-fresh-authority-revived",
                lambda value: reader_late_close_cleanup(value)[
                    "quarantine"
                ].__setitem__(
                    "forbidden_authority", "fresh-reader-admission-is-allowed"
                ),
            ),
            (
                "writer-gate-outcome-proposal-removed",
                lambda value: writer_mapping_lease(value).pop(
                    "writer_gate_outcome_evidence_amendment_proposal"
                ),
            ),
            (
                "writer-gate-outcome-closed-stage-removed",
                lambda value: writer_gate_outcome(value)["gate_profile"][
                    "ordered_stage_enum"
                ].remove("store-writer-open-before-publication-effect"),
            ),
            (
                "writer-gate-outcome-stage-effect-projection-removed",
                lambda value: writer_gate_outcome(value)["gate_profile"][
                    "stage_value_projections"
                ]["store-writer-open-before-publication-effect"].pop(
                    "exact_effect_projection"
                ),
            ),
            (
                "writer-gate-outcome-terminal-locus-allows-later-stage",
                lambda value: writer_gate_outcome(value)["gate_profile"][
                    "terminal_evidence_locus"
                ].__setitem__(
                    "rule",
                    "terminal-may-omit-its-locus-and-execute-later-stages",
                ),
            ),
            (
                "writer-gate-outcome-policy-profile-digest-weakened",
                lambda value: writer_gate_outcome(value)["gate_profile"].__setitem__(
                    "canonical_policy_profile_digest",
                    "opaque-untyped-digest",
                ),
            ),
            (
                "writer-gate-outcome-exclusivity-weakened",
                lambda value: writer_gate_outcome(value)[
                    "closed_outcome_union"
                ].__setitem__(
                    "exclusivity",
                    "success-and-failure-may-both-be-issued",
                ),
            ),
            (
                "writer-gate-outcome-positive-before-cut-completed",
                lambda value: writer_gate_outcome(value)[
                    "closed_outcome_union"
                ]["positive_success"].__setitem__(
                    "effect",
                    "promote-before-registry-cut-execution-completes",
                ),
            ),
            (
                "writer-gate-outcome-reservation-lifecycle-reusable",
                lambda value: writer_gate_outcome(value)[
                    "native_attachment_binding"
                ]["reservation_lifecycle"].append("reserved"),
            ),
            (
                "writer-gate-outcome-map-before-gate-reconsumes",
                lambda value: writer_gate_outcome(value)[
                    "native_attachment_binding"
                ].__setitem__(
                    "map_before_gate_present",
                    "consume-the-reservation-again-at-the-gate",
                ),
            ),
            (
                "writer-gate-outcome-native-mismatch-treated-no-map",
                lambda value: writer_gate_outcome(value)[
                    "native_attachment_binding"
                ].__setitem__(
                    "claim_mapped_identity_mismatch",
                    "treat-the-native-mapped-effect-as-no-map",
                ),
            ),
            (
                "writer-gate-outcome-precut-mismatch-mutates",
                lambda value: writer_gate_outcome(value)["gate_profile"].__setitem__(
                    "supplied_mismatch",
                    "seal-a-determinate-failure-and-hide-the-live-group",
                ),
            ),
            (
                "writer-gate-outcome-precut-abandonment-mutates",
                lambda value: writer_gate_outcome(value)["gate_attempt_owner"].__setitem__(
                    "pre_acceptance_drop",
                    "derive-and-hide-the-caller-member-snapshot",
                ),
            ),
            (
                "writer-gate-outcome-postcut-abandonment-determinate",
                lambda value: writer_gate_outcome(value)["gate_attempt_owner"].__setitem__(
                    "postcut_continuation_abandonment",
                    "reclassify-as-typed-determinate-failure",
                ),
            ),
            (
                "writer-gate-outcome-precut-drop-discharges-member",
                lambda value: writer_gate_outcome(value)["gate_attempt_owner"].__setitem__(
                    "existing_member_ownership_after_preacceptance_drop",
                    "discharge-every-existing-DF-0206-member-owner",
                ),
            ),
            (
                "writer-gate-outcome-cut-reseals-outcome",
                lambda value: writer_gate_outcome(value)["registry_cut"].__setitem__(
                    "outcome_immutability",
                    "cut-execution-may-reseal-failure-as-indeterminate",
                ),
            ),
            (
                "writer-gate-outcome-independent-sequence-domains",
                lambda value: writer_gate_outcome(value)["registry_cut"].__setitem__(
                    "sequence_domain",
                    "independent-member-admission-and-gate-cut-counters",
                ),
            ),
            (
                "writer-gate-outcome-sequence-exhaustion-partial-cut",
                lambda value: writer_gate_outcome(value)["registry_cut"].__setitem__(
                    "sequence_exhaustion",
                    "publish-a-partial-cut-and-discharge-existing-members",
                ),
            ),
            (
                "writer-gate-outcome-caller-defines-group",
                lambda value: writer_gate_outcome(value)["registry_cut"].__setitem__(
                    "caller_member_span",
                    "caller-subset-defines-the-authoritative-cleanup-group",
                ),
            ),
            (
                "writer-gate-outcome-cut-snapshot-is-final-group",
                lambda value: writer_gate_outcome(value)["registry_cut"].__setitem__(
                    "final_group_rederivation",
                    "reuse-the-cut-time-caller-snapshot-after-blocker-resolution",
                ),
            ),
            (
                "writer-gate-outcome-coverage-allows-missing-member",
                lambda value: writer_gate_outcome(value)["registry_cut"].__setitem__(
                    "one_to_one_coverage_proof",
                    "allow-a-missing-or-duplicate-cut-universe-token",
                ),
            ),
            (
                "writer-gate-outcome-cut-terminal-revives",
                lambda value: writer_gate_outcome(value)["registry_cut"][
                    "cut_execution_transition_graph"
                ].__setitem__("completed", ["cut_open"]),
            ),
            (
                "writer-gate-outcome-cleanup-changes-cut-terminal",
                lambda value: writer_gate_outcome(value)["registry_cut"].__setitem__(
                    "cleanup_lineage_boundary",
                    "close-failure-changes-completed-to-cut_execution_indeterminate",
                ),
            ),
            (
                "writer-gate-outcome-incomplete-coverage-subset-effect",
                lambda value: writer_gate_outcome(value)["registry_cut"].__setitem__(
                    "execution_indeterminate",
                    "initiate-a-new-subset-determinate-failure-effect",
                ),
            ),
            (
                "writer-gate-outcome-positive-cleans-up",
                lambda value: writer_gate_outcome(value)[
                    "native_state_resolution"
                ].__setitem__(
                    "positive_mapped_owner_retention",
                    "drain-the-native-mapping-before-promotion",
                ),
            ),
            (
                "writer-gate-outcome-indeterminate-mints-failure",
                lambda value: writer_gate_outcome(value)[
                    "native_state_resolution"
                ].__setitem__(
                    "drain_semantics",
                    "drain-success-mints-typed-determinate-failure",
                ),
            ),
            (
                "writer-gate-outcome-known-no-map-speculative-unmap",
                lambda value: writer_gate_outcome(value)[
                    "native_state_resolution"
                ].__setitem__(
                    "exact_no_native_mapping",
                    "attempt-one-speculative-native-unmap",
                ),
            ),
            (
                "writer-gate-outcome-mixed-group-per-member-unmap",
                lambda value: writer_gate_outcome(value)[
                    "native_state_resolution"
                ].__setitem__(
                    "mixed_no_map_and_mapped",
                    "perform-one-native-unmap-per-mapped-member",
                ),
            ),
            (
                "writer-gate-outcome-known-mapped-drain-optional",
                lambda value: writer_gate_outcome(value)[
                    "native_effect_dispatch_matrix"
                ]["cut_execution_indeterminate_operational_matrix"].__setitem__(
                    "mapped_exact_live_attachment_owner",
                    "perform-at-most-one-attachment-wide-drain",
                ),
            ),
            (
                "writer-gate-outcome-cleanup-reclassifies-cut-terminal",
                lambda value: writer_gate_outcome(value)[
                    "native_state_resolution"
                ].__setitem__(
                    "cleanup_axis_independence",
                    "close-failure-converts-completed-to-cut_execution_indeterminate",
                ),
            ),
            (
                "writer-gate-outcome-empty-exact-owner-skips-close",
                lambda value: writer_gate_outcome(value)[
                    "empty_and_mixed_group"
                ].__setitem__(
                    "terminal_indeterminate_empty_with_exact_close_owner",
                    "zero-native-unmap-and-zero-close",
                ),
            ),
            (
                "writer-gate-outcome-empty-no-owner-speculative-close",
                lambda value: writer_gate_outcome(value)[
                    "empty_and_mixed_group"
                ].__setitem__(
                    "terminal_indeterminate_empty_without_close_owner",
                    "guess-a-close-owner-and-call-close",
                ),
            ),
            (
                "writer-gate-outcome-empty-close-reclassifies-axes",
                lambda value: writer_gate_outcome(value)[
                    "empty_and_mixed_group"
                ].__setitem__(
                    "empty_close_axis_invariance",
                    "close-failure-reseals-the-gate-and-cut-terminal",
                ),
            ),
            (
                "writer-gate-outcome-mixed-live-hidden",
                lambda value: writer_gate_outcome(value)[
                    "empty_and_mixed_group"
                ].__setitem__(
                    "live_positive_and_same_attempt_failure",
                    "hide-existing-live-authority-as-clean-gate-failure",
                ),
            ),
            (
                "writer-gate-outcome-live-carveout-removed",
                lambda value: writer_gate_outcome(value)["registry_cut"].__setitem__(
                    "live_positive_lifecycle_contradiction_carveout",
                    "quarantine-the-existing-accepted-DF-0206-live-group",
                ),
            ),
            (
                "writer-gate-outcome-composite-union-collapsed",
                lambda value: writer_gate_outcome(value)[
                    "composite_cleanup_lineage"
                ]["closed_obligation_union"].remove("no_mapping_close_only"),
            ),
            (
                "writer-gate-outcome-cleanup-reissues-after-commit-failure",
                lambda value: writer_gate_outcome(value)[
                    "composite_cleanup_lineage"
                ].__setitem__(
                    "preinvoke_consumption",
                    "reissue-after-internal-commit-failure",
                ),
            ),
            (
                "writer-gate-outcome-unmap-failure-closes-and-retries",
                lambda value: writer_gate_outcome(value)[
                    "composite_cleanup_lineage"
                ].__setitem__(
                    "unmap_failure",
                    "close-and-retry-after-nonOK-or-unknown",
                ),
            ),
            (
                "writer-gate-outcome-close-failure-mapped-only",
                lambda value: writer_gate_outcome(value)[
                    "composite_cleanup_lineage"
                ].__setitem__(
                    "close_failure",
                    "apply-only-after-mapped-unmap-success",
                ),
            ),
            (
                "writer-gate-outcome-reader-transitivity",
                lambda value: writer_gate_outcome(value)["reader_boundary"].__setitem__(
                    "transitive_authorization",
                    "authorize-DF-0207-reader-attachment-grouping",
                ),
            ),
            (
                "writer-gate-outcome-indeterminate-drain-negative-removed",
                lambda value: writer_gate_outcome(value)["fail_closed_matrix"][
                    "required"
                ].remove(
                    "cut-indeterminate-mapped-exact-attachment-skips-or-duplicates-"
                    "drain-or-closes"
                ),
            ),
            (
                "writer-gate-outcome-dual-formation-not-atomic",
                lambda value: writer_gate_outcome(value)[
                    "native_attachment_binding"
                ]["callback_claim_owner"].__setitem__(
                    "reachable_formation_kind_enum",
                    ["reservation_claim_only"],
                ),
            ),
            (
                "writer-gate-outcome-invalid-partial-enters-ordinary-route",
                lambda value: writer_gate_outcome(value)[
                    "native_attachment_binding"
                ]["callback_result_totality"].__setitem__(
                    "invalid_partial_dual_formation_guard",
                    "route-partial-claim-through-the-ordinary-product",
                ),
            ),
            (
                "writer-gate-outcome-normal-substep-partition-weakened",
                lambda value: writer_gate_outcome(value)["registry_cut"][
                    "attachment_cut_exclusivity"
                ]["admission_kind_partition"]["origin_binding"][
                    "reservation_bearing_claim_or_member_start"
                ][
                    "substep_enum"
                ].remove(
                    "start_existing_dual"
                ),
            ),
            (
                "writer-gate-outcome-frozen-step-cross-origin",
                lambda value: writer_gate_outcome(value)["registry_cut"][
                    "attachment_cut_exclusivity"
                ]["admission_kind_partition"][
                    "frozen_continuation_step_enum"
                ].remove(
                    "dual_terminal_resolution"
                ),
            ),
            (
                "writer-gate-outcome-fresh-dual-custody-collapsed",
                lambda value: writer_gate_outcome(value)["registry_cut"][
                    "mapping_identity_integrity_census"
                ].__setitem__(
                    "fresh_unsafe_custody_form_enum",
                    ["nonpromotion_composite_pin_custody"],
                ),
            ),
            (
                "writer-gate-outcome-fresh-retirement-metadata-rebound",
                lambda value: writer_gate_outcome(value)[
                    "native_effect_dispatch_matrix"
                ]["mapping_identity_integrity_fence"]["quarantine_binding"].__setitem__(
                    "fresh_projection",
                    "retire-first-then-reseal-tags-and-reconstruct-pins",
                ),
            ),
            (
                "writer-gate-outcome-pin-custody-transition-revives",
                lambda value: writer_gate_outcome(value)["registry_cut"][
                    "reservation_bearing_member_terminal_binding"
                ]["mapped_pin_destination_partition"][
                    "nonpromotion_composite_pin_custody"
                ][
                    "transition_graph"
                ].__setitem__(
                    "retired_tombstone",
                    ["live_owned"],
                ),
            ),
            (
                "writer-gate-outcome-durable-release-before-current-record",
                lambda value: writer_gate_outcome(value)["registry_cut"][
                    "attachment_cut_exclusivity"
                ]["durable_prior_cut_outcome_gate"].__setitem__(
                    "release_and_wakeup_atomicity",
                    "release-slot-and-wake-before-installing-the-current-record",
                ),
            ),
            (
                "writer-mapping-lease-current-rejection-weakened",
                lambda value: writer_mapping_lease(value).__setitem__(
                    "unadmitted_native_projection",
                    "translate-native-OK-when-pointer-is-nonnull",
                ),
            ),
            (
                "writer-mapping-lease-native-ok-pass-through",
                lambda value: writer_mapping_lease(value)[
                    "admitted_native_projection"
                ].__setitem__("outward_result", "exact-SQLITE_OK-plus-pointer"),
            ),
            (
                "writer-mapping-lease-qualification-route-authorized",
                lambda value: writer_mapping_lease(value)[
                    "admitted_native_projection"
                ].__setitem__(
                    "leased_route_only",
                    "nonproduction-validation-and-production-routes",
                ),
            ),
            (
                "writer-mapping-lease-scratch-validator-authorized",
                lambda value: writer_mapping_lease(value)[
                    "admitted_native_projection"
                ].__setitem__(
                    "scratch_or_map_sequence_validator",
                    "may-consume-process-lease",
                ),
            ),
            (
                "writer-mapping-lease-pending-before-delegation",
                lambda value: writer_mapping_lease(value)[
                    "two_stage_writer_authority"
                ].__setitem__(
                    "predelegate_attempt",
                    "install-registry-pending-before-native-map",
                ),
            ),
            (
                "writer-mapping-lease-writer-inflight-removed",
                lambda value: writer_mapping_lease(value)[
                    "two_stage_writer_authority"
                ].pop("writer_predelegate_inflight"),
            ),
            (
                "writer-mapping-lease-namespace-watch-after-stat",
                lambda value: writer_mapping_lease(value)[
                    "two_stage_writer_authority"
                ].__setitem__(
                    "writer_mapping_epoch_start",
                    "arm-watch-after-pre-stat-or-native-map",
                ),
            ),
            (
                "writer-mapping-lease-untrusted-platform-mints",
                lambda value: writer_mapping_lease(value)[
                    "two_stage_writer_authority"
                ].__setitem__(
                    "writer_mapping_epoch_failure",
                    "non-Linux-or-unavailable-stat-watch-may-mint-from-final-state",
                ),
            ),
            (
                "writer-mapping-lease-absent-shm-extra-event",
                lambda value: writer_mapping_lease(value)[
                    "two_stage_writer_authority"
                ].__setitem__(
                    "absent_shm_transition",
                    "accept-any-event-when-the-final-object-matches",
                ),
            ),
            (
                "writer-mapping-lease-preexisting-shm-size-equality-regression",
                lambda value: writer_mapping_lease(value)[
                    "two_stage_writer_authority"
                ].__setitem__(
                    "preexisting_shm_transition",
                    (
                        "require-zero-namespace-events-and-the-exact-same-direct-"
                        "entry-object-mount-and-size-from-pre-stat-through-post-map"
                    ),
                ),
            ),
            (
                "writer-mapping-lease-extend-downgrade-mints",
                lambda value: writer_mapping_lease(value)[
                    "two_stage_writer_authority"
                ]["writer_map_extend_matrix"].__setitem__(
                    "one_zero", "effect-denied-downgrade-may-join"
                ),
            ),
            (
                "writer-mapping-lease-map-before-gate-no-cleanup",
                lambda value: writer_mapping_lease(value)[
                    "two_stage_writer_authority"
                ].pop("map_before_gate_failure_cleanup"),
            ),
            (
                "writer-mapping-lease-gate-before-map-deferred",
                lambda value: writer_mapping_lease(value)[
                    "two_stage_writer_authority"
                ].__setitem__(
                    "gate_before_map_callback_return",
                    "defer-promotion-to-a-later-Store-gate",
                ),
            ),
            (
                "writer-mapping-lease-holder-extend-tuple-equality-regression",
                lambda value: writer_mapping_lease(value)[
                    "two_stage_writer_authority"
                ].__setitem__(
                    "existing_mapping_holder",
                    (
                        "same-page-join-only-to-one-exact-live-generation-with-"
                        "identical-runtime-vfs-file-family-caller-delegated-extend-"
                        "tuple-page-size-pointer-and-zero-size-drift-effect-receipt"
                    ),
                ),
            ),
            (
                "writer-mapping-lease-first-writer-receipt-equality-regression",
                lambda value: writer_mapping_lease(value)[
                    "two_stage_writer_authority"
                ].__setitem__(
                    "simultaneous_first_writers",
                    (
                        "first-complete-authenticated-post-map-installs-the-generation-"
                        "and-each-later-first-writer-cohort-callback-joins-only-that-"
                        "same-generation-after-exact-pointer-page-size-range-and-"
                        "receipt-equality-otherwise-it-cleans-up-and-fails"
                    ),
                ),
            ),
            (
                "writer-mapping-lease-cross-alias-lifetime-equality",
                lambda value: writer_mapping_lease(value)[
                    "process_global_registry"
                ].__setitem__(
                    "lifetime_identity_equality",
                    "required-as-a-cross-alias-cohort-key",
                ),
            ),
            (
                "writer-mapping-lease-duplicate-target-fd",
                lambda value: writer_mapping_lease(value)[
                    "identity_receipt"
                ].__setitem__(
                    "target_handle_discipline",
                    "open-and-retain-duplicate-target-FDs",
                ),
            ),
            (
                "writer-mapping-lease-generation-extend-pair-regression",
                lambda value: writer_mapping_lease(value)["identity_receipt"].__setitem__(
                    "mapping",
                    (
                        "exact-caller-and-delegated-extend-pair-page-number-page-size-"
                        "byte-offset-range-native-nonnull-pointer-post-writer-map-shm-"
                        "size-generation-mapping-page-set-and-writer-holder-generation"
                    ),
                ),
            ),
            (
                "writer-mapping-lease-matrix-missing-one-one-to-zero-zero-join",
                lambda value: writer_mapping_lease(value)["fail_closed_matrix"][
                    "positive"
                ].remove(
                    "valid-one-one-holder-to-zero-zero-holder-same-generation-join"
                ),
            ),
            (
                "writer-mapping-lease-matrix-missing-zero-zero-to-one-one-join",
                lambda value: writer_mapping_lease(value)["fail_closed_matrix"][
                    "positive"
                ].remove(
                    "valid-zero-zero-holder-to-one-one-holder-same-generation-join"
                ),
            ),
            (
                "writer-mapping-lease-same-thread-wait",
                lambda value: writer_mapping_lease(value)[
                    "reader_lifetime"
                ].__setitem__(
                    "same_thread_reentrant_retirement",
                    "block-until-the-reader-callback-finishes",
                ),
            ),
            (
                "writer-mapping-lease-successor-with-handoff",
                lambda value: writer_mapping_lease(value)[
                    "generation_and_races"
                ].__setitem__(
                    "successor_while_handoff_live",
                    "allow-new-generation-when-pointer-matches",
                ),
            ),
            (
                "writer-mapping-lease-successor-same-page-only-regression",
                lambda value: writer_mapping_lease(value)[
                    "generation_and_races"
                ].__setitem__(
                    "successor_while_handoff_live",
                    (
                        "forbid-writer-attempt-pending-promotion-and-new-generation-"
                        "for-the-exact-family-page-until-all-retired-or-retiring-"
                        "generation-reader-handoffs-drain"
                    ),
                ),
            ),
            (
                "writer-mapping-lease-matrix-missing-different-page-successor",
                lambda value: writer_mapping_lease(value)["fail_closed_matrix"][
                    "required"
                ].remove(
                    "W1-G1-reader-handoff-live-W2-different-page-successor-rejection"
                ),
            ),
            (
                "writer-mapping-lease-successor-retryable",
                lambda value: writer_mapping_lease(value)[
                    "generation_and_races"
                ].__setitem__(
                    "successor_outward_result",
                    "outer-SQLITE_BUSY-and-retryable-true",
                ),
            ),
            (
                "writer-mapping-lease-final-size-only",
                lambda value: writer_mapping_lease(value)[
                    "reader_pre_post_receipt"
                ].__setitem__("final_size_equality_alone", "sufficient"),
            ),
            (
                "source-shm-cantinit-transition",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["shm_map_state_machine"].__setitem__(
                    "cantinit_null", "return-ok-null"
                ),
            ),
            (
                "source-shm-readonly-nonnull-preservation",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["shm_map_state_machine"].__setitem__(
                    "readonly_nonnull", "discard-native-mapping"
                ),
            ),
            (
                "source-shm-writer-attach-transition",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["shm_map_state_machine"].__setitem__(
                    "expected_writer_attach_transition", "cantinit-is-permanent"
                ),
            ),
            (
                "source-shm-readonly-null-normalization",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["shm_map_state_machine"].__setitem__(
                    "readonly_null", "preserve-readonly-null"
                ),
            ),
            (
                "source-shm-permanent-delegation-latch",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["shm_map_state_machine"].__setitem__(
                    "permanent_delegation_suppression", "allowed-after-cantinit"
                ),
            ),
            (
                "source-shm-generic-nonprofile-ok",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["shm_map_state_machine"].__setitem__(
                    "generic_nonprofile_extend_zero_ok", "translate-to-readonly"
                ),
            ),
            (
                "source-shm-unmap-reset",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["shm_map_state_machine"].__setitem__(
                    "reset", "reset-on-any-xShmUnmap-attempt"
                ),
            ),
            (
                "source-shm-same-connection",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["heap_wal_index_recovery"].__setitem__(
                    "connection", "close-and-reopen-private-copy"
                ),
            ),
            (
                "source-shm-read-lock-zero",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["heap_wal_index_recovery"].__setitem__(
                    "lock", "decode-after-releasing-WAL_READ_LOCK-0"
                ),
            ),
            (
                "source-shm-post-close-fallback",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["forbidden_fallbacks"].remove(
                    "post-close-endpoint-or-digest-only-private-copy"
                ),
            ),
            (
                "source-shm-different-connection-fallback",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["forbidden_fallbacks"].remove(
                    "different-connection-reopen"
                ),
            ),
            (
                "source-shm-arbitrary-error-fallback",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"]["forbidden_fallbacks"].remove(
                    "arbitrary-sqlite-error-fallback"
                ),
            ),
            (
                "source-shm-source-effect-allowance",
                lambda value: value["compatibility"]["predecessor_v2"]
                ["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"].__setitem__(
                    "source_effects", "allow-shm-resize"
                ),
            ),
            (
                "all-profile-open-non-ok-cleanup",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "sqlite_open_profiles"
                ]["common_non_ok_return_cleanup"].__setitem__(
                    "applicability", "writers-only"
                ),
            ),
            (
                "open-profile-census",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "sqlite_open_profiles"
                ].pop("wal_only_private_recovery"),
            ),
            (
                "accepted-empty-normalization-open-profile",
                lambda value: value["transaction"]["connection_lifecycle"]
                ["sqlite_open_profiles"]["accepted_empty_normalization"]
                ["main_flags"].append("SQLITE_OPEN_CREATE"),
            ),
            (
                "null-open-handle-cleanup",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "sqlite_open_profiles"
                ]["common_non_ok_return_cleanup"].__setitem__(
                    "null_handle", "call-close-v2-on-null"
                ),
            ),
            (
                "nonnull-open-handle-cleanup",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "sqlite_open_profiles"
                ]["common_non_ok_return_cleanup"].__setitem__(
                    "nonnull_handle", "return-without-close"
                ),
            ),
            (
                "open-close-non-ok-quarantine",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "sqlite_open_profiles"
                ]["common_non_ok_return_cleanup"].__setitem__(
                    "close_non_ok_or_unknown", "reopen-and-retry"
                ),
            ),
            (
                "existing-open-failure-common-cleanup",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "existing_v3"
                ].__setitem__("failure_effects", "return-without-common-cleanup"),
            ),
            (
                "fresh-open-failure-common-cleanup",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "fresh_or_empty"
                ].__setitem__("open_failure_effects", "return-without-close"),
            ),
            (
                "bootstrap-open-failure-common-cleanup",
                lambda value: value["transaction"]["fresh_v3_initialization"][
                    "guards"
                ]["filesystem"]["nonexistent_creation_bootstrap"].__setitem__(
                    "environmental_create_or_open_failure_effects",
                    "retry-target-open",
                ),
            ),
            (
                "fresh-phase-precedence",
                lambda value: value["runtime"]["capability_preflight"][
                    "error_precedence"
                ]["filesystem_fresh"].remove("bound-vfs-read-write-mode-proof"),
            ),
            (
                "poutflags-proof",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "read_write_fallback_to_read_only"
                ].__setitem__("proof", "trust-sqlite-open-success"),
            ),
            (
                "poutflags-order",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "read_write_fallback_to_read_only"
                ].__setitem__("timing", "after-limit-and-lock"),
            ),
            (
                "poutflags-local-zero-init",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "read_write_fallback_to_read_only"
                ].__setitem__("local_out_flags_before_call", "uninitialized"),
            ),
            (
                "bound-vfs-open-observation",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "vfs_binding"
                ]["filesystem_source"].__setitem__(
                    "readwrite_open_observation", "input-flags-only"
                ),
            ),
            (
                "bound-vfs-success-only-output-observation",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "vfs_binding"
                ]["filesystem_source"]["required_observation_capability"][
                    "required_operations"
                ].pop(),
            ),
            (
                "runtime-recheck-pre-receipt-phase",
                lambda value: value["runtime"]["capability_preflight"][
                    "recheck_failure_effects"
                ].__setitem__(
                    "filesystem_before_sealed_operation_or_recovery_receipt",
                    "run-terminal-reclassifier",
                ),
            ),
            (
                "runtime-recheck-post-receipt-phase",
                lambda value: value["runtime"]["capability_preflight"][
                    "recheck_failure_effects"
                ].__setitem__(
                    "filesystem_after_sealed_receipt_or_coordination_effect",
                    "return-gate-error-without-phase-classifier",
                ),
            ),
            (
                "runtime-recheck-epoch-delegation",
                lambda value: value["runtime"]["capability_preflight"][
                    "recheck_failure_effects"
                ].__setitem__(
                    "after_begin_immediate_epoch_check", "return-generic-error"
                ),
            ),
            (
                "runtime-recheck-memory-phase",
                lambda value: value["runtime"]["capability_preflight"][
                    "recheck_failure_effects"
                ].__setitem__(
                    "ephemeral_memory_before_authority",
                    "filesystem-reopen-and-reclassifier",
                ),
            ),
            (
                "epoch-drift-publish-delegation",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "mutation_epoch_recheck"
                ]["drift_effects"].__setitem__(
                    "filesystem_current_v3_publish", "return-drift-directly"
                ),
            ),
            (
                "epoch-drift-compaction-delegation",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "mutation_epoch_recheck"
                ]["drift_effects"].__setitem__(
                    "filesystem_current_v3_compaction", "return-drift-directly"
                ),
            ),
            (
                "epoch-drift-fresh-delegation",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "mutation_epoch_recheck"
                ]["drift_effects"].__setitem__(
                    "fresh_filesystem_initialization", "return-drift-directly"
                ),
            ),
            (
                "epoch-drift-migration-delegation",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "mutation_epoch_recheck"
                ]["drift_effects"].__setitem__(
                    "migration_or_preauthority_recovery", "return-drift-directly"
                ),
            ),
            (
                "epoch-drift-memory-delegation",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "mutation_epoch_recheck"
                ]["drift_effects"].__setitem__(
                    "ephemeral_memory_initialization_or_mutation",
                    "filesystem-reclassifier",
                ),
            ),
            (
                "committed-generation-empty-tag",
                lambda value: value["transaction"]["recovery_model"][
                    "authority_state_projection"
                ]["committed_generation_maximum"].__setitem__(
                    "empty_committed_set", "integer-zero"
                ),
            ),
            (
                "committed-generation-zero-distinction",
                lambda value: value["transaction"]["recovery_model"][
                    "authority_state_projection"
                ]["committed_generation_maximum"].__setitem__(
                    "zero_distinction", "some-zero-equals-none"
                ),
            ),
            (
                "committed-generation-equation-only-zero",
                lambda value: value["transaction"]["recovery_model"][
                    "authority_state_projection"
                ]["committed_generation_maximum"].__setitem__(
                    "equation_only_origin", "canonical-state-integer-zero"
                ),
            ),
            (
                "committed-generation-malformed-tag",
                lambda value: value["transaction"]["recovery_model"][
                    "authority_state_projection"
                ]["committed_generation_maximum"].__setitem__(
                    "malformed", "coerce-to-zero"
                ),
            ),
            (
                "stable-unreadable-sidecar",
                lambda value: value["compatibility"]["predecessor_v2"][
                    "read_path_strategy"
                ]["unreadable_sidecar_pair"].__setitem__(
                    "detail", "incomplete-wal-shm-pair"
                ),
            ),
            (
                "stable-unreadable-wal-only",
                lambda value: value["transaction"]["fresh_v3_initialization"][
                    "guards"
                ]["filesystem"]["precreate_census"]["unreadable_wal_only"].__setitem__(
                    "detail", "unrecognized-preauthority-state"
                ),
            ),
            (
                "canonical-byte-collision-guard",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ].__setitem__("state_equality", "sha256-digest-equality"),
            ),
            (
                "compressed-normal-form-bound",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"].__setitem__(
                    "generation-distance-loop-or-uncompressed-edge-replay", "allowed"
                ),
            ),
            (
                "existential-candidate-acceptance",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"].__setitem__(
                    "candidate_acceptance", "accept-canonical-candidate-only"
                ),
            ),
            (
                "canonical-reporting-only",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"].__setitem__(
                    "canonical_witness_selection", "operation-result-authority"
                ),
            ),
            (
                "existential-operation-edge-bits",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"].__setitem__(
                    "operation_edge_predicates", "canonical-count-vector-only"
                ),
            ),
            (
                "candidate-case-migration-last",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"]["candidate_cases"].__setitem__(
                    "v2_to_v3_migration_last_m", "split-generation-distance"
                ),
            ),
            (
                "candidate-case-same-format",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"]["candidate_cases"].__setitem__(
                    "same_format_final_compaction_k", "unbounded-replay"
                ),
            ),
            (
                "candidate-case-final-v3",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"]["candidate_cases"].__setitem__(
                    "v2_to_v3_final_v3_compaction_m_k",
                    "independent-v2-and-v3-generation-distance-splits",
                ),
            ),
            (
                "reverse-format-candidate",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"]["candidate_cases"].__setitem__(
                    "v3_to_v2", "allowed"
                ),
            ),
            (
                "no-reset-target-shape",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"].__setitem__(
                    "no_reset_target_shape", "maximum-only"
                ),
            ),
            (
                "reset-target-shape",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"].__setitem__(
                    "reset_target_shape", "maximum-only"
                ),
            ),
            (
                "migration-zero-population",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"].__setitem__(
                    "zero_population_migration", "allocate-generation-zero"
                ),
            ),
            (
                "migration-boundary-commutation",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"].__setitem__(
                    "format_boundary_commutation_validation", "assumed"
                ),
            ),
            (
                "normalization-completeness",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"].__setitem__(
                    "normalization_completeness", "assumed-from-equation"
                ),
            ),
            (
                "canonical-byte-work-bound",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"].__setitem__(
                    "canonical_byte_work", "repeat-for-every-representation"
                ),
            ),
            (
                "migration-equal-population-boundary",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"]["compact_run_equation"].__setitem__(
                    "final_current_format_boundary", "m-belongs-only-to-v2"
                ),
            ),
            (
                "forced-population-edge-query",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"]["compact_run_equation"].__setitem__(
                    "edge_feature_query", "inspect-canonical-counts-only"
                ),
            ),
            (
                "designated-final-no-coalesce",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"]["compact_run_equation"].__setitem__(
                    "designated_final_edge", "coalesce-with-residual"
                ),
            ),
            (
                "per-format-edge-presence",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"].__setitem__(
                    "compact_edge_presence", "canonical-reporting-witness-only"
                ),
            ),
            (
                "cross-series-rank-invariant",
                lambda value: value["transaction"]["recovery_model"][
                    "authorized_descendant_algebra"
                ]["transition_proof"].__setitem__(
                    "replacement_rank_invariant", "publication-id-order"
                ),
            ),
            (
                "fresh-receipt-profile",
                lambda value: value["transaction"]["recovery_model"][
                    "terminal_reclassification"
                ]["sealed_receipt_profiles"]["fresh_initialization"].pop(),
            ),
            (
                "fresh-empty-class",
                lambda value: value["transaction"]["recovery_model"][
                    "terminal_reclassification"
                ]["sealed_receipt_profiles"].__setitem__(
                    "fresh_empty_anchor_is_not_post_format_authority_state", False
                ),
            ),
            (
                "normalization-operation",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"].__setitem__(
                    "operation", "fresh-initialization"
                ),
            ),
            (
                "normalization-fault-boundary",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"]["typed_control_surface"]
                ["accepted_empty_normalization"].__setitem__(
                    "transition_fault_boundary", "journal-transition"
                ),
            ),
            (
                "normalization-terminal-phase",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"]["typed_control_surface"]
                ["accepted_empty_normalization"].__setitem__(
                    "terminal_phase", "journal-transition"
                ),
            ),
            (
                "normalization-distinct-terminal-operation-selector",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_result_precedence"].__setitem__(
                    "accepted-empty-normalization",
                    value["transaction"]["recovery_model"]
                    ["terminal_result_precedence"]["initialization"],
                ),
            ),
            (
                "normalization-gate-stage",
                lambda value: value["transaction"]["connection_lifecycle"]
                ["effect_gate_stages"].pop(
                    "accepted_empty_normalization_coordination"
                ),
            ),
            (
                "normalization-old-coordination-expansion",
                lambda value: value["transaction"]["connection_lifecycle"]
                ["effect_gate_stages"]["wal_shm_coordination_only"].__setitem__(
                    "accepted_empty_normalization_coordination", "allowed"
                ),
            ),
            (
                "normalization-receipt-profile",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"]["sealed_receipt_profiles"]
                ["accepted_empty_normalization_source_anchor"].remove(
                    "normalization-id"
                ),
            ),
            (
                "normalization-source-byte-snapshot-receipt",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"]["sealed_receipt_profiles"]
                ["accepted_empty_normalization_source_anchor"].remove(
                    "immutable-held-pre-main-exact-byte-snapshot-with-length-and-"
                    "streaming-byte-receipt"
                ),
            ),
            (
                "normalization-prefect-coordination-sequence-receipt",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"]["sealed_receipt_profiles"]
                ["accepted_empty_normalization"].pop(1),
            ),
            (
                "normalization-completed-edge-full-sequence-receipt",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"]["sealed_receipt_profiles"]
                ["accepted_empty_normalization_completed_edge"].pop(1),
            ),
            (
                "normalization-source-anchor-before-coordination",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"].__setitem__(
                    "accepted_empty_normalization_source_anchor",
                    "after-coordination-effect",
                ),
            ),
            (
                "normalization-source-anchor-seal-after-pending-request",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"].__setitem__(
                    "accepted_empty_normalization_source_anchor_seal",
                    "immutable-before-installing-the-pending-coordination-request",
                ),
            ),
            (
                "normalization-full-arm-has-moved",
                lambda value: value["transaction"]["connection_lifecycle"]
                ["effect_gate_stages"]["accepted_empty_normalization_coordination"]
                .__setitem__("full_arming_callback", "xLock-then-full-arm"),
            ),
            (
                "normalization-has-moved-local-zero",
                lambda value: value["transaction"]["connection_lifecycle"]
                ["effect_gate_stages"]["accepted_empty_normalization_coordination"]
                .__setitem__(
                    "has_moved_output_totality", "assume-underlying-vfs-writes-output"
                ),
            ),
            (
                "normalization-effect-bounds",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["forbidden_effects"]
                .remove(
                    "any-main-page-outside-the-derived-large-sector-record-set-write"
                ),
            ),
            (
                "normalization-journal-effect-lifecycle",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["allowed_effects"].pop(1),
            ),
            (
                "normalization-journal-sector-profile",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"].pop(
                    "journal_sector_profile"
                ),
            ),
            (
                "normalization-journal-sector-writer-clamp",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["journal_sector_profile"]
                .__setitem__("writer_effective_S", "always-512"),
            ),
            (
                "normalization-journal-sector-runtime-binding",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["journal_sector_profile"]
                .pop("runtime_binding"),
            ),
            (
                "normalization-journal-sector-power-of-two",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["journal_sector_profile"]
                .__setitem__("writer_admission", "any-effective-integer"),
            ),
            (
                "normalization-journal-sector-no-default",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["journal_sector_profile"]
                .__setitem__("no_contract_default", "S-is-always-512"),
            ),
            (
                "normalization-cold-journal-sector-domain",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["journal_sector_profile"]
                .__setitem__("cold_parser_admission", "any-header-sector-size"),
            ),
            (
                "normalization-cold-journal-all-header-consistency",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["journal_sector_profile"]
                .pop("all_header_format_requirement"),
            ),
            (
                "normalization-cold-journal-sector-match",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["journal_sector_profile"]
                .__setitem__("exact_match", "use-pinned-default-512-offsets"),
            ),
            (
                "normalization-cold-journal-header-padding",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["journal_sector_profile"]
                .__setitem__(
                    "header_layout",
                    "exact-28-byte-header-followed-by-authoritative-zero-padding",
                ),
            ),
            (
                "normalization-journal-header-chunks",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["journal_sector_profile"]
                .__setitem__("writer_header_chunks", "one-fixed-512-byte-write"),
            ),
            (
                "normalization-journal-record-layout",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["journal_sector_profile"]
                .__setitem__("page_record_layout", "first-record-starts-at-512"),
            ),
            (
                "normalization-journal-next-header-layout",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["journal_sector_profile"]
                .pop("next_header_layout"),
            ),
            (
                "normalization-journal-checksum-authority",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["journal_sector_profile"]
                .__setitem__("checksum_authority", "complete-byte-authority"),
            ),
            (
                "normalization-effect-grammar-profile-receipt",
                lambda value: normalization(value).pop(
                    "effect_grammar_profile_receipt"
                ),
            ),
            (
                "normalization-bounded-effect-transcript-receipt",
                lambda value: normalization(value).pop(
                    "normalization_bounded_effect_transcript_receipt"
                ),
            ),
            (
                "normalization-coordination-wal-delete-parent-receipt",
                lambda value: normalization(value).pop(
                    "normalizer_coordination_wal_delete_durability_receipt"
                ),
            ),
            (
                "normalization-journal-creation-parent-receipt",
                lambda value: normalization(value).pop(
                    "normalizer_parent_durability_receipt"
                ),
            ),
            (
                "normalization-terminal-journal-delete-parent-receipt",
                lambda value: normalization(value).pop(
                    "normalizer_final_delete_durability_receipt"
                ),
            ),
            (
                "normalization-failure-totality",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["failure_totality"].pop(
                    "after_coordination_effect_before_normalization_receipt_seal"
                ),
            ),
            (
                "normalization-receipt-chain",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"].__setitem__(
                    "handoff", "start-fresh-with-a-new-unbound-anchor"
                ),
            ),
            (
                "normalization-post-main-digest-only-validation",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"].__setitem__(
                    "post_main_validation", "size-and-sha256-only"
                ),
            ),
            (
                "normalization-public-success",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"].__setitem__(
                    "accepted_empty_normalization_public_success", "allowed"
                ),
            ),
            (
                "normalization-candidate-is-success",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"].__setitem__(
                    "accepted_empty_normalization_candidate_identity",
                    "planned-candidate-proves-success",
                ),
            ),
            (
                "normalization-completed-edge-seal-before-close",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"].__setitem__(
                    "accepted_empty_normalization_completed_edge_seal",
                    "before-transition-or-close",
                ),
            ),
            (
                "normalization-edge-early-empty-return",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"]
                ["accepted_empty_normalization_physical_edge"].__setitem__(
                    "exact_empty_early_return_before_physical_edge_test", "allowed"
                ),
            ),
            (
                "normalization-edge-candidate-alone",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"]
                ["accepted_empty_normalization_physical_edge"].__setitem__(
                    "candidate_receipt_presence_alone", "success-authority"
                ),
            ),
            (
                "normalization-counter-basis",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"].__setitem__(
                    "post_main_raw_projection",
                    value["transaction"]["fresh_v3_initialization"]["guards"]
                    ["filesystem"]["precreate_census"]
                    ["preauthority_sidecar_candidate"]
                    ["accepted_empty_original_normalization"]
                    ["post_main_raw_projection"].replace(
                        "pre-change-counter", "pre-version-valid-for"
                    ),
                ),
            ),
            (
                "normalization-counter-modulo",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"].__setitem__(
                    "post_main_raw_projection",
                    value["transaction"]["fresh_v3_initialization"]["guards"]
                    ["filesystem"]["precreate_census"]
                    ["preauthority_sidecar_candidate"]
                    ["accepted_empty_original_normalization"]
                    ["post_main_raw_projection"].replace(
                        "big-endian-u32-modulo-two-to-the-thirty-two-successor",
                        "checked-nonoverflowing-successor",
                    ),
                ),
            ),
            (
                "normalization-pinned-write-version",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"].__setitem__(
                    "post_main_raw_projection", "header-mode-and-counter-only"
                ),
            ),
            (
                "normalization-write-version-encoding-and-api-source",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"].__setitem__(
                    "post_main_raw_projection",
                    value["transaction"]["fresh_v3_initialization"]["guards"]
                    ["filesystem"]["precreate_census"]
                    ["preauthority_sidecar_candidate"]
                    ["accepted_empty_original_normalization"]
                    ["post_main_raw_projection"].replace(
                        "big-endian-u32-sqlite3_libversion_number-from-the-exact-"
                        "pinned-runtime",
                        "host-endian-pinned-runtime-version",
                    ),
                ),
            ),
            (
                "normalization-counter-vectors",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["qualification"]
                ["counter_vectors"].pop(),
            ),
            (
                "rollback-empty-route-precedence",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["rollback_header_exact_empty_candidate"].__setitem__(
                    "precedence", "after-expected-wal"
                ),
            ),
            (
                "rollback-empty-private-profile",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["rollback_header_exact_empty_candidate"].__setitem__(
                    "private_classification", "read-write-source-open"
                ),
            ),
            (
                "rollback-empty-private-close",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["rollback_header_exact_empty_candidate"].pop("private_close"),
            ),
            (
                "normalization-page-size-decode",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"].__setitem__(
                    "page_size_decode", "fixed-4096"
                ),
            ),
            (
                "normalization-whole-main-page-bound",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"].__setitem__(
                    "whole_main_bound", "exactly-one-page"
                ),
            ),
            (
                "normalization-runtime-receipt",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"]["sealed_receipt_profiles"]
                ["accepted_empty_normalization_source_anchor"].remove(
                    "pinned-sqlite-runtime-identity-and-version"
                ),
            ),
            (
                "normalization-sealed-expected-post-digest",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"]["sealed_receipt_profiles"]
                ["accepted_empty_normalization_source_anchor"].pop(),
            ),
            (
                "normalization-page-size-vectors",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["qualification"]
                ["page_size_vectors"].pop(),
            ),
            (
                "normalization-page-size-boundaries",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["qualification"]
                ["page_size_boundary_qualification_required"].pop(),
            ),
            (
                "normalization-journal-sector-pinned-vector",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["qualification"].pop(
                    "journal_sector_pinned_default_vector"
                ),
            ),
            (
                "normalization-journal-sector-parameterized-vectors",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["qualification"]
                ["journal_sector_parameterized_vectors_required"].pop(),
            ),
            (
                "normalization-journal-sector-negative-vectors",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["qualification"]
                ["journal_sector_negative_vectors_required"].remove(
                    "record-offset-derived-from-512-instead-of-S"
                ),
            ),
            (
                "normalization-multi-page-freelist-vector",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["qualification"].pop(
                    "multi_page_freelist_vector"
                ),
            ),
            (
                "rollback-empty-source-recheck",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["rollback_header_exact_empty_candidate"].pop("source_recheck"),
            ),
            (
                "rollback-empty-drift-result",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["rollback_header_exact_empty_candidate"]["drift_result"].__setitem__(
                    "detail", "ignore-drift"
                ),
            ),
            (
                "rollback-empty-private-close-failure",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["rollback_header_exact_empty_candidate"].pop(
                    "private_open_or_close_failure"
                ),
            ),
            (
                "normalization-prearm-summary",
                lambda value: value["runtime"]["capability_preflight"]
                ["pre_arm_synchronous"].__setitem__(
                    "accepted_empty_normalization_stage",
                    "arm-coordination-before-first-exclusive-lock",
                ),
            ),
            (
                "normalization-first-lock-callback",
                lambda value: value["transaction"]["connection_lifecycle"]
                ["effect_gate_stages"]["accepted_empty_normalization_coordination"]
                .__setitem__("coordination_arming_callback", "arm-before-xLock"),
            ),
            (
                "normalization-zero-wal-held-lock",
                lambda value: value["transaction"]["connection_lifecycle"]
                ["effect_gate_stages"]["accepted_empty_normalization_coordination"]
                ["sole_persistent_allowance"].__setitem__(
                    "exclusive_lock", "not-required"
                ),
            ),
            (
                "normalization-second-lock-callback",
                lambda value: value["transaction"]["connection_lifecycle"]
                ["effect_gate_stages"]["accepted_empty_normalization_coordination"]
                .__setitem__(
                    "full_arming_callback", "first-exclusive-callback-full-arm"
                ),
            ),
            (
                "normalization-receipt-sequence-prior",
                lambda value: value["transaction"]["connection_lifecycle"]
                ["effect_gate_stages"]["accepted_empty_normalization_coordination"]
                ["receipt_sequence"].__setitem__(
                    "coordination", "sequence-two-with-no-prerequisite"
                ),
            ),
            (
                "normalization-receipt-sequence-full",
                lambda value: value["transaction"]["connection_lifecycle"]
                ["effect_gate_stages"]["accepted_empty_normalization_coordination"]
                ["receipt_sequence"].__setitem__(
                    "full", "sequence-three-with-no-prerequisite"
                ),
            ),
            (
                "normalization-receipt-sequence-fail-closed",
                lambda value: value["transaction"]["connection_lifecycle"]
                ["effect_gate_stages"]["accepted_empty_normalization_coordination"]
                ["receipt_sequence"].__setitem__(
                    "skipped-duplicate-reordered-or-wrong-prerequisite", "allowed"
                ),
            ),
            (
                "normalization-no-unlock-between-callbacks",
                lambda value: value["transaction"]["connection_lifecycle"]
                ["effect_gate_stages"]["accepted_empty_normalization_coordination"]
                .__setitem__("lock_trace", "unlock-between-exclusive-callbacks"),
            ),
            (
                "normalization-second-callback-receipt-seal",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"].__setitem__(
                    "accepted_empty_normalization_receipt_seal",
                    "in-the-first-exclusive-xLock-callback",
                ),
            ),
            (
                "normalization-skipped-arm-qualification",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]["qualification"]
                ["required"].remove(
                    "skipped-duplicate-reordered-and-wrong-prerequisite-arm-fail-closed"
                ),
            ),
            (
                "normalization-no-truncate-trace",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"].__setitem__(
                    "rollback_journal_effect_trace", "journal-write-sync-close"
                ),
            ),
            (
                "normalization-fixed-512-journal-trace",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"].__setitem__(
                    "rollback_journal_effect_trace",
                    "xWrite-512-at-zero-page-number-one-at-512",
                ),
            ),
            (
                "normalization-parent-directory-sync",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"].__setitem__(
                    "parent_directory_effect_trace", "parent-sync-unobserved"
                ),
            ),
            (
                "normalization-supplied-vfs-parent-sync-qualification",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"]
                ["vfs_parent_sync_admission"].__setitem__(
                    "generic-supplied-vfs-inherits-linux-strace-claim", "allowed"
                ),
            ),
            (
                "normalization-final-journal-sync",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"].__setitem__(
                    "rollback_journal_effect_trace",
                    "journal-delete-without-final-normal-sync",
                ),
            ),
            (
                "normalization-main-sync-mode",
                lambda value: value["transaction"]["fresh_v3_initialization"]
                ["guards"]["filesystem"]["precreate_census"]
                ["preauthority_sidecar_candidate"]
                ["accepted_empty_original_normalization"].__setitem__(
                    "main_effect_trace", "page-one-write-without-normal-sync"
                ),
            ),
            (
                "terminal-unreadable-order",
                lambda value: value["transaction"]["recovery_model"][
                    "terminal_reclassification"
                ]["post_close_sidecar_decision_order"].reverse(),
            ),
            (
                "phase-specific-terminal-result",
                lambda value: value["transaction"]["recovery_model"][
                    "terminal_result_precedence"
                ]["publish"].__setitem__(
                    "commit-outcome-unknown", "use-generic-terminal-result"
                ),
            ),
            (
                "pre-journal-receipt",
                lambda value: value["transaction"]["fresh_v3_initialization"][
                    "guards"
                ]["filesystem"]["journal_transition_atomicity"][
                    "pre_journal_receipt"
                ]["fields"].pop(),
            ),
            (
                "pre-journal-preinit-anchor",
                lambda value: value["transaction"]["fresh_v3_initialization"][
                    "guards"
                ]["filesystem"]["journal_transition_atomicity"][
                    "pre_journal_receipt"
                ]["fields"].remove("preinit-absent-or-exact-empty-anchor"),
            ),
            (
                "post-arm-residue",
                lambda value: value["transaction"]["fresh_v3_initialization"][
                    "guards"
                ]["filesystem"]["journal_transition_atomicity"].__setitem__(
                    "failure_effects", "after-arming-zero-persistent-effect"
                ),
            ),
            (
                "post-arm-close-gate",
                lambda value: value["transaction"]["fresh_v3_initialization"][
                    "guards"
                ]["filesystem"]["journal_transition_atomicity"][
                    "failure_after_arming"
                ].__setitem__("close_ok", "optional-before-reclassifier"),
            ),
            (
                "memory-init-one-close-no-reclassifier",
                lambda value: value["transaction"]["fresh_v3_initialization"][
                    "commit_outcome_unknown"
                ].__setitem__(
                    "ephemeral_memory", "close-then-filesystem-terminal-reclassifier"
                ),
            ),
            (
                "same-logical-different-physical-publish",
                lambda value: value["transaction"]["publish"][
                    "commit_outcome_unknown"
                ]["classifier"].__setitem__(
                    "same_logical_candidate_with_different_authorized_physical_projection",
                    "invalid-or-partial",
                ),
            ),
            (
                "publish-outcome-precedence",
                lambda value: value["transaction"]["publish"][
                    "commit_outcome_unknown"
                ].__setitem__("result_precedence", "recovered-success-if-present"),
            ),
            (
                "memory-publish-one-close-no-reclassifier",
                lambda value: value["transaction"]["publish"][
                    "commit_outcome_unknown"
                ].__setitem__("ephemeral_memory", "filesystem-reopen-and-reclassify"),
            ),
            (
                "zero-filesystem-close-split",
                lambda value: value["compaction"][
                    "zero_committed_authority_operation"
                ].__setitem__("filesystem_success", "retain-open-connection"),
            ),
            (
                "zero-memory-no-close-split",
                lambda value: value["compaction"][
                    "zero_committed_authority_operation"
                ].__setitem__(
                    "ephemeral_memory_success", "close-and-terminal-reclassify"
                ),
            ),
            (
                "zero-memory-failure-one-close",
                lambda value: value["compaction"][
                    "zero_committed_authority_operation"
                ].__setitem__(
                    "ephemeral_memory_rollback-finalize-or-health-failure",
                    "discard-without-close",
                ),
            ),
            (
                "zero-commit-branch",
                lambda value: value["compaction"][
                    "zero_committed_authority_operation"
                ].__setitem__("ordinary_commit_outcome_unknown_branch", "reachable"),
            ),
            (
                "locked-census-receipt",
                lambda value: value["compaction"]["commit_outcome_unknown"][
                    "recovery_receipt"
                ]["fields"].pop(3),
            ),
            (
                "pre-empty-expected-candidate",
                lambda value: value["compaction"]["commit_outcome_unknown"][
                    "exact_descendant_classifier"
                ].__setitem__(
                    "expected_or_later_compacted",
                    "requires-a-row-in-the-open-time-pre-anchor",
                ),
            ),
            (
                "locked-census-compaction-witness",
                lambda value: value["compaction"]["commit_outcome_unknown"][
                    "exact_descendant_classifier"
                ].__setitem__(
                    "observable_compaction", "requires-open-time-pre-anchor-row"
                ),
            ),
            (
                "memory-compaction-one-close-no-reclassifier",
                lambda value: value["compaction"]["commit_outcome_unknown"].__setitem__(
                    "ephemeral_memory", "close-reopen-and-terminal-reclassify"
                ),
            ),
        ]

        for name, mutate in mutations:
            with self.subTest(drift=name):
                changed = copy.deepcopy(self.contract)
                mutate(changed)
                with self.assertRaisesRegex(
                    SQLiteStoreContractError, "sqlite.option-a-contract-invalid"
                ):
                    validate_option_a_contract(changed)

    def test_every_open_profile_has_total_non_ok_cleanup(self) -> None:
        for profile in SQLITE_OPEN_PROFILE_NAMES:
            with self.subTest(profile=profile, handle="null"):
                self.assertEqual(
                    sqlite_open_non_ok_cleanup(
                        profile, handle_present=False, close_result=None
                    ),
                    {
                        "close_attempts": 0,
                        "quarantined": False,
                        "retry": False,
                        "reclassifier": False,
                        "result": "selected-profile-open-error",
                    },
                )
            with self.subTest(profile=profile, handle="nonnull-close-ok"):
                closed = sqlite_open_non_ok_cleanup(
                    profile, handle_present=True, close_result="ok"
                )
                self.assertEqual(closed["close_attempts"], 1)
                self.assertFalse(closed["quarantined"])
                self.assertFalse(closed["retry"])
            with self.subTest(profile=profile, handle="nonnull-close-non-ok"):
                quarantined = sqlite_open_non_ok_cleanup(
                    profile, handle_present=True, close_result="non-ok"
                )
                self.assertEqual(quarantined["close_attempts"], 1)
                self.assertTrue(quarantined["quarantined"])
                self.assertFalse(quarantined["retry"])
            with self.subTest(profile=profile, handle="nonnull-close-unknown"):
                unknown = sqlite_open_non_ok_cleanup(
                    profile, handle_present=True, close_result="unknown"
                )
                self.assertEqual(unknown["close_attempts"], 1)
                self.assertTrue(unknown["quarantined"])
                self.assertFalse(unknown["retry"])

        with self.assertRaisesRegex(
            SQLiteStoreContractError, "sqlite.open-cleanup-invalid"
        ):
            sqlite_open_non_ok_cleanup(
                "unknown", handle_present=False, close_result=None
            )
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "sqlite.open-cleanup-invalid"
        ):
            sqlite_open_non_ok_cleanup(
                SQLITE_OPEN_PROFILE_NAMES[0],
                handle_present=True,
                close_result=None,
            )

    def test_normalization_journal_sector_layout_is_parameterized(self) -> None:
        self.assertEqual(effective_sqlite_sector_size(16), 512)
        self.assertEqual(effective_sqlite_sector_size(4096), 4096)
        self.assertEqual(effective_sqlite_sector_size(131_072), 65_536)
        self.assertEqual(
            effective_sqlite_sector_size(4096, powersafe_overwrite=True),
            512,
        )
        self.assertEqual(
            normalization_journal_layout(
                raw_xsector_size=4096,
                decoded_page_size=8192,
            ),
            {
                "sector_size": 4096,
                "header_offset": 0,
                "header_field_end": 28,
                "header_padding_begin": 28,
                "header_padding_end": 4096,
                "header_write_length": 4096,
                "header_chunk_size": 4096,
                "header_chunk_count": 1,
                "record_index": 0,
                "record_size": 8200,
                "page_number_offset": 4096,
                "page_image_offset": 4100,
                "checksum_offset": 12_292,
                "record_end": 12_296,
                "next_header_offset": 16_384,
            },
        )
        header = bytearray(4096)
        header[20:24] = (4096).to_bytes(4, "big")
        header[24:28] = (8192).to_bytes(4, "big")
        header[28] = 1
        header[-1] = 0xA5
        self.assertEqual(
            parse_normalization_journal_header_sector(
                bytes(header),
                expected_sector_size=4096,
                expected_page_size=8192,
            )["page_number_offset"],
            4096,
        )
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "journal sector size mismatch"
        ):
            parse_normalization_journal_header_sector(
                bytes(header[:-1]),
                expected_sector_size=4096,
                expected_page_size=8192,
            )
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "journal sector size mismatch"
        ):
            parse_normalization_journal_header_sector(
                bytes(header) + b"\0",
                expected_sector_size=4096,
                expected_page_size=8192,
            )
        with self.assertRaisesRegex(
            SQLiteStoreContractError,
            "effective journal sector size is not a power of two",
        ):
            normalization_journal_layout(
                raw_xsector_size=48,
                decoded_page_size=8192,
            )
        large = normalization_large_sector_record_set(
            raw_xsector_size=65_536,
            decoded_page_size=4096,
            database_page_count=66,
        )
        self.assertEqual(large["record_pages"], tuple(range(1, 17)))
        self.assertEqual(large["record_count"], 16)
        self.assertEqual(large["journal_size"], 131_200)
        self.assertEqual(large["main_write_offsets"], tuple(range(0, 65_536, 4096)))
        self.assertGreater(large["locking_page"], large["pages_per_sector"])
        for page_count, expected_count in ((1, 1), (15, 15), (16, 16), (17, 16)):
            self.assertEqual(
                normalization_large_sector_record_set(
                    raw_xsector_size=65_536,
                    decoded_page_size=4096,
                    database_page_count=page_count,
                )["record_count"],
                expected_count,
            )
        self.assertFalse(
            normalization_record_pages_match(
                raw_xsector_size=65_536,
                decoded_page_size=4096,
                database_page_count=16,
                observed_pages=large["record_pages"] + (large["locking_page"],),
            )
        )
        for page_size in (512, 1024, 2048, 4096, 8192, 16_384, 32_768, 65_536):
            for sector_size in tuple(1 << exponent for exponent in range(5, 17)):
                pages_per_sector = (
                    sector_size // page_size if sector_size > page_size else 1
                )
                locking_page = (0x4000_0000 // page_size) + 1
                self.assertGreater(locking_page, pages_per_sector)
                for page_count in {
                    value
                    for value in (
                        1,
                        pages_per_sector - 1,
                        pages_per_sector,
                        pages_per_sector + 1,
                    )
                    if value > 0
                }:
                    expected = tuple(
                        range(1, min(page_count, pages_per_sector) + 1)
                    )
                    self.assertEqual(
                        normalization_large_sector_record_set(
                            raw_xsector_size=sector_size,
                            decoded_page_size=page_size,
                            database_page_count=page_count,
                        )["record_pages"],
                        expected,
                    )
        page_image = bytes((index * 37 + 11) & 0xFF for index in range(4096))
        nonce = 0x1234_5678
        sparse_sum = sum(
            page_image[index] for index in range(4096 - 200, 0, -200)
        )
        checksum = ((nonce + sparse_sum) & 0xFFFF_FFFF).to_bytes(4, "big")
        self.assertEqual(
            normalization_reconstruct_pager_nonce(checksum, page_image),
            nonce,
        )

    def test_readwrite_open_observation_is_zero_initialized_and_success_only(
        self,
    ) -> None:
        writer_input = frozenset(
            {"SQLITE_OPEN_MAIN_DB", "SQLITE_OPEN_READWRITE"}
        )
        # Output flags need not echo the input flags; READONLY absence after a
        # successful xOpen is the exact mode proof.
        self.assertEqual(
            readwrite_open_observation(
                input_flags=writer_input,
                local_out_flags_before_call=0,
                sqlite_result="ok",
                returned_out_flags=frozenset(),
            ),
            "read-write-proved",
        )
        self.assertEqual(
            readwrite_open_observation(
                input_flags=writer_input,
                local_out_flags_before_call=0,
                sqlite_result="ok",
                returned_out_flags=frozenset({"SQLITE_OPEN_READONLY"}),
            ),
            "store.sqlite-failure/open/read-write-required",
        )
        self.assertEqual(
            readwrite_open_observation(
                input_flags=writer_input,
                local_out_flags_before_call=0,
                sqlite_result="non-ok",
                returned_out_flags=None,
            ),
            "open-non-ok-no-returned-flags",
        )
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "sqlite.open-observation-invalid"
        ):
            readwrite_open_observation(
                input_flags=writer_input,
                local_out_flags_before_call=7,
                sqlite_result="ok",
                returned_out_flags=frozenset(),
            )
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "sqlite.open-observation-invalid"
        ):
            readwrite_open_observation(
                input_flags=writer_input,
                local_out_flags_before_call=0,
                sqlite_result="non-ok",
                returned_out_flags=frozenset({"SQLITE_OPEN_READONLY"}),
            )

    def test_committed_generation_maximum_has_distinct_none_and_some_tags(
        self,
    ) -> None:
        tagged_none = committed_generation_maximum(())
        tagged_some_zero = committed_generation_maximum((0,))
        self.assertEqual(tagged_none, ("none",))
        self.assertEqual(tagged_some_zero, ("some", 0))
        self.assertNotEqual(tagged_none, tagged_some_zero)
        self.assertNotEqual(
            canonical_json(tagged_none), canonical_json(tagged_some_zero)
        )
        self.assertEqual(
            validate_committed_generation_maximum(
                tagged_none, committed_row_count=0
            ),
            0,
        )
        self.assertEqual(
            validate_committed_generation_maximum(
                tagged_some_zero, committed_row_count=1
            ),
            0,
        )
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "sqlite.generation-maximum-invalid"
        ):
            validate_committed_generation_maximum(
                tagged_none, committed_row_count=1
            )
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "sqlite.generation-maximum-invalid"
        ):
            validate_committed_generation_maximum(
                tagged_some_zero, committed_row_count=0
            )

    def test_structural_candidates_are_existential_and_reporting_is_canonical(
        self,
    ) -> None:
        same_format_no_reset = scalar_authorized_descendant_summary(
            source_format="v3",
            target_format="v3",
            source_row_count=1,
            source_generation_maximum=("some", 1),
            target_row_count=2,
            target_generation_maximum=("some", 2),
        )
        self.assertTrue(same_format_no_reset["accepted"])
        self.assertEqual(
            same_format_no_reset["canonical_reporting_witness"]["case"],
            "same_format_no_reset",
        )

        same_format_final = scalar_authorized_descendant_summary(
            source_format="v3",
            target_format="v3",
            source_row_count=1,
            source_generation_maximum=("some", 1),
            target_row_count=1,
            target_generation_maximum=("some", 2),
        )
        self.assertEqual(
            same_format_final["canonical_reporting_witness"]["case"],
            "same_format_final_compaction_k",
        )
        self.assertTrue(same_format_final["edge_presence"]["v3-compact"])

        migration_last = scalar_authorized_descendant_summary(
            source_format="v2",
            target_format="v3",
            source_row_count=1,
            source_generation_maximum=("some", 1),
            target_row_count=1,
            target_generation_maximum=("some", 2),
        )
        self.assertEqual(
            migration_last["canonical_reporting_witness"]["case"],
            "v2_to_v3_migration_last_m",
        )
        self.assertEqual(
            migration_last["edge_presence"],
            {
                "legacy-v2-compact": False,
                "migrate-v2-v3": True,
                "v3-compact": False,
            },
        )

        zero_population_migration = scalar_authorized_descendant_summary(
            source_format="v2",
            target_format="v3",
            source_row_count=0,
            source_generation_maximum=("none",),
            target_row_count=0,
            target_generation_maximum=("none",),
        )
        self.assertEqual(
            zero_population_migration["canonical_reporting_witness"][
                "migration_population"
            ],
            0,
        )
        self.assertFalse(
            zero_population_migration["edge_presence"]["legacy-v2-compact"]
        )
        self.assertFalse(
            zero_population_migration["edge_presence"]["v3-compact"]
        )

        rejected = scalar_authorized_descendant_summary(
            source_format="v3",
            target_format="v3",
            source_row_count=1,
            source_generation_maximum=("some", 1),
            target_row_count=1,
            target_generation_maximum=("some", 0),
        )
        self.assertFalse(rejected["accepted"])
        self.assertIsNone(rejected["canonical_reporting_witness"])

        reverse_format = scalar_authorized_descendant_summary(
            source_format="v3",
            target_format="v2",
            source_row_count=1,
            source_generation_maximum=("some", 1),
            target_row_count=1,
            target_generation_maximum=("some", 1),
        )
        self.assertFalse(reverse_format["accepted"])

    def test_migration_boundary_edge_bits_range_over_all_representations(
        self,
    ) -> None:
        boundary = scalar_authorized_descendant_summary(
            source_format="v2",
            target_format="v3",
            source_row_count=1,
            source_generation_maximum=("some", 1),
            target_row_count=2,
            target_generation_maximum=("some", 6),
        )
        candidate = next(
            value
            for value in boundary["accepted_candidates"]
            if value["case"] == "v2_to_v3_final_v3_compaction_m_k"
            and value["migration_population"] == 1
            and value["last_reset_population"] == 2
        )
        # Population m is assigned to v2 in the canonical reporting schedule,
        # but the forced edge query admits the commuting representation on each
        # side of the migration boundary.
        self.assertEqual(
            candidate["format_tagged_residual_schedule"],
            (("legacy-v2-compact", 1, 1),),
        )
        self.assertEqual(
            candidate["forced_edge_populations"],
            {"legacy-v2-compact": (1,), "v3-compact": (1,)},
        )
        self.assertTrue(forced_population_reachable(1, 1, 2, 1))

    def test_exact_two_representation_counterexample_sets_both_compact_bits(
        self,
    ) -> None:
        # Source v2 S=1/G=1 -> target v3 R=1/G=3 is reachable either
        # compact(v2)+migrate or migrate+compact(v3).
        summary = scalar_authorized_descendant_summary(
            source_format="v2",
            target_format="v3",
            source_row_count=1,
            source_generation_maximum=("some", 1),
            target_row_count=1,
            target_generation_maximum=("some", 3),
        )
        self.assertEqual(
            summary["canonical_reporting_witness"]["case"],
            "v2_to_v3_migration_last_m",
        )
        self.assertEqual(
            summary["edge_presence"],
            {
                "legacy-v2-compact": True,
                "migrate-v2-v3": True,
                "v3-compact": True,
            },
        )
        self.assertEqual(len(summary["accepted_candidates"]), 2)

    def test_huge_generation_distance_with_one_row_stays_closed_form(self) -> None:
        summary = scalar_authorized_descendant_summary(
            source_format="v3",
            target_format="v3",
            source_row_count=1,
            source_generation_maximum=("some", 1),
            target_row_count=1,
            target_generation_maximum=("some", (1 << 63) + 1),
        )
        self.assertEqual(len(summary["accepted_candidates"]), 1)
        witness = summary["canonical_reporting_witness"]
        self.assertEqual(
            witness["residual_schedule"], ((1, (1 << 63) - 1),)
        )
        self.assertEqual(witness["designated_final_edge"], ("v3-compact", 1))

    def test_compressed_normal_form_vectors_are_bounded_and_exact(self) -> None:
        self.assertEqual(
            compressed_compaction_schedule(1 << 63, 1, 1),
            (((1, 1 << 63),), None),
        )
        self.assertEqual(
            compressed_compaction_schedule(
                11, 2, 4, designated_final_population=3
            ),
            (((4, 2),), 3),
        )
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "sqlite.normal-form-unreachable"
        ):
            compressed_compaction_schedule(1, 2, 4)
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "sqlite.normal-form-unreachable"
        ):
            compressed_compaction_schedule(
                3, 2, 4, designated_final_population=4
            )

    def test_cross_series_and_format_reset_vectors_are_deterministic(self) -> None:
        rows = [
            {"sequence": 4, "generation": 12, "publication_id": "X"},
            {"sequence": 4, "generation": 11, "publication_id": "Y"},
        ]
        self.assertEqual(canonical_replacement_order(rows), ("Y", "X"))
        self.assertEqual(
            canonical_replacement_order(list(reversed(rows))), ("Y", "X")
        )
        self.assertEqual(
            format_reset_projection(
                ["legacy-v2-compact", "migrate-v2-v3"]
            ),
            {
                "migration_count": 1,
                "v2_compactions": 1,
                "v3_compactions": 0,
                "last_reset": "migrate-v2-v3",
            },
        )
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "sqlite.normal-form-invalid"
        ):
            format_reset_projection(
                ["migrate-v2-v3", "legacy-v2-compact"]
            )

    def test_canonical_bytes_and_locked_census_own_recovery(self) -> None:
        self.assertTrue(authority_state_bytes_equal(b"same", b"same"))
        # Even an acceleration-digest collision cannot make different canonical
        # states equal; the byte oracle never receives a digest as authority.
        self.assertFalse(authority_state_bytes_equal(b"left", b"right"))
        self.assertEqual(
            compaction_recovery_verdict(
                open_anchor_population=0,
                locked_census_population=1,
                exact_expected_candidate=False,
                compact_edge_populations=(1,),
            ),
            "recovered-success",
        )
        self.assertEqual(
            compaction_recovery_verdict(
                open_anchor_population=0,
                locked_census_population=1,
                exact_expected_candidate=False,
                compact_edge_populations=(),
            ),
            "store.sqlite-failure/database/opaque",
        )
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "sqlite.normal-form-invalid"
        ):
            compaction_recovery_verdict(
                open_anchor_population=2,
                locked_census_population=1,
                exact_expected_candidate=False,
                compact_edge_populations=(1,),
            )

    def test_critical_contract_semantic_drift_is_rejected(self) -> None:
        mutations: list[tuple[str, Mutation]] = [
            (
                "physical-version",
                lambda value: value["physical_format"].__setitem__(
                    "current", "3.0.1"
                ),
            ),
            (
                "format-profile-metadata",
                lambda value: value["physical_format"]["metadata_rows"].__setitem__(
                    "payload_chunk_maximum_bytes", "4194304"
                ),
            ),
            (
                "runtime-floor",
                lambda value: value["runtime"]["limit_length"].__setitem__(
                    "minimum", 8_388_608
                ),
            ),
            (
                "preauthority-v3-runtime-gate",
                lambda value: value["runtime"]["capability_preflight"][
                    "preauthority_wal_only_current_v3_gate"
                ].__setitem__(
                    "source_before_first_mutation",
                    "defer-limit-check-until-after-authority-write",
                ),
            ),
            (
                "actual-connection-write-denying-vfs",
                lambda value: value["runtime"]["capability_preflight"].__setitem__(
                    "actual_connection_vfs_arming",
                    "open-write-enabled-before-capability-and-identity-gates",
                ),
            ),
            (
                "chunk-table",
                lambda value: value["tables"].pop(2),
            ),
            (
                "v3-strict-schema",
                lambda value: value["schema_profiles"]["current_v3"].__setitem__(
                    "strict", False
                ),
            ),
            (
                "v3-canonical-ddl",
                lambda value: value["schema_profiles"]["current_v3"][
                    "canonical_ddl"
                ].__setitem__("digest", "sha256:" + "0" * 64),
            ),
            (
                "v2-schema-census",
                lambda value: value["schema_profiles"]["predecessor_v2"][
                    "exact_user_objects"
                ]["tables"].append("cxxlens_ng_payload_chunk"),
            ),
            (
                "v2-canonical-ddl",
                lambda value: value["schema_profiles"]["predecessor_v2"][
                    "canonical_ddl"
                ].__setitem__("validation_projection", "sqlite-schema-sql-only"),
            ),
            (
                "counter-domain",
                lambda value: value["counter_storage"]["fields"].remove(
                    "chunk_ordinal"
                ),
            ),
            (
                "chunk-size",
                lambda value: value["payload"]["chunk_profile"].__setitem__(
                    "maximum_bytes", 4_194_304
                ),
            ),
            (
                "chunk-count",
                lambda value: value["payload"]["chunk_profile"].__setitem__(
                    "maximum_chunk_count", 2_199_023_255_551
                ),
            ),
            (
                "negative-ordinal",
                lambda value: value["payload"]["chunk_profile"].__setitem__(
                    "ordinal_sqlite_encoding", "signed-int64"
                ),
            ),
            (
                "diagnostic-empty-payload",
                lambda value: value["payload"]["chunk_profile"].__setitem__(
                    "diagnostic_noncommitted_empty_payload", "forbidden"
                ),
            ),
            (
                "dynamic-sql",
                lambda value: value["payload"]["write_api"].__setitem__(
                    "dynamic_values", "sql-string-concatenation"
                ),
            ),
            (
                "hex-payload",
                lambda value: value["payload"]["write_api"].__setitem__(
                    "sql_hex_literal", "allowed"
                ),
            ),
            (
                "bounded-chunk-read",
                lambda value: value["payload"]["write_api"].__setitem__(
                    "payload_read", "sqlite3-column-blob-unbounded-row"
                ),
            ),
            (
                "resident-payload",
                lambda value: value["payload"]["streaming"].__setitem__(
                    "all-payload-vector", "allowed"
                ),
            ),
            (
                "v2-write",
                lambda value: value["compatibility"]["predecessor_v2"].__setitem__(
                    "mutation", "allowed"
                ),
            ),
            (
                "v2-side-effect",
                lambda value: value["compatibility"]["predecessor_v2"].__setitem__(
                    "open_side_effects", "allowed"
                ),
            ),
            (
                "v2-connection-mode",
                lambda value: value["compatibility"]["predecessor_v2"].__setitem__(
                    "connection_mode", "sqlite-open-readwrite"
                ),
            ),
            (
                "v2-file-uri",
                lambda value: value["compatibility"]["predecessor_v2"][
                    "read_path_strategy"
                ]["quiescent"].__setitem__(
                    "user_uri_or_query_passthrough", "allowed"
                ),
            ),
            (
                "v2-shm-authority",
                lambda value: value["compatibility"]["predecessor_v2"][
                    "zero_mutation_oracle"
                ].__setitem__("shm", "exact-bytes-required"),
            ),
            (
                "v2-active-wal-open",
                lambda value: value["compatibility"]["predecessor_v2"][
                    "read_path_strategy"
                ]["active_wal"].__setitem__(
                    "open", "sqlite-open-v2-readonly-default-vfs"
                ),
            ),
            (
                "v2-active-wal-receipt",
                lambda value: value["compatibility"]["predecessor_v2"][
                    "read_path_strategy"
                ]["active_wal"].__setitem__(
                    "stable_source_receipt", "main-bytes-only"
                ),
            ),
            (
                "v2-active-wal-permitted-descendant",
                lambda value: value["compatibility"]["predecessor_v2"][
                    "read_path_strategy"
                ]["active_wal"].__setitem__(
                    "permitted_concurrent_change", "any-wal-prefix-rewrite"
                ),
            ),
            (
                "v2-active-wal-lock-zero-main-binding",
                lambda value: value["compatibility"]["predecessor_v2"][
                    "read_path_strategy"
                ]["active_wal"].__setitem__(
                    "lock_zero", "allow-main-size-or-sha256-drift"
                ),
            ),
            (
                "v2-active-wal-lock-zero-drift-mapping",
                lambda value: value["compatibility"]["predecessor_v2"][
                    "read_path_strategy"
                ]["active_wal"].__setitem__(
                    "replacement_or_drift",
                    "open-handle-replacement-only-is-concurrent-source-change",
                ),
            ),
            (
                "v2-active-wal-lock-n-prefix-binding",
                lambda value: value["compatibility"]["predecessor_v2"][
                    "read_path_strategy"
                ]["active_wal"].__setitem__(
                    "lock_n", "trust-shm-mxFrame-without-open-wal-header"
                ),
            ),
            (
                "v2-quiescent-copy-verification",
                lambda value: value["compatibility"]["predecessor_v2"][
                    "read_path_strategy"
                ]["quiescent"].__setitem__(
                    "verification", "source-digest-before-copy-only"
                ),
            ),
            (
                "v2-persisted-journal-oracle",
                lambda value: value["compatibility"]["predecessor_v2"][
                    "read_path_strategy"
                ]["quiescent"].__setitem__(
                    "persisted_journal_oracle", "private-pragma-journal-mode"
                ),
            ),
            (
                "v2-non-wal-result",
                lambda value: value["compatibility"]["predecessor_v2"][
                    "read_path_strategy"
                ]["quiescent"].__setitem__(
                    "non_wal_result",
                    {
                        "code": "store.format-incompatible",
                        "field": "sqlite-physical-format",
                        "detail": "",
                    },
                ),
            ),
            (
                "v2-zero-mutation-directory-set",
                lambda value: value["compatibility"]["predecessor_v2"][
                    "zero_mutation_oracle"
                ].__setitem__("directory_entry_set", "non-authoritative"),
            ),
            (
                "implicit-migration",
                lambda value: value["compatibility"].__setitem__(
                    "implicit_migration", "allowed"
                ),
            ),
            (
                "unregistered-cross-major-migration",
                lambda value: value["compatibility"][
                    "unknown_major_or_marker"
                ].__setitem__("mutation", "allowed-by-compact"),
            ),
            (
                "registered-predecessor-migration-id",
                lambda value: value["physical_format"]["predecessor"].__setitem__(
                    "migration_id", "compact-any-v2-to-v3"
                ),
            ),
            (
                "connection-probe",
                lambda value: value["transaction"][
                    "connection_lifecycle"
                ].__setitem__("existing_probe", "read-write-create"),
            ),
            (
                "connection-file-uri",
                lambda value: value["transaction"][
                    "connection_lifecycle"
                ]["locator_modes"]["filesystem"].__setitem__(
                    "binding", "caller-file-uri"
                ),
            ),
            (
                "filesystem-canonicalization-order",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "locator_modes"
                ]["filesystem"].__setitem__(
                    "canonicalization_order", "host-realpath-before-vfs-binding"
                ),
            ),
            (
                "filesystem-full-path-authority",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "locator_modes"
                ]["filesystem"].__setitem__(
                    "full_path_authority", "host-std-filesystem-canonical"
                ),
            ),
            (
                "filesystem-canonicalization-failure",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "locator_modes"
                ]["filesystem"].__setitem__(
                    "canonicalization_failure",
                    {
                        "code": "store.format-incompatible",
                        "field": "database_path",
                        "detail": "",
                    },
                ),
            ),
            (
                "ephemeral-memory-locator",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "locator_modes"
                ]["ephemeral_memory"].__setitem__(
                    "filesystem_database_or_sidecars", "allowed"
                ),
            ),
            (
                "empty-locator-effects",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "locator_modes"
                ]["empty"].__setitem__(
                    "database_vfs_or_sidecar_open", "allowed"
                ),
            ),
            (
                "source-vfs-binding",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "vfs_binding"
                ]["filesystem_source"].__setitem__(
                    "reresolve-null-default-or-name-only", "allowed"
                ),
            ),
            (
                "fresh-scratch-memory-vfs-binding",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "vfs_binding"
                ]["fresh_filesystem_scratch_memory"].__setitem__(
                    "vfs", "null-default-reresolution"
                ),
            ),
            (
                "write-transaction-lock-before-recheck",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "mutation_epoch_recheck"
                ]["existing_v3_publish_or_compaction"].__setitem__(
                    "lock", "after-first-authority-write"
                ),
            ),
            (
                "write-transaction-filesystem-wal-recheck",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "mutation_epoch_recheck"
                ]["existing_v3_publish_or_compaction"].__setitem__(
                    "first_check", "connection-pragma-journal-mode-only"
                ),
            ),
            (
                "write-transaction-fresh-filesystem-recheck",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "mutation_epoch_recheck"
                ]["fresh_filesystem_initialization"].__setitem__(
                    "transaction", "write-ddl-before-wal-recheck"
                ),
            ),
            (
                "write-transaction-memory-journal-recheck",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "mutation_epoch_recheck"
                ]["ephemeral_memory_initialization"].__setitem__(
                    "transaction", "accept-wal-or-memory-journal"
                ),
            ),
            (
                "write-transaction-recovery-recheck",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "mutation_epoch_recheck"
                ].__setitem__(
                    "migration_or_preauthority_recovery",
                    "write-before-named-post-lock-revalidation",
                ),
            ),
            (
                "write-transaction-journal-drift-effects",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "mutation_epoch_recheck"
                ].__setitem__(
                    "drift_effects", "continue-with-schema-and-data-write"
                ),
            ),
            (
                "wal-before-validation",
                lambda value: value["transaction"].__setitem__(
                    "journal_mode", "WAL-before-format-validation"
                ),
            ),
            (
                "current-v3-journal-probe",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "existing_v3"
                ].__setitem__("journal_mode_probe", "set-wal-with-fallback"),
            ),
            (
                "current-v3-file-identity",
                lambda value: value["transaction"]["connection_lifecycle"][
                    "existing_v3"
                ].__setitem__(
                    "identity_mismatch",
                    {
                        "code": "store.sqlite-failure",
                        "field": "database",
                        "detail": "opaque",
                    },
                ),
            ),
            (
                "fresh-initialization-marker-last",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["steps"].__setitem__(0, "insert-physical-format-marker-first"),
            ),
            (
                "fresh-nonexistent-bootstrap-lease",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["guards"]["filesystem"][
                    "nonexistent_creation_bootstrap"
                ]["forbidden_before_lease"].remove("database-header-write"),
            ),
            (
                "fresh-initialization-recovery",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["commit_outcome_unknown"].__setitem__(
                    "invalid_partial_or_mixed",
                    {
                        "code": "store.sqlite-failure",
                        "field": "database",
                        "detail": "opaque",
                    },
                ),
            ),
            (
                "fresh-preauthority-stability-receipt",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["guards"]["filesystem"]["precreate_census"][
                    "preauthority_sidecar_candidate"
                ].__setitem__("stable_source_receipt", "main-size-only"),
            ),
            (
                "fresh-preauthority-shm-copy",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["guards"]["filesystem"]["precreate_census"][
                    "preauthority_sidecar_candidate"
                ].__setitem__(
                    "private_shm", "copy-source-shm-into-private-directory"
                ),
            ),
            (
                "fresh-preauthority-wal-participation",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["guards"]["filesystem"]["precreate_census"][
                    "preauthority_sidecar_candidate"
                ]["wal_participation_gate"].__setitem__(
                    "recovered_state_source", "accept-valid-main-without-wal-prefix"
                ),
            ),
            (
                "fresh-preauthority-wal-provenance",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["guards"]["filesystem"]["precreate_census"][
                    "preauthority_sidecar_candidate"
                ]["wal_participation_gate"]["main_alone_logical_anchor"].__setitem__(
                    "cold-factory-with-valid-committed-prefix",
                    "require-main-alone-historical-binding",
                ),
            ),
            (
                "fresh-preauthority-profile-classifier",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["guards"]["filesystem"]["precreate_census"][
                    "preauthority_sidecar_candidate"
                ]["classification"].__setitem__(
                    "base_profile_split", "require-v3-gate-before-v2-classification"
                ),
            ),
            (
                "fresh-preauthority-v2-route",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["guards"]["filesystem"]["precreate_census"][
                    "preauthority_sidecar_candidate"
                ].__setitem__("accepted_v2", "route-to-fresh-v3-initialization"),
            ),
            (
                "fresh-preauthority-v2-recovery",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["guards"]["filesystem"]["precreate_census"][
                    "preauthority_sidecar_candidate"
                ].__setitem__(
                    "accepted_v2_compact_recovery", "migrate-private-copy-only"
                ),
            ),
            (
                "fresh-preauthority-terminal-routing",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["guards"]["filesystem"]["precreate_census"][
                    "preauthority_sidecar_candidate"
                ].__setitem__(
                    "terminal_routing", "return-to-incomplete-sidecar-rejection"
                ),
            ),
            (
                "fresh-preauthority-v3-normalization",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["guards"]["filesystem"]["precreate_census"][
                    "preauthority_sidecar_candidate"
                ].__setitem__(
                    "accepted_current_v3_first_mutation_recovery",
                    "write-before-source-normalization",
                ),
            ),
            (
                "fresh-preauthority-recovery-concurrency",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["guards"]["filesystem"]["precreate_census"][
                    "preauthority_sidecar_candidate"
                ].__setitem__(
                    "special_recovery_concurrency",
                    "accept-any-valid-descendant-before-normal-handoff",
                ),
            ),
            (
                "fresh-preauthority-handoff-poison",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["guards"]["filesystem"]["precreate_census"][
                    "preauthority_sidecar_candidate"
                ]["recovery_failure_totality"].__setitem__(
                    "poisoned_store_instance_state",
                    "continue-with-stale-process-state",
                ),
            ),
            (
                "fresh-preauthority-poison-observers",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["guards"]["filesystem"]["precreate_census"][
                    "preauthority_sidecar_candidate"
                ]["recovery_failure_totality"].__setitem__(
                    "retained_generation_count", "zero"
                ),
            ),
            (
                "active-wal-empty-receipt",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["guards"]["filesystem"]["precreate_census"][
                    "active_wal_empty_preauthority_candidate"
                ].__setitem__("receipt", "main-and-wal-bytes-only"),
            ),
            (
                "active-wal-empty-recovery",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ]["guards"]["filesystem"]["precreate_census"][
                    "active_wal_empty_preauthority_candidate"
                ].__setitem__(
                    "recovery", "initialize-before-normalizing-source"
                ),
            ),
            (
                "fresh-filesystem-handoff",
                lambda value: value["transaction"][
                    "fresh_v3_initialization"
                ].__setitem__(
                    "successful_filesystem_handoff",
                    "install-process-state-before-reopen-validation",
                ),
            ),
            (
                "migration-trigger",
                lambda value: value["migration"].__setitem__(
                    "public_trigger", "implicit-open"
                ),
            ),
            (
                "migration-connection",
                lambda value: value["migration"]["source_connection"].__setitem__(
                    "compact_connection", "reuse-read-only-connection"
                ),
            ),
            (
                "migration-precommit-reopen",
                lambda value: value["migration"]["source_connection"].__setitem__(
                    "precommit_failure", "retain-read-write-connection"
                ),
            ),
            (
                "migration-established-anchor",
                lambda value: value["migration"]["source_connection"].__setitem__(
                    "established_anchor", "heads-only"
                ),
            ),
            (
                "migration-failure",
                lambda value: value["migration"].__setitem__(
                    "failure", "best-effort-partial-v3"
                ),
            ),
            (
                "migration-noncommitted-row-preservation",
                lambda value: value["migration"]["source_row_gate"].__setitem__(
                    "noncommitted_rows", "reject-as-corrupt"
                ),
            ),
            (
                "migration-noncommitted-payload-preservation",
                lambda value: value["migration"]["row_class_migration"][
                    "noncommitted"
                ].__setitem__("payload", "canonical-reencode"),
            ),
            (
                "commit-unknown-retry",
                lambda value: value["migration"][
                    "commit_outcome_unknown"
                ].__setitem__("implicit-retry-write", "allowed"),
            ),
            (
                "commit-unknown-census-mismatch",
                lambda value: value["migration"][
                    "commit_outcome_unknown"
                ]["invalid_source_or_target_census"].__setitem__(
                    "detail", "opaque"
                ),
            ),
            (
                "commit-unknown-valid-descendant",
                lambda value: value["migration"][
                    "commit_outcome_unknown"
                ]["valid_descendant_rule"].__setitem__(
                    "deletion-rewrite-or-nonancestor-head", "allowed"
                ),
            ),
            (
                "migration-reopen-failure-poison",
                lambda value: value["migration"]["reopen_failure"].__setitem__(
                    "store_instance_state", "continue-with-stale-process-state"
                ),
            ),
            (
                "migration-reopen-poison-observers",
                lambda value: value["migration"]["reopen_failure"].__setitem__(
                    "retained_generation_count", "zero"
                ),
            ),
            (
                "migration-final-ddl",
                lambda value: value["migration"].__setitem__(
                    "cold_reopen_ddl_digest", "not-revalidated"
                ),
            ),
            (
                "concurrent-migrator",
                lambda value: value["migration"]["concurrent_migrator"].__setitem__(
                    "format_recheck", "before-begin-immediate"
                ),
            ),
            (
                "durable-old-chunks",
                lambda value: value["compaction"].__setitem__(
                    "durable_old_chunk_reclamation", "after-commit"
                ),
            ),
            (
                "compaction-noncommitted-rows",
                lambda value: value["compaction"].__setitem__(
                    "noncommitted_rows_and_chunks", "drop"
                ),
            ),
            (
                "compaction-locked-anchor",
                lambda value: value["compaction"].__setitem__(
                    "locked_precondition", "heads-only"
                ),
            ),
            (
                "compaction-exact-descendant-classifier",
                lambda value: value["compaction"][
                    "commit_outcome_unknown"
                ]["exact_descendant_classifier"].__setitem__(
                    "authorized_compaction_transition",
                    "any-generation-replacement",
                ),
            ),
            (
                "compaction-empty-anchor-ambiguity",
                lambda value: value["compaction"][
                    "commit_outcome_unknown"
                ]["exact_descendant_classifier"].__setitem__(
                    "zero_pre_anchor_committed_publications",
                    "assume-recovered-success",
                ),
            ),
            (
                "compaction-commit-unknown-retry",
                lambda value: value["compaction"][
                    "commit_outcome_unknown"
                ].__setitem__("implicit_retry_write", "allowed"),
            ),
            (
                "compaction-reopen-failure-poison",
                lambda value: value["compaction"]["reopen_failure"].__setitem__(
                    "store_instance_state", "continue-with-stale-process-state"
                ),
            ),
            (
                "compaction-reopen-poison-observers",
                lambda value: value["compaction"]["reopen_failure"].__setitem__(
                    "retained_generation_count", "zero"
                ),
            ),
            (
                "chunk-corruption",
                lambda value: value["corruption"]["chunk_rejection"].remove(
                    "semantic-tamper-with-recomputed-checksums"
                ),
            ),
            (
                "global-chunk-corruption",
                lambda value: value["corruption"].__setitem__(
                    "global_chunk_authority_result",
                    {
                        "code": "store.publication-corrupt",
                        "field": "publication-id",
                        "detail": "",
                    },
                ),
            ),
            (
                "migration-result",
                lambda value: value["public_error_set"]["additive"].clear(),
            ),
            (
                "materializer-implicit-migration",
                lambda value: value["materializer_binding"].__setitem__(
                    "implicit_migration", "allowed"
                ),
            ),
            (
                "vfs-observation-capability",
                lambda value: value["transaction"]["connection_lifecycle"]
                ["vfs_binding"]["filesystem_source"]
                ["required_observation_capability"].__setitem__(
                    "generic-registered-name-method-table-and-untyped-lifetime-only",
                    "allowed",
                ),
            ),
            (
                "authority-state-projection",
                lambda value: value["transaction"]["recovery_model"]
                ["authority_state_projection"][
                    "diagnostic_noncommitted_projection"
                ].__setitem__("authority", "may-authorize-generation"),
            ),
            (
                "descendant-diagnostic-rewrite",
                lambda value: value["transaction"]["recovery_model"]
                ["authorized_descendant_algebra"]["transition_proof"].__setitem__(
                    "diagnostic-row-add-delete-or-rewrite", "allowed"
                ),
            ),
            (
                "terminal-close-non-ok",
                lambda value: value["transaction"]["recovery_model"]
                ["terminal_reclassification"]["cleanup"].__setitem__(
                    "close-non-ok", "reopen-and-retry"
                ),
            ),
            (
                "zero-row-compaction-commit",
                lambda value: value["compaction"][
                    "zero_committed_authority_operation"
                ].__setitem__("ordinary_commit_outcome_unknown_branch", "reachable"),
            ),
            (
                "mixed-format-fallback",
                lambda value: value["compatibility"]["mixed_v2_v3"].__setitem__(
                    "code", "store.format-incompatible"
                ),
            ),
            (
                "qualification-evidence",
                lambda value: value["release_binding"][
                    "required_sqlite_evidence"
                ].remove("limit-length-exceeding-valid-canonical-v5-reopened-parity"),
            ),
        ]

        for name, mutate in mutations:
            with self.subTest(drift=name):
                changed = copy.deepcopy(self.contract)
                mutate(changed)
                with self.assertRaisesRegex(
                    SQLiteStoreContractError, "sqlite.contract-drift"
                ):
                    validate_exact_contract(changed)

    def test_schema_semantic_drift_is_rejected(self) -> None:
        mutations: list[tuple[str, Mutation]] = [
            (
                "version",
                lambda value: value["properties"]["document_version"].__setitem__(
                    "const", "2.1.0"
                ),
            ),
            (
                "physical-format",
                lambda value: value["$defs"]["physical_format"][
                    "const"
                ].__setitem__("current", "3.0.1"),
            ),
            (
                "chunk-size",
                lambda value: value["$defs"]["payload"]["properties"][
                    "chunk_profile"
                ]["const"].__setitem__("maximum_bytes", 4_194_304),
            ),
            (
                "strict-schema-profile",
                lambda value: value["$defs"]["schema_profiles"]["const"][
                    "current_v3"
                ].__setitem__("strict", False),
            ),
            (
                "v2-write",
                lambda value: value["$defs"]["compatibility"]["const"][
                    "predecessor_v2"
                ].__setitem__("mutation", "allowed"),
            ),
            (
                "source-shm-capability",
                lambda value: value["$defs"]["compatibility"]["const"]
                ["predecessor_v2"]["read_path_strategy"]["active_wal"]
                ["source_shm_readonly_capability"].__setitem__(
                    "id", "sqlite-source-shm-readonly-drift"
                ),
            ),
            (
                "migration-trigger",
                lambda value: value["$defs"]["migration"]["const"].__setitem__(
                    "public_trigger", "implicit-open"
                ),
            ),
            (
                "required-field",
                lambda value: value["required"].remove("release_binding"),
            ),
        ]

        for name, mutate in mutations:
            with self.subTest(drift=name):
                changed = copy.deepcopy(self.schema)
                mutate(changed)
                with self.assertRaisesRegex(
                    SQLiteStoreContractError, "sqlite.schema-drift"
                ):
                    validate_exact_schema(changed)

    def test_json_schema_and_unknown_extension_are_fail_closed(self) -> None:
        invalid_schema = copy.deepcopy(self.schema)
        invalid_schema["type"] = "not-a-json-schema-type"
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "sqlite.schema-invalid"
        ):
            validate_schema_document(invalid_schema)

        extended_contract = copy.deepcopy(self.contract)
        extended_contract["unexpected_extension"] = "allowed"
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "sqlite.schema-invalid"
        ):
            schema_validate(extended_contract, self.schema)

    def test_coordinated_contract_and_schema_drift_is_rejected(self) -> None:
        contract = copy.deepcopy(self.contract)
        schema = copy.deepcopy(self.schema)
        contract["physical_format"]["current"] = "3.0.1"
        schema["$defs"]["physical_format"]["const"]["current"] = "3.0.1"

        # The paired edit is structurally schema-valid, but the independent
        # canonical authority bindings must each still reject it.
        schema_validate(contract, schema)
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "sqlite.contract-drift"
        ):
            validate_exact_contract(contract)
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "sqlite.schema-drift"
        ):
            validate_exact_schema(schema)
        with self.assertRaisesRegex(
            SQLiteStoreContractError, "sqlite.schema-drift"
        ):
            validate_authorities(contract, schema, copy.deepcopy(self.snapshot))

    def test_snapshot_one_way_binding_is_fail_closed(self) -> None:
        mutations: list[tuple[str, Mutation]] = [
            (
                "option-a",
                lambda value: value["df_0200_materialization_ingress"][
                    "sqlite_capacity_decision"
                ].__setitem__("selected_alternative", "B"),
            ),
            (
                "parity",
                lambda value: value["df_0200_materialization_ingress"][
                    "sqlite_capacity_decision"
                ].__setitem__("weakening_parity", "allowed"),
            ),
            (
                "publication-outcome-unknown-retry",
                lambda value: value["df_0200_materialization_ingress"][
                    "sqlite_backend"
                ]["publication_commit_outcome_unknown_recovery"].__setitem__(
                    "implicit_retry", "allowed"
                ),
            ),
            (
                "current-format",
                lambda value: value["format_compatibility"].__setitem__(
                    "current_sqlite", "cxxlens.sqlite-semantic-store.v2-2.6.0"
                ),
            ),
            (
                "semantic-digest",
                lambda value: value["format_compatibility"].__setitem__(
                    "migration_commit_requires_same_semantic_digest", False
                ),
            ),
            (
                "implicit-migration",
                lambda value: value["format_compatibility"].__setitem__(
                    "implicit_migration", "allowed"
                ),
            ),
            (
                "unregistered-migration-chain",
                lambda value: value["format_compatibility"].__setitem__(
                    "migration", "arbitrary-cross-major-upgrade"
                ),
            ),
            (
                "shared-generation-allocation",
                lambda value: value["compaction"][
                    "generation_allocation"
                ].__setitem__(
                    "authority", "sqlite-private-generation-allocator"
                ),
            ),
            (
                "migration-trigger",
                lambda value: value["compaction"][
                    "sqlite_v2_to_v3_migration"
                ].__setitem__("trigger", "implicit-open"),
            ),
            (
                "migration-generation-allocation",
                lambda value: value["compaction"][
                    "sqlite_v2_to_v3_migration"
                ].__setitem__(
                    "generation_allocation", "sqlite-private-range"
                ),
            ),
            (
                "migration-recovery",
                lambda value: value["compaction"][
                    "sqlite_v2_to_v3_migration"
                ].__setitem__("commit_outcome_unknown", "assume-v3-success"),
            ),
            (
                "migration-post-classification-state",
                lambda value: value["compaction"][
                    "sqlite_v2_to_v3_migration"
                ]["commit_outcome_unknown"]["post_classification_state"].__setitem__(
                    "valid_non_descendant_or_invalid_or_mixed",
                    "continue-with-last-process-state",
                ),
            ),
            (
                "v3-compaction-empty-ambiguity",
                lambda value: value["compaction"][
                    "sqlite_v3_compaction_commit_outcome_unknown"
                ].__setitem__(
                    "zero_pre_anchor_committed_publications", "recovered-success"
                ),
            ),
            (
                "v3-compaction-post-classification-state",
                lambda value: value["compaction"][
                    "sqlite_v3_compaction_commit_outcome_unknown"
                ]["post_classification_state"].__setitem__(
                    "valid_non_descendant_or_invalid_or_mixed",
                    "continue-with-last-process-state",
                ),
            ),
            (
                "invalid-filesystem-locator-binding",
                lambda value: value["format_compatibility"].__setitem__(
                    "invalid_filesystem_locator_result",
                    "store.format-incompatible",
                ),
            ),
            (
                "source-shm-capability-binding",
                lambda value: value["format_compatibility"]
                ["sqlite_source_shm_readonly_capability"].__setitem__(
                    "id", "sqlite-source-shm-readonly-drift"
                ),
            ),
            (
                "snapshot-only-normalization-completed-edge-binding",
                lambda value: value["publication_transaction"]
                ["sqlite_terminal_recovery"]["sealed_receipt_profiles"]
                ["accepted_empty_normalization_completed_edge"].__setitem__(
                    -1, "planned-normalization-candidate-id"
                ),
            ),
            (
                "v2-begin-result",
                lambda value: value["df_0200_materialization_ingress"][
                    "compatibility"
                ].__setitem__(
                    "sqlite_predecessor_begin_failure", "store.format-incompatible"
                ),
            ),
            (
                "cursor-success-semantics",
                lambda value: value["df_0200_materialization_ingress"][
                    "compatibility"
                ].__setitem__(
                    "public_cursor_lifetime_and-success-results", "changed"
                ),
            ),
        ]

        for name, mutate in mutations:
            with self.subTest(binding=name):
                changed = copy.deepcopy(self.snapshot)
                mutate(changed)
                with self.assertRaisesRegex(
                    SQLiteStoreContractError, "sqlite.snapshot-binding-drift"
                ):
                    validate_snapshot_binding(self.contract, changed)

    def test_checker_has_no_materialization_reverse_dependency(self) -> None:
        checker = (
            ROOT / "tools/quality/check_ng_sqlite_store_contract.py"
        ).read_text(encoding="utf-8")
        self.assertNotIn("MATERIALIZATION_CONTRACT", checker)
        self.assertNotIn(
            "cxxlens_ng_clang22_materialization_contract.yaml", checker
        )


if __name__ == "__main__":
    unittest.main()
