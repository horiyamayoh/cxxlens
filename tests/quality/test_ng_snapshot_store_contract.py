#!/usr/bin/env python3
"""Property, mutation, and failure-isolation tests for the NG store contract."""

from __future__ import annotations

import copy
import itertools
import pathlib
import sys
import unittest
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_snapshot_store_contract import (  # noqa: E402
    CONTRACT,
    CONTRACT_SCHEMA,
    CLOSURE_FIELDS,
    EXPECTED_READER_LATE_CLOSE_CLEANUP_AMENDMENT_DIGEST,
    EXPECTED_READER_NATIVE_ATTACHMENT_AMENDMENT_DIGEST,
    EXPECTED_SAME_PROCESS_WRITER_MAPPING_LEASE_PROPOSAL_DIGEST,
    EXPECTED_SCHEMA_DIGEST,
    EXPECTED_WRITER_GATE_OUTCOME_EVIDENCE_AMENDMENT_DIGEST,
    EXPECTED_WRITER_NATIVE_ATTACHMENT_AMENDMENT_DIGEST,
    SELECTOR_FIELDS,
    StoreContractError,
    canonical_binary,
    claim_identity,
    closure_binding,
    closure_mutation_matrix,
    compact,
    decode_sqlite_unsigned_integer,
    document_digest,
    format_open,
    identity_digest,
    load_yaml,
    producer_basis,
    publish,
    select_current,
    series_id,
    schema_validate,
    sqlite_unsigned_integer,
    snapshot_digest_matrix,
    unsigned_counter_canonical_integer,
    validate_all,
    validate_contract_shape,
    validate_df_0200_ingress_schema,
    validate_exact_schema,
    validate_identity_graph,
)


class NgSnapshotStoreContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = load_yaml(ROOT / CONTRACT)
        cls.schema = load_yaml(ROOT / CONTRACT_SCHEMA)

    def test_contract_and_exact_vector_set(self) -> None:
        contract, results, comparisons = validate_all(ROOT)
        self.assertEqual(contract["maturity"], "accepted")
        ingress = contract["df_0200_materialization_ingress"]
        self.assertEqual(
            ingress["status"], "accepted-authority-implementation-pending"
        )
        self.assertEqual(
            ingress["implementation_disposition"],
            "pending-implementation-and-qualification",
        )
        self.assertEqual(
            ingress["sqlite_capacity_decision"]["status"], "accepted"
        )
        self.assertEqual(len(results), 31)
        self.assertEqual(comparisons, 36)
        self.assertEqual(document_digest(self.schema), EXPECTED_SCHEMA_DIGEST)
        changed_schema = copy.deepcopy(self.schema)
        changed_schema["$id"] = "https://cxxlens.invalid/weakened-schema"
        with self.assertRaisesRegex(StoreContractError, "store.schema-drift"):
            validate_exact_schema(changed_schema)

    def test_sqlite_writer_mapping_lease_proposal_is_exact_and_fail_closed(
        self,
    ) -> None:
        lease = self.contract["format_compatibility"][
            "sqlite_source_shm_readonly_capability"
        ]["shm_map_state_machine"][
            "same_process_writer_mapping_lease_proposal"
        ]
        self.assertEqual(
            document_digest(lease),
            EXPECTED_SAME_PROCESS_WRITER_MAPPING_LEASE_PROPOSAL_DIGEST,
        )
        self.assertEqual(
            lease["status"], "accepted-authority-implementation-pending"
        )
        attachment = lease["writer_native_attachment_amendment_proposal"]
        self.assertEqual(
            document_digest(attachment),
            EXPECTED_WRITER_NATIVE_ATTACHMENT_AMENDMENT_DIGEST,
        )
        self.assertEqual(
            attachment["status"], "accepted-authority-implementation-pending"
        )
        self.assertEqual(
            attachment["tracking"], {"issue": "#206", "feedback": "DF-0206"}
        )
        self.assertEqual(
            attachment["acceptance_review_receipt"],
            "exact-commit-bf30978eb34d5f94bbadfd675c8ce2b50fb2f899-"
            "issue-206-comment-5097950062-independent-semantic-and-structural-"
            "P0-0-P1-0-P2-0-authorizes-internal-writer-attachment-group-state-"
            "machine-and-focused-tests-only-reader-grouping-blocked-by-DF-0207-"
            "production-remains-blocked",
        )
        self.assertEqual(
            attachment["authorization"]["before_independent_acceptance"],
            "authority-edit-readonly-audit-and-temporary-reproduction-only-no-"
            "attachment-group-implementation-or-production-binding",
        )
        self.assertEqual(
            attachment["generation_fresh_reader_page_set"],
            "union-of-page-support-from-exact-live-attachment-groups-recomputed-"
            "atomically-after-nonlast-cleanup",
        )
        self.assertEqual(
            attachment["gate_completion_total_order"],
            "same-attachment-gate-completion-and-later-map-admission-are-"
            "serialized-at-one-registry-state-boundary-before-the-later-native-"
            "callback",
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
            reader_attachment["status"],
            "accepted-authority-implementation-pending",
        )
        self.assertEqual(
            reader_attachment["tracking"],
            {"issue": "#207", "feedback": "DF-0207"},
        )
        self.assertEqual(
            reader_attachment["acceptance_review_receipt"],
            "exact-commit-636ef43803665e9999b38b9c33bd3afdbb6b4460-"
            "issue-207-comment-5125815049-fresh-independent-semantic-and-"
            "structural-P0-0-P1-0-P2-0-authorizes-internal-reader-attachment-"
            "group-map-session-lifetime-unmap-close-state-machine-and-focused-"
            "tests-only-production-VFS-public-native-OK-remain-blocked",
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
            "call-native-zero-times-until-the-active-use-owner-set-is-empty",
            reader_attachment["cleanup_dispatch"][
                "failure_exact_mapped_existing_group"
            ],
        )
        self.assertEqual(
            reader_attachment["authorization"]["before_independent_acceptance"],
            "authority-edit-readonly-audit-and-temporary-reproduction-only-no-"
            "reader-group-implementation-native-cleanup-production-binding-or-"
            "native-OK-projection",
        )
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
            late_close["status"], "proposed-unqualified-non-authorizing"
        )
        self.assertEqual(
            late_close["tracking"], {"issue": "#209", "feedback": "DF-0209"}
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
            late_close["authorization"]["transitive_authorization"],
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
            gate_outcome["status"], "accepted-authority-implementation-pending"
        )
        self.assertEqual(
            gate_outcome["tracking"], {"issue": "#208", "feedback": "DF-0208"}
        )
        self.assertEqual(
            gate_outcome["acceptance_review_receipt"],
            "exact-commit-bd2505f26d0d45b7bfa785a533c308ab957b11aa-"
            "issue-208-comment-5119882571-fresh-independent-semantic-and-structural-"
            "P0-0-P1-0-P2-0-authorizes-internal-writer-gate-outcome-evidence-state-"
            "machine-and-focused-tests-only-reader-grouping-blocked-by-DF-0207-"
            "production-VFS-public-native-OK-remain-blocked",
        )
        self.assertEqual(
            gate_outcome["gate_profile"]["ordered_stage_enum"],
            [
                "writer-readwrite-mode",
                "runtime-version-and-locator",
                "runtime-vfs-file-family-and-open-epoch",
                "synchronous-full-and-wal-mode",
                "current-v3-format-schema-head-counter-authority",
                "store-writer-open-before-publication-effect",
            ],
        )
        for stage_projection in gate_outcome["gate_profile"][
            "stage_value_projections"
        ].values():
            self.assertEqual(
                set(stage_projection),
                {
                    "authority_paths",
                    "exact_value_projection",
                    "exact_effect_projection",
                    "success",
                },
            )
        self.assertEqual(
            gate_outcome["native_attachment_binding"]["reservation_lifecycle"],
            [
                "reserved",
                "claimed_inflight",
                "consumed_to_present",
                "revoked",
                "quarantined",
            ],
        )
        self.assertEqual(
            gate_outcome["native_attachment_binding"]["observed_state"],
            ["absent", "present"],
        )
        self.assertEqual(
            gate_outcome["registry_cut"]["initial_member_classification"],
            [
                "exact_no_native_mapping",
                "exact_native_mapping",
                "native_outcome_unresolved",
            ],
        )
        self.assertEqual(
            gate_outcome["registry_cut"]["cut_execution_state"],
            [
                "cut_open",
                "resolving_cut_universe",
                "effect_ready",
                "completed",
                "cut_execution_indeterminate",
            ],
        )
        self.assertIn(
            "whole_effect_owner_transfer",
            gate_outcome["registry_cut"]["cleanup_lineage_boundary"][
                "terminal_commit_kind_enum"
            ],
        )
        self.assertIn(
            "preserve-existing-member-owners",
            gate_outcome["registry_cut"]["sequence_exhaustion"],
        )
        self.assertIn(
            "terminal_token_and_tag_rederivation-one_to_one_coverage_proof",
            gate_outcome["registry_cut"]["final_group_rederivation"],
        )
        self.assertEqual(
            gate_outcome["closed_outcome_union"]["typed_determinate_failure"][
                "open_epoch_drift"
            ],
            "forbidden-in-this-variant-and-always-terminal-indeterminate",
        )
        self.assertEqual(
            gate_outcome["composite_cleanup_lineage"][
                "closed_obligation_union"
            ],
            ["no_mapping_close_only", "mapped_unmap_then_close"],
        )
        self.assertIn(
            "mark-only-cut_execution_indeterminate",
            gate_outcome["empty_and_mixed_group"][
                "live_positive_and_same_attempt_failure"
            ],
        )
        self.assertIn(
            "every-existing-accepted-live-owner-remains-unchanged",
            gate_outcome["registry_cut"][
                "live_positive_lifecycle_contradiction_carveout"
            ],
        )
        native_attachment = gate_outcome["native_attachment_binding"]
        registry_cut = gate_outcome["registry_cut"]
        dispatch = gate_outcome["native_effect_dispatch_matrix"]
        self.assertEqual(
            native_attachment["callback_claim_owner"][
                "reachable_formation_kind_enum"
            ],
            ["atomically_dual_bound"],
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
        terminal_binding = registry_cut[
            "reservation_bearing_member_terminal_binding"
        ]
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
        self.assertIn(
            "zero-live-pin-terminal-proof-binding",
            pin_custody["fence_ordering"]["totality"],
        )
        self.assertEqual(
            pin_custody["retirement_receipt_kind_enum"],
            [
                "DF0206_same_invocation_confirmed",
                "ownerless_open_epoch_lifetime_retirement",
            ],
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
        self.assertEqual(
            gate_outcome["authorization"]["production_activation"],
            "blocked-until-proposal-acceptance-distinct-exact-implementation-"
            "complete-counterexample-matrix-and-separate-production-VFS-public-"
            "projection-review",
        )
        self.assertEqual(
            lease["authorization"]["production_activation"],
            "blocked-until-the-exact-implementation-and-complete-counterexample-"
            "matrix-receive-a-distinct-independent-review",
        )
        self.assertEqual(
            self.contract["format_compatibility"][
                "sqlite_source_shm_readonly_capability"
            ]["shm_map_state_machine"]["any_native_ok"],
            "backend-protocol-violation-fail-closed-never-translate-to-readonly",
        )
        schema_validate(self.contract, self.schema, "store contract")

        def reader_native_attachment(value: dict[str, Any]) -> dict[str, Any]:
            return value["reader_native_attachment_amendment_proposal"]

        def reader_late_close_cleanup(value: dict[str, Any]) -> dict[str, Any]:
            return value["reader_late_close_cleanup_amendment_proposal"]

        def writer_gate_outcome(value: dict[str, Any]) -> dict[str, Any]:
            return value["writer_gate_outcome_evidence_amendment_proposal"]

        mutations = [
            (
                "status",
                lambda value: value.__setitem__("status", "accepted"),
            ),
            (
                "current-rejection",
                lambda value: value.__setitem__(
                    "current_rule_before_acceptance",
                    "allow-native-OK-before-review",
                ),
            ),
            (
                "projection",
                lambda value: value["post_acceptance_native_projection"].__setitem__(
                    "outward_result", "exact-SQLITE_OK-plus-pointer"
                ),
            ),
            (
                "pending-order",
                lambda value: value["two_stage_writer_authority"].__setitem__(
                    "predelegate_attempt",
                    "install-pending-before-native-delegation",
                ),
            ),
            (
                "attachment-amendment",
                lambda value: value.pop(
                    "writer_native_attachment_amendment_proposal"
                ),
            ),
            (
                "attachment-status-regressed",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ].__setitem__(
                    "status", "proposed-unqualified-non-authorizing"
                ),
            ),
            (
                "attachment-review-receipt-removed",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ].pop("acceptance_review_receipt"),
            ),
            (
                "attachment-pre-review-implementation-authorized",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ]["authorization"].__setitem__(
                    "before_independent_acceptance",
                    "attachment-group-implementation-authorized",
                ),
            ),
            (
                "attachment-production-self-authorized",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ]["authorization"].__setitem__(
                    "production_activation", "allowed"
                ),
            ),
            (
                "attachment-pointer-only-identity",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ].__setitem__(
                    "attachment_identity",
                    "native-pointer-and-connection-only",
                ),
            ),
            (
                "attachment-group",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ].__setitem__(
                    "cleanup_completion",
                    "reuse-one-native-outcome-for-independent-holders",
                ),
            ),
            (
                "attachment-page-support-removed",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ].pop("generation_fresh_reader_page_set"),
            ),
            (
                "attachment-sole-page-inflight-blocker-removed",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ].pop("nonlast_sole_page_reader_predelegate_blocker"),
            ),
            (
                "attachment-remaining-page-inflight-blocked",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ].__setitem__(
                    "nonlast_remaining_page_reader_predelegate",
                    "every-reader-predelegation-blocks-nonlast-cleanup",
                ),
            ),
            (
                "attachment-sole-page-same-thread-waits",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ].__setitem__(
                    "nonlast_sole_page_same_thread",
                    "wait-on-the-cleanup-callback-thread-then-delegate-unmap",
                ),
            ),
            (
                "attachment-sole-page-other-thread-retries",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ].__setitem__(
                    "nonlast_sole_page_other_thread",
                    "retry-after-timeout-unknown-or-unconfirmed-reader-cleanup",
                ),
            ),
            (
                "attachment-handoff-becomes-page-support",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ].__setitem__(
                    "established_reader_handoff_during_writer_cleanup",
                    "block-writer-cleanup-and-mint-fresh-reader-page-support",
                ),
            ),
            (
                "attachment-retired-evidence-transferable",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ].__setitem__(
                    "retired_attachment_evidence",
                    "transfer-to-any-live-attachment-in-the-generation",
                ),
            ),
            (
                "attachment-gate-total-order-removed",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ].pop("gate_completion_total_order"),
            ),
            (
                "attachment-gate-partial-pending-allowed",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ].__setitem__(
                    "successful_gate_postcondition",
                    "same-attachment-pending-members-may-survive-gate-success",
                ),
            ),
            (
                "attachment-sole-page-inflight-negative-removed",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ]["fail_closed_matrix"]["required"].remove(
                    "nonlast-sole-page-support-writer-unmap-before-reader-"
                    "predelegation-resolution"
                ),
            ),
            (
                "attachment-handoff-negative-removed",
                lambda value: value[
                    "writer_native_attachment_amendment_proposal"
                ]["fail_closed_matrix"]["required"].remove(
                    "established-reader-handoff-treated-as-writer-cleanup-"
                    "blocker-or-fresh-page-support"
                ),
            ),
            (
                "reader-attachment-proposal-removed",
                lambda value: value.pop(
                    "reader_native_attachment_amendment_proposal"
                ),
            ),
            (
                "reader-attachment-status-regressed",
                lambda value: reader_native_attachment(value).__setitem__(
                    "status", "proposed-unqualified-non-authorizing"
                ),
            ),
            (
                "reader-attachment-review-receipt-removed",
                lambda value: reader_native_attachment(value).pop(
                    "acceptance_review_receipt"
                ),
            ),
            (
                "reader-attachment-observed-SHM-identity-removed",
                lambda value: reader_native_attachment(value)[
                    "attachment_identity"
                ]["binding"].remove(
                    "checked-observed-SHM-native-attachment-object-direct-entry-"
                    "device-and-mount-receipt"
                ),
            ),
            (
                "reader-attachment-cleanup-observation-owner-removed",
                lambda value: reader_native_attachment(value)[
                    "attachment_identity"
                ].pop("cleanup_only_observation_owner"),
            ),
            (
                "reader-attachment-predecessor-retirement-removed",
                lambda value: reader_native_attachment(value)["group_state"][
                    "reservation_transition_graph"
                ].pop("predecessor_route_active"),
            ),
            (
                "reader-attachment-unpublished-cleanup-state-removed",
                lambda value: reader_native_attachment(value)["group_state"][
                    "reservation_transition_graph"
                ].pop("unpublished_cleanup_admitted"),
            ),
            (
                "reader-attachment-later-failure-proactively-unmaps",
                lambda value: reader_native_attachment(value)[
                    "cleanup_dispatch"
                ].__setitem__(
                    "failure_exact_mapped_existing_group",
                    "proactively-unmap-before-active-use-owners-end",
                ),
            ),
            (
                "reader-attachment-use-owner-set-removed",
                lambda value: reader_native_attachment(value)[
                    "eager_transaction_lifetime"
                ].pop("owner_set"),
            ),
            (
                "reader-attachment-pointer-coverage-removed",
                lambda value: reader_native_attachment(value)[
                    "eager_transaction_lifetime"
                ].pop("pointer_coverage_relation"),
            ),
            (
                "reader-attachment-pointer-publication-atomicity-removed",
                lambda value: reader_native_attachment(value)[
                    "eager_transaction_lifetime"
                ].pop("pointer_publication_commit"),
            ),
            (
                "reader-attachment-session-start-issuer-removed",
                lambda value: reader_native_attachment(value)[
                    "eager_transaction_lifetime"
                ].pop("session_start_issuer"),
            ),
            (
                "reader-attachment-pre-mint-route-partition-removed",
                lambda value: reader_native_attachment(value)[
                    "eager_transaction_lifetime"
                ].pop("pre_mint_route_partition"),
            ),
            (
                "reader-attachment-cached-pointer-admission-removed",
                lambda value: reader_native_attachment(value)[
                    "eager_transaction_lifetime"
                ].pop("cached_pointer_session_admission"),
            ),
            (
                "reader-attachment-protocol-invalid-route-removed",
                lambda value: reader_native_attachment(value)[
                    "map_attempt"
                ].pop("exact_protocol_invalid_no_attachment"),
            ),
            (
                "reader-attachment-closed-custody-enum-removed",
                lambda value: reader_native_attachment(value)["ownership"].pop(
                    "custody_kind_enum"
                ),
            ),
            (
                "reader-attachment-opaque-successor-blocker-removed",
                lambda value: reader_native_attachment(value)[
                    "writer_generation_boundary"
                ].__setitem__(
                    "successor",
                    "only-live-group-count-blocks-successor-admission",
                ),
            ),
            (
                "reader-attachment-terminal-commit-success-leaks",
                lambda value: reader_native_attachment(value)[
                    "outward_projection"
                ].__setitem__(
                    "unmap_result",
                    "native-SQLITE-OK-is-always-outward-SQLITE-OK",
                ),
            ),
            (
                "reader-attachment-logical-ack-replayable",
                lambda value: reader_native_attachment(value)[
                    "cleanup_dispatch"
                ]["logical_ack_phase_enum"].append("replayable"),
            ),
            (
                "reader-attachment-close-cut-removed",
                lambda value: reader_native_attachment(value)["ordering"].pop(
                    "close_cut"
                ),
            ),
            (
                "reader-attachment-OK-null-passes-through",
                lambda value: reader_native_attachment(value)[
                    "outward_projection"
                ].__setitem__(
                    "exact_determinate_no_change",
                    "preserve-SQLITE-OK-null",
                ),
            ),
            (
                "reader-attachment-production-self-authorized",
                lambda value: reader_native_attachment(value)[
                    "authorization"
                ].__setitem__("production_activation", "allowed"),
            ),
            (
                "reader-late-close-cleanup-proposal-removed",
                lambda value: value.pop(
                    "reader_late_close_cleanup_amendment_proposal"
                ),
            ),
            (
                "reader-late-close-cleanup-status-self-authorized",
                lambda value: reader_late_close_cleanup(value).__setitem__(
                    "status", "accepted-authority-implementation-pending"
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
                "gate-outcome-proposal-removed",
                lambda value: value.pop(
                    "writer_gate_outcome_evidence_amendment_proposal"
                ),
            ),
            (
                "gate-outcome-status-regressed",
                lambda value: writer_gate_outcome(value).__setitem__(
                    "status", "proposed-unqualified-non-authorizing"
                ),
            ),
            (
                "gate-outcome-review-receipt-removed",
                lambda value: writer_gate_outcome(value).pop(
                    "acceptance_review_receipt"
                ),
            ),
            (
                "gate-outcome-stage-profile-weakened",
                lambda value: writer_gate_outcome(value)["gate_profile"][
                    "ordered_stage_enum"
                ].remove("current-v3-format-schema-head-counter-authority"),
            ),
            (
                "gate-outcome-stage-value-projection-removed",
                lambda value: writer_gate_outcome(value)["gate_profile"][
                    "stage_value_projections"
                ]["runtime-version-and-locator"].pop("exact_value_projection"),
            ),
            (
                "gate-outcome-terminal-locus-open-ended",
                lambda value: writer_gate_outcome(value)["gate_profile"][
                    "terminal_evidence_locus"
                ]["closed_kind_enum"].append("arbitrary-caller-locus"),
            ),
            (
                "gate-outcome-policy-profile-digest-weakened",
                lambda value: writer_gate_outcome(value)["gate_profile"].__setitem__(
                    "canonical_policy_profile_digest",
                    "opaque-untyped-digest",
                ),
            ),
            (
                "gate-outcome-exclusivity-weakened",
                lambda value: writer_gate_outcome(value)[
                    "closed_outcome_union"
                ].__setitem__(
                    "exclusivity",
                    "success-and-failure-may-both-be-issued",
                ),
            ),
            (
                "gate-outcome-reservation-state-open-ended",
                lambda value: writer_gate_outcome(value)[
                    "native_attachment_binding"
                ]["reservation_lifecycle"].append("reusable"),
            ),
            (
                "gate-outcome-map-before-gate-reconsumes",
                lambda value: writer_gate_outcome(value)[
                    "native_attachment_binding"
                ].__setitem__(
                    "map_before_gate_present",
                    "consume-a-second-reservation-at-the-gate",
                ),
            ),
            (
                "gate-outcome-precut-abandonment-mutates",
                lambda value: writer_gate_outcome(value)["gate_attempt_owner"].__setitem__(
                    "pre_acceptance_drop",
                    "hide-the-caller-selected-group-and-close",
                ),
            ),
            (
                "gate-outcome-postcut-abandonment-determinate",
                lambda value: writer_gate_outcome(value)["gate_attempt_owner"].__setitem__(
                    "postcut_continuation_abandonment",
                    "reclassify-as-typed-determinate-failure",
                ),
            ),
            (
                "gate-outcome-precut-mismatch-cuts",
                lambda value: writer_gate_outcome(value)["gate_profile"].__setitem__(
                    "supplied_mismatch",
                    "accept-a-cross-bound-owner-and-issue-a-cut",
                ),
            ),
            (
                "gate-outcome-caller-snapshot-authoritative",
                lambda value: writer_gate_outcome(value)["registry_cut"].__setitem__(
                    "final_group_rederivation",
                    "use-the-caller-member-snapshot-as-the-complete-group",
                ),
            ),
            (
                "gate-outcome-independent-cut-sequence",
                lambda value: writer_gate_outcome(value)["registry_cut"].__setitem__(
                    "sequence_domain",
                    "allocate-cut-from-an-independent-reusable-counter",
                ),
            ),
            (
                "gate-outcome-coverage-allows-omission",
                lambda value: writer_gate_outcome(value)["registry_cut"].__setitem__(
                    "one_to_one_coverage_proof",
                    "allow-missing-mapped-blocker-members",
                ),
            ),
            (
                "gate-outcome-cut-terminal-changes-after-close",
                lambda value: writer_gate_outcome(value)["registry_cut"].__setitem__(
                    "cleanup_lineage_boundary",
                    "close-unknown-changes-completed-to-cut_execution_indeterminate",
                ),
            ),
            (
                "gate-outcome-positive-cleanup",
                lambda value: writer_gate_outcome(value)[
                    "native_state_resolution"
                ].__setitem__(
                    "positive_mapped_owner_retention",
                    "perform-one-native-drain-before-promotion",
                ),
            ),
            (
                "gate-outcome-indeterminate-mints-failure",
                lambda value: writer_gate_outcome(value)[
                    "native_state_resolution"
                ].__setitem__(
                    "drain_semantics",
                    "drain-success-mints-typed-determinate-failure",
                ),
            ),
            (
                "gate-outcome-known-mapped-drain-optional",
                lambda value: writer_gate_outcome(value)[
                    "native_effect_dispatch_matrix"
                ]["cut_execution_indeterminate_operational_matrix"].__setitem__(
                    "mapped_exact_live_attachment_owner",
                    "perform-zero-or-one-drain",
                ),
            ),
            (
                "gate-outcome-mixed-member-duplicate-unmap",
                lambda value: writer_gate_outcome(value)[
                    "native_state_resolution"
                ].__setitem__(
                    "mixed_no_map_and_mapped",
                    "perform-one-unmap-per-mapped-member",
                ),
            ),
            (
                "gate-outcome-empty-owner-ambiguous",
                lambda value: writer_gate_outcome(value)[
                    "empty_and_mixed_group"
                ].__setitem__(
                    "terminal_indeterminate_empty_without_close_owner",
                    "guess-and-call-one-close",
                ),
            ),
            (
                "gate-outcome-mixed-live-mutates",
                lambda value: writer_gate_outcome(value)[
                    "empty_and_mixed_group"
                ].__setitem__(
                    "live_positive_and_same_attempt_failure",
                    "hide-existing-live-members-as-clean-gate-failure",
                ),
            ),
            (
                "gate-outcome-live-carveout-removed",
                lambda value: writer_gate_outcome(value)["registry_cut"].__setitem__(
                    "live_positive_lifecycle_contradiction_carveout",
                    "quarantine-the-existing-accepted-DF-0206-live-group",
                ),
            ),
            (
                "gate-outcome-composite-owner-not-closed",
                lambda value: writer_gate_outcome(value)[
                    "composite_cleanup_lineage"
                ]["closed_obligation_union"].append("unmap_only"),
            ),
            (
                "gate-outcome-cleanup-reissue",
                lambda value: writer_gate_outcome(value)[
                    "composite_cleanup_lineage"
                ].__setitem__(
                    "preinvoke_consumption",
                    "reissue-after-native-or-internal-commit-failure",
                ),
            ),
            (
                "gate-outcome-df0207-transitivity",
                lambda value: writer_gate_outcome(value)["reader_boundary"].__setitem__(
                    "grouping",
                    "writer-group-authorizes-reader-attachment-grouping",
                ),
            ),
            (
                "gate-outcome-indeterminate-drain-negative-removed",
                lambda value: writer_gate_outcome(value)["fail_closed_matrix"][
                    "required"
                ].remove(
                    "cut-indeterminate-mapped-exact-attachment-skips-or-duplicates-"
                    "drain-or-closes"
                ),
            ),
            (
                "gate-outcome-dual-formation-not-atomic",
                lambda value: writer_gate_outcome(value)[
                    "native_attachment_binding"
                ]["callback_claim_owner"].__setitem__(
                    "reachable_formation_kind_enum",
                    ["reservation_claim_only"],
                ),
            ),
            (
                "gate-outcome-invalid-partial-enters-ordinary-route",
                lambda value: writer_gate_outcome(value)[
                    "native_attachment_binding"
                ]["callback_result_totality"].__setitem__(
                    "invalid_partial_dual_formation_guard",
                    "route-partial-claim-through-the-ordinary-product",
                ),
            ),
            (
                "gate-outcome-normal-substep-partition-weakened",
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
                "gate-outcome-frozen-step-cross-origin",
                lambda value: writer_gate_outcome(value)["registry_cut"][
                    "attachment_cut_exclusivity"
                ]["admission_kind_partition"][
                    "frozen_continuation_step_enum"
                ].remove(
                    "dual_terminal_resolution"
                ),
            ),
            (
                "gate-outcome-fresh-dual-custody-collapsed",
                lambda value: writer_gate_outcome(value)["registry_cut"][
                    "mapping_identity_integrity_census"
                ].__setitem__(
                    "fresh_unsafe_custody_form_enum",
                    ["nonpromotion_composite_pin_custody"],
                ),
            ),
            (
                "gate-outcome-fresh-retirement-metadata-rebound",
                lambda value: writer_gate_outcome(value)[
                    "native_effect_dispatch_matrix"
                ]["mapping_identity_integrity_fence"]["quarantine_binding"].__setitem__(
                    "fresh_projection",
                    "retire-first-then-reseal-tags-and-reconstruct-pins",
                ),
            ),
            (
                "gate-outcome-pin-custody-transition-revives",
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
                "gate-outcome-durable-release-before-current-record",
                lambda value: writer_gate_outcome(value)["registry_cut"][
                    "attachment_cut_exclusivity"
                ]["durable_prior_cut_outcome_gate"].__setitem__(
                    "release_and_wakeup_atomicity",
                    "release-slot-and-wake-before-installing-the-current-record",
                ),
            ),
            (
                "gate-outcome-production-self-authorized",
                lambda value: writer_gate_outcome(value)["authorization"].__setitem__(
                    "production_activation", "allowed"
                ),
            ),
            (
                "attachment-untrusted-platform-mints",
                lambda value: value["two_stage_writer_authority"].__setitem__(
                    "writer_mapping_epoch_failure",
                    "non-Linux-or-unavailable-stat-watch-may-mint-from-final-state",
                ),
            ),
            (
                "successor",
                lambda value: value["generation_and_races"].__setitem__(
                    "successor_while_handoff_live",
                    "allow-successor-when-pointer-matches",
                ),
            ),
            (
                "production-activation",
                lambda value: value["authorization"].__setitem__(
                    "production_activation",
                    "allowed",
                ),
            ),
        ]
        for name, mutate in mutations:
            with self.subTest(drift=name):
                changed = copy.deepcopy(self.contract)
                changed_lease = changed["format_compatibility"][
                    "sqlite_source_shm_readonly_capability"
                ]["shm_map_state_machine"][
                    "same_process_writer_mapping_lease_proposal"
                ]
                mutate(changed_lease)
                with self.assertRaisesRegex(
                    StoreContractError,
                    "store.sqlite-shm-writer-lease-proposal-invalid",
                ):
                    validate_contract_shape(changed)
                with self.assertRaisesRegex(
                    StoreContractError,
                    "store.schema-invalid",
                ):
                    schema_validate(changed, self.schema, "store contract")

    def test_binary_encoding_separates_types_and_boundaries(self) -> None:
        values = [None, False, 0, b"0", "0", ["a", "bc"], ["ab", "c"]]
        encoded = [canonical_binary(value) for value in values]
        self.assertEqual(len(encoded), len(set(encoded)))

    def test_unsigned_counter_codecs_are_exact_across_sign_boundary(self) -> None:
        values = [0, (1 << 63) - 1, 1 << 63, (1 << 64) - 1]
        expected_signed = [0, (1 << 63) - 1, -(1 << 63), -1]
        self.assertEqual(
            [unsigned_counter_canonical_integer(value) for value in values],
            expected_signed,
        )
        self.assertEqual(
            [
                decode_sqlite_unsigned_integer(sqlite_unsigned_integer(value))
                for value in values
            ],
            values,
        )
        for invalid in (-1, 1 << 64, True):
            with self.assertRaisesRegex(StoreContractError, "counter-domain-invalid"):
                unsigned_counter_canonical_integer(invalid)

    def test_transactional_publication_and_compaction_contract_is_fail_closed(self) -> None:
        mutations = [
            (
                "store.counter-authority-invalid",
                lambda value: value["publication_counters"]["authority_record"][
                    "excluded"
                ].remove("rejected"),
            ),
            (
                "store.counter-allocation-invalid",
                lambda value: value["publication_counters"]["sqlite_allocation"].update(
                    authority_scan="process-local-records"
                ),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"]["sqlite_head_cas"].update(
                    conflict="store.publication-conflict-any-failure"
                ),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"]["sqlite_head_cas"].update(
                    memory_update="after-commit-local-candidate-only"
                ),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"]["sqlite_head_cas"][
                    "steps"
                ].remove("full-committed-authority-census"),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ].update(reclassifier_id="unbound"),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ]["sealed_receipt_profiles"]["post_format_prewrite"].remove(
                    "no-candidate-yet"
                ),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ].update(post_format_candidate_extension="candidate-may-be-partial"),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ]["sealed_receipt_profiles"]["fresh_initialization"].remove(
                    "actual-target-main-open-file-instance-identity-and-directory-entry-binding"
                ),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ].update(fresh_initialization_receipt_seal="after-journal-arming"),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"]
                ["sqlite_terminal_recovery"]["sealed_receipt_profiles"]
                ["accepted_empty_normalization_source_anchor"].remove(
                    "pinned-sqlite-runtime-identity-and-version"
                ),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"]
                ["sqlite_terminal_recovery"]["sealed_receipt_profiles"]
                ["accepted_empty_normalization_source_anchor"].remove(
                    "immutable-held-pre-main-exact-byte-snapshot-with-length-and-"
                    "streaming-byte-receipt"
                ),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"]
                ["sqlite_terminal_recovery"]["sealed_receipt_profiles"]
                ["accepted_empty_normalization"].pop(),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"]
                ["sqlite_terminal_recovery"]["sealed_receipt_profiles"]
                ["accepted_empty_normalization_completed_edge"].pop(1),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"]
                ["sqlite_terminal_recovery"].update(
                    accepted_empty_normalization_source_anchor=(
                        "after-coordination-effect"
                    )
                ),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"]
                ["sqlite_terminal_recovery"].update(
                    accepted_empty_normalization_receipt_seal=(
                        "in-the-first-exclusive-xLock-callback"
                    )
                ),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"]
                ["sqlite_terminal_recovery"].update(
                    accepted_empty_normalization_source_anchor_seal=(
                        "before-installing-pending-request"
                    )
                ),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"]
                ["sqlite_terminal_recovery"].update(
                    accepted_empty_normalization_candidate_identity=(
                        "planned-candidate-proves-success"
                    )
                ),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"]
                ["sqlite_terminal_recovery"].update(
                    accepted_empty_normalization_completed_edge_seal=(
                        "before-close"
                    )
                ),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"]
                ["sqlite_terminal_recovery"].update(
                    accepted_empty_normalization_operation_identity=(
                        "reuse-fresh-initialization-operation"
                    )
                ),
            ),
            (
                "store.publication-cas-invalid",
                lambda value: value["publication_transaction"]
                ["sqlite_terminal_recovery"].update(
                    accepted_empty_normalization_success="install-store"
                ),
            ),
            (
                "store.counter-allocation-invalid",
                lambda value: value["publication_counters"]["sqlite_allocation"][
                    "compaction_range"
                ].update(distinct_generation_per_publication=False),
            ),
            (
                "store.compaction-contract-invalid",
                lambda value: value["compaction"].update(
                    sqlite_process_memory_update="before-database-commit"
                ),
            ),
            (
                "store.compaction-contract-invalid",
                lambda value: value["compaction"]["generation_allocation"].update(
                    committed_nonempty="allocate-from-zero"
                ),
            ),
            (
                "store.compaction-contract-invalid",
                lambda value: value["compaction"]["generation_allocation"].update(
                    committed_empty_operation="commit-empty-transaction"
                ),
            ),
            (
                "store.compaction-contract-invalid",
                lambda value: value["compaction"]["generation_allocation"][
                    "committed_empty_operation"
                ].update(
                    failure="return-original-error-without-close-or-reclassification"
                ),
            ),
            (
                "store.compaction-contract-invalid",
                lambda value: value["compaction"]["sqlite_v2_to_v3_migration"][
                    "commit_outcome_unknown"
                ]["post_classification_state"].update(
                    valid_non_descendant_or_invalid_or_mixed=(
                        "install-reopened-state"
                    )
                ),
            ),
            (
                "store.compaction-contract-invalid",
                lambda value: value["compaction"]
                ["sqlite_v3_compaction_commit_outcome_unknown"]
                ["post_classification_state"].update(
                    exact_pre_or_valid_uncompacted="return-opaque-with-stale-state"
                ),
            ),
            (
                "store.compaction-contract-invalid",
                lambda value: value["compaction"]["sqlite_v2_to_v3_migration"].update(
                    diagnostic_projection="semantic-only"
                ),
            ),
            (
                "store.compaction-contract-invalid",
                lambda value: value["compaction"]
                ["sqlite_v3_compaction_commit_outcome_unknown"].update(
                    zero_anchor="commit-outcome-unknown"
                ),
            ),
            (
                "store.counter-storage-invalid",
                lambda value: value["publication_identity"].update(
                    sequence_canonical_codec="implementation-defined-cast"
                ),
            ),
        ]
        for code, mutate in mutations:
            with self.subTest(code=code):
                changed = copy.deepcopy(self.contract)
                mutate(changed)
                with self.assertRaisesRegex(StoreContractError, code):
                    validate_contract_shape(changed)

    def test_df_0202_sqlite_terminal_recovery_projection_is_exact_and_closed(
        self,
    ) -> None:
        sqlite_contract = load_yaml(
            ROOT / "schemas" / "cxxlens_ng_sqlite_store_contract.yaml"
        )
        sqlite_terminal = sqlite_contract["transaction"]["recovery_model"][
            "terminal_reclassification"
        ]
        snapshot_terminal = self.contract["publication_transaction"][
            "sqlite_terminal_recovery"
        ]
        for receipt_name in (
            "accepted_empty_normalization_source_anchor",
            "accepted_empty_normalization",
            "accepted_empty_normalization_completed_edge",
        ):
            self.assertEqual(
                snapshot_terminal["sealed_receipt_profiles"][receipt_name],
                sqlite_terminal["sealed_receipt_profiles"][receipt_name],
            )
        for field in (
            "accepted_empty_normalization_source_anchor",
            "accepted_empty_normalization_source_anchor_profile",
            "accepted_empty_normalization_source_anchor_seal",
            "accepted_empty_normalization_receipt_seal",
            "accepted_empty_normalization_receipt_extension",
            "accepted_empty_normalization_candidate_identity",
            "accepted_empty_normalization_completed_edge_profile",
            "accepted_empty_normalization_completed_edge_seal",
            "accepted_empty_normalization_operation_identity",
            "accepted_empty_normalization_success",
            "accepted_empty_normalization_public_success",
            "accepted_empty_normalization_receiptless_crash_profile_draft",
        ):
            self.assertEqual(snapshot_terminal[field], sqlite_terminal[field])

        completed = "accepted_empty_normalization_completed_edge"
        mutations = [
            (
                "effect-grammar-profile",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ]["sealed_receipt_profiles"][
                    "accepted_empty_normalization_source_anchor"
                ].remove("exact-normalization-effect-grammar-profile-receipt"),
            ),
            (
                "bounded-effect-transcript",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ]["sealed_receipt_profiles"][completed].remove(
                    "exact-normalization-bounded-effect-transcript-receipt"
                ),
            ),
            (
                "coordination-wal-delete-parent-sync",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ]["sealed_receipt_profiles"][completed].remove(
                    "exact-coordination-wal-delete-retained-authenticated-parent-"
                    "fsync-receipt"
                ),
            ),
            (
                "journal-creation-parent-sync",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ]["sealed_receipt_profiles"][completed].remove(
                    "exact-journal-creation-retained-authenticated-parent-fsync-"
                    "receipt"
                ),
            ),
            (
                "terminal-journal-delete-parent-sync",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ]["sealed_receipt_profiles"][completed].remove(
                    "exact-terminal-journal-delete-retained-authenticated-parent-"
                    "fsync-receipt"
                ),
            ),
            (
                "final-sync-seal",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ].__setitem__(
                    "accepted_empty_normalization_completed_edge_seal",
                    "seal-before-terminal-journal-delete-parent-fsync",
                ),
            ),
            (
                "six-family-route-partition",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ]["accepted_empty_normalization_receiptless_crash_profile_draft"][
                    "family_partition"
                ].pop(),
            ),
            (
                "cold-operation-history",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ]["accepted_empty_normalization_receiptless_crash_profile_draft"]
                .__setitem__(
                    "cold_operation_history_inference",
                    "infer-the-prior-normalization-edge",
                ),
            ),
            (
                "disposable-fixture-capability",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ]["accepted_empty_normalization_receiptless_crash_profile_draft"].pop(
                    "disposable_fixture_capability"
                ),
            ),
            (
                "profile-receipt-layering",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ]["accepted_empty_normalization_receiptless_crash_profile_draft"].pop(
                    "profile_receipt_layering"
                ),
            ),
            (
                "proposal-review-receipt-separation",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ]["accepted_empty_normalization_receiptless_crash_profile_draft"].pop(
                    "review_receipt_separation"
                ),
            ),
            (
                "public-route",
                lambda value: value["publication_transaction"][
                    "sqlite_terminal_recovery"
                ]["accepted_empty_normalization_receiptless_crash_profile_draft"]
                .__setitem__("public_success", "allowed"),
            ),
        ]
        for name, mutate in mutations:
            with self.subTest(drift=name):
                changed = copy.deepcopy(self.contract)
                mutate(changed)
                with self.assertRaisesRegex(
                    StoreContractError, "store.publication-cas-invalid"
                ):
                    validate_contract_shape(changed)

    def test_df_0200_accepted_materialization_ingress_is_closed(self) -> None:
        validate_contract_shape(copy.deepcopy(self.contract))

        changed = copy.deepcopy(self.contract)
        changed.pop("df_0200_materialization_ingress")
        with self.assertRaisesRegex(
            StoreContractError, "materialization-ingress-contract-invalid"
        ):
            validate_contract_shape(changed)

        ingress_mutations: list[tuple[str, dict[str, object]]] = []

        changed = copy.deepcopy(self.contract)
        changed["df_0200_materialization_ingress"]["source"]["codec"][
            "event_kind_codes"
        ]["partition-end"] = 8
        ingress_mutations.append(("codec-kind-code", changed))

        changed = copy.deepcopy(self.contract)
        changed["df_0200_materialization_ingress"]["source"]["codec"][
            "authority_binding"
        ]["canonical_json_sha256"] = "sha256:" + "0" * 64
        ingress_mutations.append(("codec-full-authority-digest", changed))

        changed = copy.deepcopy(self.contract)
        changed["df_0200_materialization_ingress"]["source"][
            "external_completeness_authority"
        ]["whole_partition_drop"] = "trust-self-reported-trailer"
        ingress_mutations.append(("whole-partition-drop", changed))

        changed = copy.deepcopy(self.contract)
        changed["df_0200_materialization_ingress"]["source"][
            "external_completeness_authority"
        ]["pre_encoder_receipt_oracle"]["receipt_seal"]["projection"].remove(
            "successful-seal"
        )
        ingress_mutations.append(("receipt-successful-seal", changed))

        changed = copy.deepcopy(self.contract)
        receipt_projection = changed["df_0200_materialization_ingress"][
            "source"
        ]["external_completeness_authority"]["pre_encoder_receipt_oracle"][
            "receipt_seal"
        ]["projection"]
        receipt_projection[
            receipt_projection.index("selected-request-entry-binding-digest")
        ] = "execution-journal-receipt-set-digest"
        ingress_mutations.append(("receipt-journal-cycle", changed))

        changed = copy.deepcopy(self.contract)
        changed["df_0200_materialization_ingress"]["counter_model"][
            "canonical_v5_collection_counts"
        ]["maximum"] = 4_294_967_295
        ingress_mutations.append(("collection-count-u32-narrowing", changed))

        changed = copy.deepcopy(self.contract)
        changed["df_0200_materialization_ingress"]["counter_model"][
            "collection_overflow_failure"
        ]["operation"] = "writer_publish"
        ingress_mutations.append(("overflow-operation", changed))

        changed = copy.deepcopy(self.contract)
        changed["df_0200_materialization_ingress"][
            "sqlite_capacity_decision"
        ]["selected_alternative"] = "B"
        ingress_mutations.append(("sqlite-option-a-binding", changed))

        changed = copy.deepcopy(self.contract)
        changed["df_0200_materialization_ingress"][
            "sqlite_capacity_decision"
        ].pop("decision_ref")
        ingress_mutations.append(("sqlite-option-a-authority", changed))

        for name, changed in ingress_mutations:
            with self.subTest(ingress=name):
                with self.assertRaisesRegex(
                    StoreContractError, "materialization-ingress-contract-invalid"
                ):
                    validate_contract_shape(changed)

        changed = copy.deepcopy(self.contract)
        changed["df_0200_materialization_ingress"]["source"][
            "store-validation"
        ]["required-recomputation"].remove("canonical-claim-and-row-identity")
        with self.assertRaisesRegex(
            StoreContractError, "materialization-ingress-contract-invalid"
        ):
            validate_contract_shape(changed)

        changed = copy.deepcopy(self.contract)
        changed["df_0200_materialization_ingress"][
            "unexpected-extension"
        ] = "allowed"
        with self.assertRaisesRegex(
            StoreContractError, "materialization-ingress-contract-invalid"
        ):
            validate_contract_shape(changed)

        schema = load_yaml(ROOT / CONTRACT_SCHEMA)
        validate_df_0200_ingress_schema(copy.deepcopy(schema))
        changed_schema = copy.deepcopy(schema)
        changed_schema["$defs"]["df_0200_materialization_ingress"][
            "const"
        ]["compatibility"].pop("snapshot_payload_v5_schema_and_semantic_projection")
        with self.assertRaisesRegex(
            StoreContractError, "materialization-ingress-contract-invalid"
        ):
            validate_df_0200_ingress_schema(changed_schema)

        changed_schema = copy.deepcopy(schema)
        changed_schema["required"].remove(
            "df_0200_materialization_ingress"
        )
        with self.assertRaisesRegex(
            StoreContractError, "materialization-ingress-contract-invalid"
        ):
            validate_df_0200_ingress_schema(changed_schema)

    def test_generic_store_checker_has_no_materialization_reverse_dependency(
        self,
    ) -> None:
        checker = (
            ROOT / "tools/quality/check_ng_snapshot_store_contract.py"
        ).read_text(encoding="utf-8")
        self.assertNotIn("MATERIALIZATION_CONTRACT", checker)
        self.assertNotIn("validate_df_0200_cross_contract_binding", checker)
        self.assertNotIn(
            "cxxlens_ng_clang22_materialization_contract.yaml", checker
        )

    def test_domain_separation_changes_digest(self) -> None:
        fields = ["same", 1]
        self.assertNotEqual(
            identity_digest("semantic-key", fields),
            identity_digest("assertion", fields),
        )

    def test_identity_graph_is_dag_and_cycle_fails(self) -> None:
        order = validate_identity_graph(self.contract)
        self.assertLess(order.index("partition_content_digest"), order.index("snapshot_id"))
        with self.assertRaisesRegex(StoreContractError, "identity-cycle"):
            validate_identity_graph(
                self.contract,
                {"node": "partition_content_digest", "dependency": "snapshot_id"},
            )

    def test_claim_identity_has_no_containing_snapshot_dependency(self) -> None:
        value = {
            "relation_descriptor_id": "cc.entity.v1",
            "semantic_major": 1,
            "authoritative_key_tuple": ["entity-1"],
            "condition_universe_id": "universe-1",
            "canonical_condition": "true",
            "interpretation_domain_id": "cc.canonical-1",
            "producer_semantic_contract": "sha256:" + "a" * 64,
            "authoritative_payload_tuple": ["function"],
        }
        ids = claim_identity(value)
        self.assertEqual(set(ids), {"semantic_key_id", "assertion_id", "content_digest"})
        with self.assertRaisesRegex(StoreContractError, "containing-snapshot"):
            claim_identity(dict(value, containing_snapshot_id="snapshot-output"))

    def test_direct_and_derived_basis_are_tagged(self) -> None:
        direct = producer_basis(
            {"kind": "direct", "basis_digest": "sha256:" + "a" * 64}
        )
        derived = producer_basis(
            {
                "kind": "derived",
                "input_snapshot": "snapshot-prior",
                "input_generation": 1,
                "output_generation": 2,
                "consumed_partition_content_digests": ["partition-a"],
                "transform_semantics": "sha256:" + "b" * 64,
            }
        )
        self.assertNotEqual(direct, derived)
        with self.assertRaisesRegex(StoreContractError, "direct-basis-snapshot"):
            producer_basis(
                {
                    "kind": "direct",
                    "basis_digest": "sha256:" + "a" * 64,
                    "input_snapshot": "snapshot-prior",
                }
            )

    def test_every_closure_identity_field_is_digest_bound(self) -> None:
        value = {field: f"value-{index}" for index, field in enumerate(CLOSURE_FIELDS)}
        baseline = closure_binding(value)
        result = closure_mutation_matrix(value)
        self.assertEqual(result["distinct_ids"], len(CLOSURE_FIELDS) + 1)
        for field in CLOSURE_FIELDS:
            changed = copy.deepcopy(value)
            changed[field] += "-changed"
            self.assertNotEqual(baseline, closure_binding(changed), field)

    def test_snapshot_digest_is_invariant_under_all_perturbations(self) -> None:
        base = {
            "snapshot_semantics_version": "1.0.0",
            "catalog_semantic_digest": "sha256:" + "1" * 64,
            "condition_universe_id": "universe-1",
            "relation_registry_digest": "sha256:" + "2" * 64,
            "interpretation_policy_digest": "sha256:" + "3" * 64,
            "closure_ids": ["closure-1"],
            "partitions": [],
        }
        for index in range(3):
            base["partitions"].append(
                {
                    "relation_descriptor_id": "cc.entity.v1",
                    "scope": f"scope-{index}",
                    "condition": "condition-1",
                    "interpretation": "cc.canonical-1",
                    "producer_semantics": "sha256:" + "4" * 64,
                    "input_basis_digest": "sha256:" + "5" * 64,
                    "precision_profile": "exact",
                    "assumption_set_id": "empty",
                    "claim_content_digests": [f"claim-{index}"],
                    "coverage_units": [f"covered-{index}"],
                }
            )
        _, comparisons = snapshot_digest_matrix(base)
        self.assertEqual(comparisons, 36)

    def test_series_selector_has_no_ambient_defaults(self) -> None:
        selector = {field: f"value-{field}" for field in SELECTOR_FIELDS}
        self.assertTrue(series_id(selector).startswith("snapshot-series:sha256:"))
        for field in SELECTOR_FIELDS:
            incomplete = dict(selector)
            del incomplete[field]
            with self.assertRaisesRegex(StoreContractError, "selection-authority"):
                series_id(incomplete)

    def test_current_does_not_fallback_from_corrupt_head(self) -> None:
        selector = {field: f"value-{field}" for field in SELECTOR_FIELDS}
        publications = [
            {
                "publication_id": "p1",
                "selector": selector,
                "sequence": 1,
                "state": "committed",
                "physical_state": "intact",
                "snapshot_id": "s1",
            },
            {
                "publication_id": "p2",
                "selector": selector,
                "sequence": 2,
                "state": "committed",
                "physical_state": "corrupt",
                "snapshot_id": "s2",
            },
        ]
        with self.assertRaisesRegex(StoreContractError, "current-corrupt"):
            select_current({"selector": selector, "publications": publications})

    def test_failed_publish_and_compaction_preserve_prior(self) -> None:
        head, reason = publish(
            {
                "current_head": "p1",
                "expected_parent": "p1",
                "candidate": "p2",
                "validated": False,
                "history": ["created", "staged", "validating", "rejected", "rolled_back"],
            }
        )
        self.assertEqual((head, reason), ("p1", "store.publish-failure-isolated"))
        generation, reason = compact(
            {
                "current_generation": "g1",
                "candidate_generation": "g2",
                "pinned_generations": ["g1"],
                "current_semantic_digest": "d1",
                "candidate_semantic_digest": "d1",
                "candidate_valid": False,
            }
        )
        self.assertEqual(generation["active_generation"], "g1")
        self.assertEqual(reason, "store.compact-failure-isolated")

    def test_format_migration_never_changes_semantic_identity(self) -> None:
        digest = "sha256:" + "a" * 64
        result, reason = format_open(
            {
                "source_format": "1.0.0",
                "reader_major": 2,
                "semantic_digest": digest,
                "migrations": [
                    {"from_major": 1, "to_major": 2, "result_semantic_digest": digest}
                ],
            }
        )
        self.assertEqual(result, digest)
        self.assertEqual(reason, "store.format_migration-valid")
        for changed in ("sha256:" + "b" * 64, "sha256:" + "c" * 64):
            with self.assertRaisesRegex(StoreContractError, "semantic-drift"):
                format_open(
                    {
                        "source_format": "1.0.0",
                        "reader_major": 2,
                        "semantic_digest": digest,
                        "migrations": [
                            {
                                "from_major": 1,
                                "to_major": 2,
                                "result_semantic_digest": changed,
                            }
                        ],
                    }
                )

    def test_canonical_set_inputs_are_order_invariant(self) -> None:
        values = ["a", "b", "c"]
        digests = {
            identity_digest("claim-set", sorted(permutation))
            for permutation in itertools.permutations(values)
        }
        self.assertEqual(len(digests), 1)


if __name__ == "__main__":
    unittest.main()
