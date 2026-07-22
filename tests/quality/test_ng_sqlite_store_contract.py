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
    EXPECTED_SCHEMA_DIGEST,
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
    forced_population_reachable,
    format_reset_projection,
    load_yaml,
    option_a_projection,
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
        self.assertEqual(
            capacity_decision["status"], "accepted"
        )
        self.assertEqual(contract["physical_format"]["current"], "3.0.0")
        self.assertEqual(
            contract["compatibility"]["predecessor_v2"]["readable_format"],
            "2.6.0",
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
            (
                "qualification-report-binding",
                lambda value: value["release_binding"][
                    "qualification_report"
                ].__setitem__("revision_binding", "latest-branch"),
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
                "decision-ref",
                lambda value: value["authority"].__setitem__(
                    "sqlite_decision_adr",
                    "docs/design/adr/0013-ng-sqlite-physical-store.md",
                ),
            ),
            (
                "option-a",
                lambda value: value["df_0200_materialization_ingress"][
                    "sqlite_capacity_decision"
                ].__setitem__("selected_alternative", "B"),
            ),
            (
                "option-a-review-status",
                lambda value: value["df_0200_materialization_ingress"][
                    "sqlite_capacity_decision"
                ].__setitem__(
                    "status", "user-selected-independent-review-pending"
                ),
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
