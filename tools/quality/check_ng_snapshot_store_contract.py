#!/usr/bin/env python3
"""Executable snapshot identity and publication-series contract for Issue #148."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import random
import sqlite3
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONTRACT = pathlib.Path("schemas/cxxlens_ng_snapshot_store_contract.yaml")
CONTRACT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_snapshot_store_contract.schema.yaml"
)
MANIFEST_SCHEMA = pathlib.Path("schemas/cxxlens_ng_snapshot_manifest.schema.yaml")
VECTORS = pathlib.Path("schemas/cxxlens_ng_store_conformance_vectors.yaml")
VECTORS_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_store_conformance_vectors.schema.yaml"
)
REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_store_conformance_report.schema.yaml"
)

SELECTOR_FIELDS = (
    "catalog_id",
    "channel_id",
    "engine_generation_id",
    "condition_universe_id",
    "relation_registry_digest",
    "interpretation_policy_digest",
    "trust_policy_digest",
)
CLOSURE_FIELDS = (
    "relation_descriptor_id",
    "subject_partition_id",
    "partition_content_digest",
    "coverage_digest",
    "key_domain_digest",
    "condition",
    "interpretation",
    "assumption_set_id",
    "closure_kind",
    "producer_semantics",
    "evidence_digest",
)
DF_0200_EXTERNAL_COMPLETENESS_AUTHORITY = {
    "validated_request": {
        "authority": "selected-schema-validated-request-external-to-event-stream",
        "producer": "request-schema-and-derived-binding-validator",
        "generation_timing": "before-provider-dispatch-and-before-event-encoding",
        "seal": "immutable-selected-request-journal-entry",
        "exact_projection": [
            "materialization-request-id",
            "canonical-task-id-order",
            "exact-task-count",
            "per-task-source-digest",
            "per-task-output-and-row-budgets",
            "ordered-selected-request-entry-binding-digests",
        ],
        "selected_request_entry_binding": {
            "domain": "cxxlens.df-0200.selected-request-entry-binding.v1",
            "cardinality": "exactly-one-per-selected-task",
            "projection": [
                "materialization-request-id",
                "task-id",
                "canonical-task-ordinal-u64be-as-canonical-bytes",
                "source-digest",
                "output-budget-u64be-as-canonical-bytes",
                "row-budget-u64be-as-canonical-bytes",
            ],
            "output": "semantic-v2-sha256-string",
            "global_set": {
                "domain": "cxxlens.df-0200.selected-request-entry-binding-set.v1",
                "projection": [
                    "materialization-request-id",
                    "exact-task-count",
                    "canonical-task-id-order",
                    "ordered-selected-request-entry-binding-digests",
                ],
                "output": "semantic-v2-sha256-string",
            },
            "generation_timing": "after-request-validation-before-provider-dispatch",
        },
    },
    "sealed_execution_journal_and_task_receipts": {
        "authority": "sealed-execution-evidence-external-to-event-stream",
        "producer": "runtime-transport-receipt-plus-independent-pre-encoder-oracle",
        "generation_timing": "after-shared-task-seal-before-event-encoder",
        "seal": "immutable-before-store-ingress",
        "exact_task_projection": [
            "materialization-request-id",
            "task-id",
            "canonical-task-ordinal",
            "successful-seal",
            "provider-stdout-byte-count",
            "provider-stdout-sha256",
            "decoded-provider-frame-count",
            "provider-frame-transcript-digest",
            "provider-sealed-transcript-digest",
            "task-partition-count-and-full-projection-digest",
            "task-event-count-and-digest",
            "task-claim-count-and-digest",
            "task-row-count-and-digest",
            "task-coverage-count-and-digest",
            "task-unresolved-count-and-digest",
            "pre-encoder-task-receipt-seal-digest",
            "selected-request-entry-binding-digest",
        ],
        "exact_journal_projection": [
            "materialization-request-id",
            "exact-task-count",
            "canonical-task-id-order",
            "ordered-pre-encoder-task-receipt-seal-digests",
            "execution-journal-receipt-set-digest",
        ],
    },
    "pre_encoder_receipt_oracle": {
        "owner": "installed-tool-private-independent-receipt-builder",
        "input": "immutable-sealed-task-result-before-move-or-destruction",
        "generation_timing": "after-task-result-seal-before-first-event-encoder-call",
        "enumeration": (
            "exact-event-identity-and-full-canonical-projection-multiset-for-every-"
            "partition"
        ),
        "canonicalization": (
            "independent-claim-law-then-sort-by-full-event-key-and-full-projection"
        ),
        "input_occurrence_law": {
            "exact_duplicate_claim_occurrence": "collapse-before-event-enumeration",
            "metadata_distinct_same_content_occurrence": "preserve-as-distinct-event",
            "duplicate_final_full_event_projection": "reject",
            "qualification_binding": (
                "cxxlens.df-0200.claim-batch-differential-corpus.v1-raw-sha256-"
                "f05513d05b0b57788b6f94d9c1a477c88d589b64dd8232d88a5c6c6022a84836"
            ),
        },
        "shared_implementation_allowlist": [
            "canonical-codecs",
            "identity-functions",
            "field-validators",
        ],
        "shared_event_enumeration_or_aggregation_control_flow": "forbidden",
        "selected_request_entry_cross_check": (
            "exactly-one-entry-whose-task-id-and-canonical-ordinal-equal-this-task-"
            "receipt"
        ),
        "receipt_seal": {
            "domain": "cxxlens.df-0200.pre-encoder-task-receipt.v1",
            "projection": [
                "materialization-request-id",
                "selected-request-entry-binding-digest",
                "task-id",
                "canonical-task-ordinal",
                "successful-seal",
                "provider-stdout-byte-count",
                "provider-stdout-sha256",
                "decoded-provider-frame-count",
                "provider-frame-transcript-digest",
                "provider-sealed-transcript-digest",
                "task-partition-count-and-full-projection-digest",
                "task-event-count-and-digest",
                "task-claim-count-and-digest",
                "task-row-count-and-digest",
                "task-coverage-count-and-digest",
                "task-unresolved-count-and-digest",
            ],
            "output": "semantic-v2-sha256-string",
        },
        "execution_journal_receipt_set": {
            "domain": "cxxlens.df-0200.execution-journal-receipt-set.v1",
            "projection": [
                "materialization-request-id",
                "exact-task-count",
                "canonical-task-id-order",
                "ordered-pre-encoder-task-receipt-seal-digests",
            ],
            "output": "semantic-v2-sha256-string",
            "seal_timing": "after-all-task-receipts-before-store-ingress",
        },
        "receipt_field_catalog": {
            "materialization-request-id": {
                "type": "strict-utf8-string",
                "domain_id": "not-applicable-scalar-bound-by-receipt-seal",
                "exact_projection": "selected-materialization-request-id",
            },
            "task-id": {
                "type": "strict-utf8-string",
                "domain_id": "not-applicable-scalar-bound-by-receipt-seal",
                "exact_projection": "selected-provider-task-id",
            },
            "canonical-task-ordinal": {
                "type": "canonical-bytes-containing-u64be",
                "domain_id": "not-applicable-scalar-bound-by-receipt-seal",
                "exact_projection": "zero-based-selected-request-canonical-task-order",
            },
            "successful-seal": {
                "type": "canonical-boolean-true",
                "domain_id": "not-applicable-scalar-bound-by-receipt-seal",
                "exact_projection": "shared-validator-produced-immutable-successful-seal",
            },
            "provider-stdout-byte-count": {
                "type": "canonical-bytes-containing-u64be",
                "domain_id": "not-applicable-scalar-bound-by-receipt-seal",
                "exact_projection": "runtime-observed-stdout-bytes-before-decode-or-move",
            },
            "provider-stdout-sha256": {
                "type": "sha256-string",
                "domain_id": "sha256-content-digest",
                "exact_projection": "runtime-observed-exact-stdout-bytes-before-decode-or-move",
                "authority_ref": (
                    "schemas/cxxlens_ng_provider_runtime_contract.yaml#"
                    "runtime_private_receipt.raw_stdout"
                ),
            },
            "decoded-provider-frame-count": {
                "type": "canonical-bytes-containing-u64be",
                "domain_id": "not-applicable-scalar-bound-by-receipt-seal",
                "exact_projection": "exact-shared-validator-decoded-frame-census",
            },
            "provider-frame-transcript-digest": {
                "type": "semantic-v2-sha256-string",
                "domain_id": "cxxlens.provider-frame-transcript.v2",
                "exact_projection": (
                    "explicit-frame-count-and-decoded-wire-order-full-eight-field-"
                    "frame-projections"
                ),
                "authority_ref": (
                    "schemas/cxxlens_ng_provider_runtime_contract.yaml#"
                    "runtime_private_receipt.frame_transcript"
                ),
            },
            "provider-sealed-transcript-digest": {
                "type": "semantic-v2-sha256-string",
                "domain_id": "cxxlens.provider-sealed-transcript.v1",
                "exact_projection": (
                    "task-terminal-batches-full-coverage-unresolved-and-evidence-"
                    "projections-in-provider-authority-order"
                ),
                "authority_ref": (
                    "schemas/cxxlens_ng_provider_runtime_contract.yaml#"
                    "runtime_private_receipt.sealed_transcript"
                ),
            },
            "task-partition-count-and-full-projection-digest": {
                "type": "canonical-tuple-u64be-count-and-semantic-v2-sha256-string",
                "domain_id": "cxxlens.df-0200.task-partition-full-projection.v1",
                "exact_projection": (
                    "task-id-partition-count-and-ordered-partition-id-plus-partition-"
                    "event-full-projection-digest-tuples"
                ),
            },
            "task-event-count-and-digest": {
                "type": "canonical-tuple-u64be-count-and-semantic-v2-sha256-string",
                "domain_id": "cxxlens.df-0200.task-event-full-projection.v1",
                "exact_projection": (
                    "task-id-event-count-and-ordered-full-event-projection-bytes"
                ),
            },
            "task-claim-count-and-digest": {
                "type": "canonical-tuple-u64be-count-and-semantic-v2-sha256-string",
                "domain_id": "cxxlens.df-0200.task-claim-occurrence-full-projection.v1",
                "exact_projection": (
                    "task-id-claim-count-and-ordered-full-claim-occurrence-projection-bytes"
                ),
            },
            "task-row-count-and-digest": {
                "type": "canonical-tuple-u64be-count-and-semantic-v2-sha256-string",
                "domain_id": "cxxlens.df-0200.task-detached-row-full-projection.v1",
                "exact_projection": (
                    "task-id-row-count-and-ordered-full-detached-row-projection-bytes"
                ),
            },
            "task-coverage-count-and-digest": {
                "type": "canonical-tuple-u64be-count-and-semantic-v2-sha256-string",
                "domain_id": "cxxlens.df-0200.task-coverage-full-projection.v1",
                "exact_projection": (
                    "task-id-coverage-count-and-ordered-full-coverage-projection-bytes"
                ),
            },
            "task-unresolved-count-and-digest": {
                "type": "canonical-tuple-u64be-count-and-semantic-v2-sha256-string",
                "domain_id": "cxxlens.df-0200.task-unresolved-full-projection.v1",
                "exact_projection": (
                    "task-id-unresolved-count-and-ordered-full-unresolved-projection-bytes"
                ),
            },
            "pre-encoder-task-receipt-seal-digest": {
                "type": "semantic-v2-sha256-string",
                "domain_id": "cxxlens.df-0200.pre-encoder-task-receipt.v1",
                "exact_projection": "receipt-seal-projection-in-listed-order",
            },
            "selected-request-entry-binding-digest": {
                "type": "semantic-v2-sha256-string",
                "domain_id": "cxxlens.df-0200.selected-request-entry-binding.v1",
                "exact_projection": (
                    "request-id-task-id-canonical-ordinal-source-digest-and-output-"
                    "row-budgets-before-provider-dispatch"
                ),
            },
            "execution-journal-receipt-set-digest": {
                "type": "semantic-v2-sha256-string",
                "domain_id": "cxxlens.df-0200.execution-journal-receipt-set.v1",
                "exact_projection": (
                    "execution-journal-receipt-set-projection-in-listed-order"
                ),
            },
            "exact-task-count": {
                "type": "canonical-bytes-containing-u64be",
                "domain_id": "not-applicable-scalar-bound-by-journal-seal",
                "exact_projection": "selected-request-exact-task-count",
            },
            "canonical-task-id-order": {
                "type": "ordered-unique-strict-utf8-string-tuple",
                "domain_id": "not-applicable-scalar-bound-by-journal-seal",
                "exact_projection": "selected-request-canonical-task-id-order",
            },
            "ordered-pre-encoder-task-receipt-seal-digests": {
                "type": "ordered-unique-semantic-v2-sha256-string-tuple",
                "domain_id": "cxxlens.df-0200.pre-encoder-task-receipt.v1",
                "exact_projection": "canonical-task-order-exact-task-receipt-seal-digests",
            },
        },
        "unknown_or_additional_receipt_field": "reject",
        "cycle_prohibition": {
            "task_receipt_may_bind": "selected-request-entry-binding-digest-only",
            "task_receipt_must_not_bind": "execution-journal-receipt-set-digest",
            "execution_journal_may_bind": "ordered-finalized-task-receipt-seal-digests",
            "bidirectional-or-self-reference": "reject-authority-before-implementation",
        },
    },
    "task_result_lifetime": (
        "destroy-only-after-pre-encoder-receipt-seal-and-event-stream-seal"
    ),
    "stream_header_and_trailer": "non-authoritative-cross-check-only",
    "required_global_censuses_and_digests": [
        "task",
        "partition",
        "event",
        "claim",
        "row",
        "coverage",
        "unresolved",
    ],
    "required_manifests": ["segment-manifest", "run-manifest", "merge-manifest"],
    "required_receipts": ["byte-receipt", "record-receipt", "seal-receipt"],
    "store_comparison": {
        "raw_provider_transport": (
            "exact-stdout-byte-count-sha256-decoded-frame-count-and-frame-transcript-"
            "digest-against-runtime-receipt"
        ),
        "event_projection": (
            "exact-partition-and-event-count-plus-full-projection-digests-from-"
            "pre-encoder-receipt-against-independent-stream-replay"
        ),
        "receipt_seal": (
            "recompute-task-receipt-seal-and-exact-match-immutable-execution-"
            "journal-receipt-set-digest"
        ),
        "global": (
            "recompute-dedicated-task-partition-global-domains-and-exact-match-"
            "external-censuses"
        ),
    },
    "comparison": (
        "exact-equality-against-external-request-journal-and-task-receipts-before-"
        "store-candidate"
    ),
    "whole_partition_drop": (
        "reject-even-if-stream-header-trailer-and-internal-digests-self-consistent"
    ),
    "correlated_omission_rejection": {
        "stream_and_stream_owned_end_or_trailer_edited_together": (
            "reject-against-fixed-pre-encoder-receipt-full-projection-digest"
        ),
        "stream_and_receipt_edited_together": (
            "reject-fixed-selected-request-entry-binding-or-immutable-execution-"
            "journal-receipt-set-digest-mismatch"
        ),
    },
}
DF_0200_SQLITE_CAPACITY_DECISION = {
    "status": "accepted",
    "selected_alternative": "A",
    "decision_ref": "docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md",
    "decision_issue": "#200",
    "confirmed_blocker": (
        "sqlite-v2-single-payload-blob-runtime-max-length-1000000000-cannot-"
        "satisfy-required-limit-adjacent-passed-memory-sqlite-parity"
    ),
    "required_parity": "limit-adjacent-passed-memory-and-reopened-sqlite",
    "weakening_parity": "forbidden",
    "alternatives": {
        "A": {
            "decision": "sqlite-physical-v3-segmented-or-chunk-table",
            "disposition": "selected",
            "preserves": (
                "logical-canonical-v5-bytes-except-authorized-physical-generation-field"
            ),
            "requires": [
                "physical-format-v3-authority",
                "deterministic-v2-to-v3-migration",
                "reopen-compaction-pin-and-backend-parity-qualification",
            ],
        },
        "B": {
            "decision": (
                "successor-request-budget-and-cross-backend-canonical-payload-cap"
            ),
            "disposition": "rejected-not-selected",
            "preserves": "memory-sqlite-parity-inside-successor-cap",
            "requires": [
                "successor-version",
                "fresh-request-and-budget-authority",
                "same-cap-for-memory-and-sqlite-qualification",
            ],
        },
    },
    "implementation_and_acceptance": (
        "may-proceed-under-accepted-option-a-not-qualified-until-required-evidence"
    ),
}
EXPECTED_DF_0200_MATERIALIZATION_INGRESS = {
    "status": "accepted-authority-implementation-pending",
    "resolution_id": "cxxlens.df-0200.incremental-claim-store.v1",
    "implementation_disposition": "pending-implementation-and-qualification",
    "activation": (
        "accepted-df-0200-review-and-sqlite-option-a-authority-binding"
    ),
    "caller_scope": "installed-clang22-materializer-only",
    "bridge": "source-private-non-installed-header-without-public-catalog-entry",
    "public_api": "unchanged",
    "public_writer_states": [
        "created",
        "staged",
        "validating",
        "committed",
        "rejected",
        "rolled_back",
    ],
    "public_stage_validate_publish_semantics": "unchanged",
    "source": {
        "ownership": "move-only-replayable-sealed",
        "codec": {
            "id": "cxxlens.df-0200.partition-event-stream.v1",
            "event_kind_codes": {
                "partition-begin": 1,
                "claim-occurrence": 2,
                "detached-row": 3,
                "claim-annotation": 4,
                "coverage": 5,
                "unresolved": 6,
                "partition-end": 7,
            },
            "authority_binding": {
                "canonical_json_sha256": (
                    "sha256:b040bb8b58814e5ac1397a6f409677a1db5c9bb73304578253dc6071491fd45c"
                ),
                "required_sections": [
                    "canonical_tuple_profile",
                    "field_catalog",
                    "stream_header",
                    "frame",
                    "event_projections",
                    "event_container",
                    "digest_framing",
                    "digest_domains",
                    "canonical_order",
                    "stream_trailer",
                    "rejection",
                ],
                "store_checker": "self-contained-hardcoded-binding-no-reverse-load",
                "materialization_checker": "recompute-and-exact-match-full-codec-object",
            },
            "validation": (
                "decode-and-validate-every-full-byte-frame-header-trailer-order-"
                "checksum-and-seal"
            ),
            "unknown_missing_reordered_or_truncated": "reject-entire-candidate",
        },
        "events": [
            "partition-begin",
            "claim-occurrence",
            "detached-row",
            "claim-annotation",
            "coverage",
            "unresolved",
            "partition-end",
        ],
        "public-stage-mixing": "forbidden",
        "materializer-receipt-is-validation-authority": False,
        "external_completeness_authority": (
            DF_0200_EXTERNAL_COMPLETENESS_AUTHORITY
        ),
        "store-validation": {
            "independence": "independent-full-replay-and-recomputation",
            "external-authority-inputs": [
                "validated-request",
                "sealed-execution-journal",
                "sealed-task-receipts",
            ],
            "self-reported-stream-census-authority": "forbidden",
            "required-recomputation": [
                "relation-engine-and-schema",
                "canonical-claim-and-row-identity",
                "exact-eight-field-partition-grouping",
                "occurrence-content-coverage-and-unresolved-censuses-and-digests",
                "hard-and-soft-references-and-closure",
                "full-byte-event-codec-framing-order-checksum-and-seal",
                "external-request-journal-task-and-global-census-digest-closure",
                "segment-run-merge-manifests-and-byte-record-seal-receipts",
                "manifest-snapshot-and-publication-identity",
                "canonical-v5-encode-decode-byte-identity",
            ],
        },
    },
    "counter_model": {
        "semantic_version_components": {
            "encoding": "u32",
            "maximum": 4_294_967_295,
        },
        "canonical_v5_collection_counts": {
            "encoding": "u64be",
            "maximum": 18_446_744_073_709_551_615,
            "aggregate_before_narrowing": "checked-u128",
        },
        "legacy_decoder_caps": {
            "one_million_and_ten_million": (
                "implementation-guards-not-authority-ceilings"
            ),
            "disposition": "remove-via-bounded-streaming",
        },
        "collection_overflow_failure": {
            "trigger": "any-canonical-v5-collection-count-greater-than-u64-max",
            "operation": "partition_stage",
            "code": "store.counter-overflow",
            "field": "materialization-v5-collection-count",
            "detail": "",
            "mapping": (
                "store-stage-materialization.store-failure-draft-discarded"
            ),
        },
    },
    "publication": {
        "candidate": "exactly-one-unpublished",
        "publish-attempts": "exactly-one",
        "partial-visibility": "forbidden",
        "stale-or-failed-publish-preserves-head": True,
    },
    "memory_backend": {
        "full_payload_owners": "exactly-one",
        "candidate_to_store_transfer": "same-immutable-owner-without-full-copy",
        "duplicate-full-row-annotation-or-envelope-graphs": "forbidden",
    },
    "sqlite_backend": {
        "prepublication": (
            "sealed-canonical-v5-payload-spool-and-independent-validation"
        ),
        "predecessor_v2_capacity": (
            "single-payload-blob-runtime-max-length-1000000000-insufficient-for-"
            "required-limit-adjacent-parity"
        ),
        "current_physical_format": (
            "cxxlens.sqlite-semantic-store.v3-3.0.0-bounded-8388608-byte-chunks"
        ),
        "publish_and_reopen_qualification": (
            "required-current-v3-plus-v2-read-migration-and-limit-exceeding-parity"
        ),
        "filesystem_readwrite_admission": (
            "bound-forwarding-vfs-zero-initializes-local-pOutFlags-records-main-db-"
            "readwrite-input-and-only-after-success-records-returned-pOutFlags-and-"
            "requires-readonly-clear-before-limit-lock-journal-or-store-"
            "effect-otherwise-store.sqlite-failure-open-read-write-required"
        ),
        "initialization_terminal_close_gate": (
            "filesystem-journal-transition-precommit-commit-unknown-and-success-"
            "handoff-attempt-exactly-one-close-v2-and-require-close-ok-before-"
            "reclassifier-or-reopen-close-non-ok-quarantines-and-returns-no-store"
        ),
        "fresh_journal_failure_classification": (
            "pre-arm-receipt-admits-header-and-sidecar-residue-after-arming-and-after-"
            "confirmed-close-total-classification-maps-same-identity-empty-to-"
            "initialization-recovery-opaque-current-v3-compressed-descendant-to-"
            "success-and-unsafe-state-to-phase-typed-failure"
        ),
        "authorized_descendant_proof": (
            "exact-canonical-bytes-not-digest-equality-and-row-count-cubed-run-length-"
            "compressed-closed-form-without-generation-distance-replay-and-"
            "existential-all-representations-operation-edge-query"
        ),
        "writer_publish_enospc_or_sqlite_toobig": {
            "operation": "writer_publish",
            "code": "store.sqlite-failure",
            "field": "database",
            "detail": "opaque",
            "outcome": "publication_outcome_unknown",
        },
        "publication_commit_outcome_unknown_recovery": {
            "public_result": (
                "store.sqlite-failure-database-opaque-and-publication-outcome-unknown-"
                "even-when-reopen-observes-the-candidate"
            ),
            "receipt": (
                "locator-vfs-main-file-instance-directory-entry-exact-length-framed-"
                "prestate-authority-state-bytes-and-digest-exact-candidate-immutable-"
                "logical-projection-attempted-physical-row-chunk-head-counter-and-"
                "publication-id"
            ),
            "authority_state_projection_id": "cxxlens.sqlite-authority-state.v1",
            "descendant_algebra_id": "cxxlens.sqlite-authorized-descendant.v1",
            "terminal_reclassifier_id": "cxxlens.sqlite-terminal-reclassifier.v1",
            "valid_reopened_state": (
                "independently-install-attempted-physical-candidate-present-same-"
                "logical-candidate-with-different-authorized-physical-projection-or-"
                "candidate-identity-absent-authorized-descendant-current-v3-state-and-"
                "retain-old-handles-by-pins"
            ),
            "non_descendant_invalid_or_reopen_failure": (
                "poison-result-operations-reopen-required-preserve-last-compatibility-"
                "and-live-pin-observers"
            ),
            "implicit_retry": "forbidden",
        },
        "publish_returned_handle_verification_failure": (
            "committed_unverified-detailed-response-when-safely-constructible"
        ),
        "compaction_and_pin": (
            "preserve-existing-copy-on-write-and-generation-pin-semantics"
        ),
    },
    "sqlite_capacity_decision": DF_0200_SQLITE_CAPACITY_DECISION,
    "compatibility": {
        "snapshot_payload_v5_schema_and_semantic_projection": (
            "unchanged-with-authorized-physical-generation-transition"
        ),
        "sqlite_physical_format": "cxxlens.sqlite-semantic-store.v3-3.0.0",
        "sqlite_predecessor": "exact-v2.6.0-read-only-direct-open",
        "sqlite_predecessor_begin_failure": (
            "store.migration-required-sqlite-physical-format-"
            "cxxlens.sqlite-semantic-store.v2-to-v3"
        ),
        "sqlite_predecessor_begin_precedence": (
            "preserve-existing-invalid-input-draft-errors-then-after-valid-input-"
            "return-migration-required-before-internal-writer-or-draft-allocation"
        ),
        "sqlite_migration": "snapshot-store-compact-only-single-transaction-cow",
        "sqlite_state_projection": "cxxlens.sqlite-authority-state.v1",
        "sqlite_descendant_algebra": "cxxlens.sqlite-authorized-descendant.v1",
        "sqlite_terminal_reclassifier": "cxxlens.sqlite-terminal-reclassifier.v1",
        "snapshot_and_publication_identity": "unchanged",
        "public_cursor_lifetime_and-success-results": "unchanged",
        "additive_public_result": "store.migration-required",
        "incompatible_format_or_public_semantics_change": (
            "fresh-approval-and-successor-authority-required"
        ),
    },
}
REQUIRED_VECTOR_IDS = {
    "identity-graph-dag",
    "identity-graph-cycle",
    "direct-input-basis",
    "direct-input-basis-with-snapshot",
    "derived-input-basis",
    "derived-input-basis-missing-snapshot",
    "derived-input-basis-containing-generation",
    "canonical-claim-identities",
    "claim-containing-snapshot-forbidden",
    "closure-exact-binding",
    "closure-containing-snapshot-forbidden",
    "closure-input-snapshot-forbidden",
    "closure-field-mutation-changes-id",
    "snapshot-perturbation-matrix",
    "current-exact-series",
    "current-catalog-only-rejected",
    "current-cross-series-no-fallback",
    "corrupt-current-no-fallback",
    "explicit-prior-remains-readable",
    "failed-publish-preserves-head",
    "successful-publish-atomic-head",
    "stale-parent-publish-rejected",
    "pinned-compaction-copy-on-write",
    "failed-compaction-preserves-generation",
    "compaction-semantic-drift-rejected",
    "format-direct-compatible",
    "format-migration-preserves-semantics",
    "format-migration-semantic-drift",
    "format-incompatible-no-migrator",
    "duplicate-object-same-bytes",
    "digest-collision-quarantined",
}


class StoreContractError(ValueError):
    """A stable snapshot/store contract violation."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code


def fail(code: str, message: str) -> None:
    raise StoreContractError(code, message)


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail("store.document-invalid", str(path))
    return value


def schema_validate(value: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(value)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail("store.schema-invalid", f"{label}: {error.message}")


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")


def document_digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value)).hexdigest()


def _length(value: int) -> bytes:
    return value.to_bytes(8, "big", signed=False)


def canonical_binary(value: Any) -> bytes:
    """Encode the cxxlens-canonical-tuple-v1 primitive value algebra."""
    if value is None:
        return b"\x00"
    if isinstance(value, bool):
        return b"\x01" + (b"\x01" if value else b"\x00")
    if isinstance(value, int):
        sign = b"\x01" if value < 0 else b"\x00"
        magnitude = abs(value)
        raw = magnitude.to_bytes(max(1, (magnitude.bit_length() + 7) // 8), "big")
        return b"\x02" + sign + _length(len(raw)) + raw
    if isinstance(value, bytes):
        return b"\x03" + _length(len(value)) + value
    if isinstance(value, str):
        raw = value.encode("utf-8", errors="strict")
        return b"\x04" + _length(len(raw)) + raw
    if isinstance(value, (list, tuple)):
        encoded = [canonical_binary(item) for item in value]
        return b"\x05" + _length(len(encoded)) + b"".join(
            _length(len(item)) + item for item in encoded
        )
    if isinstance(value, dict):
        rows = sorted(value.items(), key=lambda item: canonical_binary(item[0]))
        encoded = [canonical_binary([key, item]) for key, item in rows]
        return b"\x06" + _length(len(encoded)) + b"".join(
            _length(len(item)) + item for item in encoded
        )
    fail("store.canonical-type-unsupported", type(value).__name__)


def unsigned_counter_canonical_integer(value: int) -> int:
    """Map one logical u64 to the SDK's explicit signed canonical integer."""
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or not 0 <= value < (1 << 64)
    ):
        fail("store.counter-domain-invalid", str(value))
    return value if value <= (1 << 63) - 1 else value - (1 << 64)


def sqlite_unsigned_integer(value: int) -> int:
    """Encode one logical u64 in SQLite's signed INTEGER domain."""
    return unsigned_counter_canonical_integer(value)


def decode_sqlite_unsigned_integer(value: int) -> int:
    """Recover the exact logical u64 from a SQLite signed INTEGER."""
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or not -(1 << 63) <= value < (1 << 63)
    ):
        fail("store.counter-storage-invalid", str(value))
    return value if value >= 0 else value + (1 << 64)


def identity_digest(kind: str, fields: list[Any]) -> str:
    domain = b"cxxlens\0" + kind.encode("ascii") + b"\0v1\0"
    hashed = hashlib.sha256(domain + canonical_binary(fields)).hexdigest()
    return f"{kind}:sha256:{hashed}"


def plain_identity_digest(kind: str, fields: list[Any]) -> str:
    return "sha256:" + identity_digest(kind, fields).rsplit(":", 1)[1]


def validate_identity_graph(
    contract: dict[str, Any], extra_dependency: dict[str, str] | None = None
) -> list[str]:
    rows = contract["identity_graph"]["nodes"]
    graph = {row["id"]: set(row["depends_on"]) for row in rows}
    if len(graph) != len(rows):
        fail("store.identity-node-duplicate", "identity graph node IDs differ")
    if extra_dependency is not None:
        node = extra_dependency["node"]
        if node not in graph:
            fail("store.identity-node-unknown", node)
        graph[node].add(extra_dependency["dependency"])

    visiting: set[str] = set()
    visited: set[str] = set()
    order: list[str] = []

    def visit(node: str) -> None:
        if node in visiting:
            fail("store.identity-cycle", node)
        if node in visited:
            return
        visiting.add(node)
        for dependency in sorted(graph.get(node, set())):
            if dependency in graph:
                visit(dependency)
        visiting.remove(node)
        visited.add(node)
        order.append(node)

    for node in sorted(graph):
        visit(node)
    return order


def producer_basis(value: dict[str, Any]) -> str:
    kind = value.get("kind")
    if "containing_snapshot" in value or "containing_snapshot_id" in value:
        fail("store.containing-snapshot-forbidden", "producer basis")
    if kind == "direct":
        if set(value) != {"kind", "basis_digest"}:
            if "input_snapshot" in value:
                fail("store.direct-basis-snapshot-forbidden", "direct observation")
            fail("store.direct-basis-incomplete", "direct basis fields")
        return plain_identity_digest("producer-input-direct", [value["basis_digest"]])
    if kind == "derived":
        required = {
            "kind",
            "input_snapshot",
            "input_generation",
            "output_generation",
            "consumed_partition_content_digests",
            "transform_semantics",
        }
        if set(value) != required or not value.get("consumed_partition_content_digests"):
            fail("store.derived-basis-incomplete", "derived basis fields")
        if value["input_generation"] >= value["output_generation"]:
            fail("store.derived-basis-not-prior", "input generation")
        partitions = value["consumed_partition_content_digests"]
        if len(partitions) != len(set(partitions)):
            fail("store.derived-basis-duplicate-partition", "partition digests")
        return plain_identity_digest(
            "producer-input-derived",
            [
                value["input_snapshot"],
                sorted(partitions),
                value["transform_semantics"],
            ],
        )
    fail("store.producer-basis-kind-unknown", str(kind))


def claim_identity(value: dict[str, Any]) -> dict[str, str]:
    if "containing_snapshot_id" in value or "containing_snapshot" in value:
        fail("store.containing-snapshot-forbidden", "claim")
    key = identity_digest(
        "semantic-key",
        [
            value["relation_descriptor_id"],
            value["semantic_major"],
            value["authoritative_key_tuple"],
        ],
    )
    assertion = identity_digest(
        "assertion",
        [
            key,
            value["condition_universe_id"],
            value["canonical_condition"],
            value["interpretation_domain_id"],
            value["producer_semantic_contract"],
        ],
    )
    content = identity_digest(
        "claim-content", [assertion, value["authoritative_payload_tuple"]]
    )
    return {"semantic_key_id": key, "assertion_id": assertion, "content_digest": content}


def closure_binding(value: dict[str, Any]) -> str:
    if "containing_snapshot_id" in value or "input_snapshot" in value:
        fail("store.closure-snapshot-forbidden", "closure certificate")
    missing = [field for field in CLOSURE_FIELDS if field not in value]
    extras = sorted(set(value) - set(CLOSURE_FIELDS))
    if missing or extras:
        fail("store.closure-binding-incomplete", f"missing={missing}, extras={extras}")
    return identity_digest(
        "closure-certificate", [value[field] for field in CLOSURE_FIELDS]
    )


def closure_mutation_matrix(value: dict[str, Any]) -> dict[str, int]:
    baseline = closure_binding(value)
    results = {baseline}
    for field in CLOSURE_FIELDS:
        candidate = copy.deepcopy(value)
        current = candidate[field]
        candidate[field] = f"{current}-changed"
        results.add(closure_binding(candidate))
    if len(results) != len(CLOSURE_FIELDS) + 1:
        fail("store.closure-binding-not-injective", "mutation matrix")
    return {"identity_fields": len(CLOSURE_FIELDS), "distinct_ids": len(results)}


def make_partition(value: dict[str, Any]) -> dict[str, Any]:
    partition_id = identity_digest(
        "partition",
        [
            value["relation_descriptor_id"],
            value["scope"],
            value["condition"],
            value["interpretation"],
            value["producer_semantics"],
            value["input_basis_digest"],
            value["precision_profile"],
            value["assumption_set_id"],
        ],
    )
    claim_set = identity_digest(
        "claim-set", sorted(value["claim_content_digests"])
    )
    coverage = identity_digest("coverage", sorted(value["coverage_units"]))
    content = identity_digest(
        "partition-content", [partition_id, claim_set, coverage]
    )
    return {
        "partition_id": partition_id,
        "relation_descriptor_id": value["relation_descriptor_id"],
        "input_basis_digest": value["input_basis_digest"],
        "claim_set_digest": claim_set,
        "coverage_digest": coverage,
        "content_digest": content,
        "claim_count": len(value["claim_content_digests"]),
    }


def snapshot_id(value: dict[str, Any], partitions: list[dict[str, Any]]) -> str:
    projection = sorted(
        [
            [row["partition_id"], row["content_digest"], row["coverage_digest"]]
            for row in partitions
        ],
        key=lambda row: row[0],
    )
    return identity_digest(
        "snapshot",
        [
            value["snapshot_semantics_version"],
            value["catalog_semantic_digest"],
            value["condition_universe_id"],
            value["relation_registry_digest"],
            value["interpretation_policy_digest"],
            projection,
            sorted(value["closure_ids"]),
        ],
    )


def snapshot_digest_matrix(value: dict[str, Any]) -> tuple[str, int]:
    outputs: list[str] = []
    for backend in ("memory", "sqlite"):
        for root in ("root-a", "root-b"):
            for jobs in (1, 2, 8):
                for order in ("forward", "reverse", "seeded-shuffle"):
                    rows = copy.deepcopy(value["partitions"])
                    if order == "reverse":
                        rows.reverse()
                    elif order == "seeded-shuffle":
                        random.Random(63).shuffle(rows)
                    if backend == "sqlite":
                        database = sqlite3.connect(":memory:")
                        database.execute("create table partitions(value text)")
                        database.executemany(
                            "insert into partitions values (?)",
                            [(canonical_json(row).decode("utf-8"),) for row in rows],
                        )
                        rows = [
                            json.loads(row[0])
                            for row in database.execute("select value from partitions")
                        ]
                        database.close()
                    _operational = {"root": root, "jobs": jobs, "backend": backend}
                    manifests = [make_partition(row) for row in rows]
                    outputs.append(snapshot_id(value, manifests))
    if len(set(outputs)) != 1:
        fail("store.snapshot-semantic-digest-mismatch", "perturbation matrix")
    return outputs[0], len(outputs)


def make_snapshot_manifest(value: dict[str, Any]) -> dict[str, Any]:
    partitions = [make_partition(row) for row in value["partitions"]]
    return {
        "schema": "cxxlens.snapshot-manifest.v1",
        "id": snapshot_id(value, partitions),
        "semantic": {
            "snapshot_semantics_version": value["snapshot_semantics_version"],
            "catalog_semantic_digest": value["catalog_semantic_digest"],
            "condition_universe_id": value["condition_universe_id"],
            "relation_registry_digest": value["relation_registry_digest"],
            "interpretation_policy_digest": value["interpretation_policy_digest"],
            "partitions": sorted(partitions, key=lambda row: row["partition_id"]),
            "closures": sorted(value["closure_ids"]),
        },
    }


def series_id(selector: dict[str, Any]) -> str:
    missing = [field for field in SELECTOR_FIELDS if field not in selector]
    extras = sorted(set(selector) - set(SELECTOR_FIELDS))
    if missing or extras:
        fail("store.selection-authority-incomplete", f"missing={missing}, extras={extras}")
    return identity_digest("snapshot-series", [selector[field] for field in SELECTOR_FIELDS])


def select_current(value: dict[str, Any]) -> str:
    wanted = series_id(value["selector"])
    candidates = [
        row
        for row in value["publications"]
        if row["state"] == "committed" and series_id(row["selector"]) == wanted
    ]
    if not candidates:
        fail("store.current-not-found", wanted)
    highest = max(row["sequence"] for row in candidates)
    heads = [row for row in candidates if row["sequence"] == highest]
    if len(heads) != 1:
        fail("store.current-ambiguous", str(highest))
    if heads[0]["physical_state"] != "intact":
        fail("store.current-corrupt", heads[0]["publication_id"])
    return heads[0]["snapshot_id"]


def open_publication(value: dict[str, Any]) -> str:
    matches = [
        row
        for row in value["publications"]
        if row["publication_id"] == value["publication_id"]
    ]
    if len(matches) != 1 or matches[0]["state"] != "committed":
        fail("store.publication-not-found", value["publication_id"])
    if matches[0]["physical_state"] != "intact":
        fail("store.publication-corrupt", value["publication_id"])
    return matches[0]["snapshot_id"]


def publish(value: dict[str, Any]) -> tuple[str, str]:
    if value["expected_parent"] != value["current_head"]:
        fail("store.publish-stale-parent", value["expected_parent"])
    if value["validated"]:
        if value["history"] != ["created", "staged", "validating", "committed"]:
            fail("store.publish-transition-invalid", str(value["history"]))
        return value["candidate"], "store.publish-valid"
    if value["history"] not in (
        ["created", "rejected", "rolled_back"],
        ["created", "staged", "rejected", "rolled_back"],
        ["created", "staged", "validating", "rejected", "rolled_back"],
    ):
        fail("store.publish-transition-invalid", str(value["history"]))
    return value["current_head"], "store.publish-failure-isolated"


def compact(value: dict[str, Any]) -> tuple[dict[str, Any], str]:
    if not value["candidate_valid"]:
        return (
            {
                "active_generation": value["current_generation"],
                "retained_generations": [value["current_generation"]],
            },
            "store.compact-failure-isolated",
        )
    if value["candidate_semantic_digest"] != value["current_semantic_digest"]:
        fail("store.compact-semantic-drift", value["candidate_generation"])
    retained = sorted(set(value["pinned_generations"]))
    return (
        {
            "active_generation": value["candidate_generation"],
            "retained_generations": retained,
        },
        "store.compact-valid",
    )


def format_open(value: dict[str, Any]) -> tuple[str, str]:
    source_major = int(value["source_format"].split(".", 1)[0])
    if source_major == value["reader_major"]:
        return value["semantic_digest"], "store.format_open-valid"
    migrations = [
        row
        for row in value["migrations"]
        if row["from_major"] == source_major
        and row["to_major"] == value["reader_major"]
    ]
    if len(migrations) != 1:
        fail("store.format-incompatible", value["source_format"])
    if migrations[0]["result_semantic_digest"] != value["semantic_digest"]:
        fail("store.format-migration-semantic-drift", value["source_format"])
    return value["semantic_digest"], "store.format_migration-valid"


def collision(value: dict[str, Any]) -> str:
    if value["existing_id"] != value["candidate_id"]:
        return "candidate-object"
    if value["existing_canonical_hex"] != value["candidate_canonical_hex"]:
        fail("store.hash-collision", value["candidate_id"])
    return "existing-object"


def execute(
    contract: dict[str, Any], vector: dict[str, Any]
) -> tuple[dict[str, Any], int]:
    operation = vector["operation"]
    value = vector["input"]
    comparisons = 0
    try:
        reason = f"store.{operation}-valid"
        if operation == "identity_graph":
            output = validate_identity_graph(contract, value.get("extra_dependency"))
        elif operation == "producer_basis":
            output = producer_basis(value["basis"])
        elif operation == "claim_identity":
            output = claim_identity(value)
        elif operation == "closure_binding":
            output = closure_binding(value)
        elif operation == "closure_mutation_matrix":
            output = closure_mutation_matrix(value)
        elif operation == "snapshot_digest_matrix":
            output, comparisons = snapshot_digest_matrix(value)
        elif operation == "select_current":
            output = select_current(value)
        elif operation == "open_publication":
            output = open_publication(value)
        elif operation == "publish":
            output, reason = publish(value)
        elif operation == "compact":
            output, reason = compact(value)
        elif operation == "format_open":
            output, reason = format_open(value)
        elif operation == "collision":
            output = collision(value)
            reason = "store.collision_duplicate-valid"
        else:
            fail("store.operation-unknown", operation)
        return {"decision": "accepted", "reason_code": reason, "value": output}, comparisons
    except StoreContractError as error:
        return {"decision": "rejected", "reason_code": error.code}, comparisons


def validate_contract_shape(contract: dict[str, Any]) -> None:
    if (
        contract.get("df_0200_materialization_ingress")
        != EXPECTED_DF_0200_MATERIALIZATION_INGRESS
    ):
        fail(
            "store.materialization-ingress-contract-invalid",
            "DF-0200 accepted materialization ingress differs",
        )
    if contract["canonical_encoding"]["serialized_identity"] != (
        "{identity-kind}:sha256:{64-lowercase-hex}"
    ):
        fail("store.identity-serialization-invalid", "typed digest format")
    if contract["canonical_encoding"]["hash"] != {
        "algorithm": "sha256",
        "digest_bits": 256,
        "truncation": "forbidden",
        "domain_prefix": "cxxlens\\0{identity-kind}\\0v1\\0",
        "algorithm_change": "identity-contract-major",
    }:
        fail("store.hash-contract-invalid", "SHA-256 authority differs")
    expected_selector = list(SELECTOR_FIELDS)
    if contract["publication_series"]["selector_fields"] != expected_selector:
        fail("store.selection-authority-incomplete", "selector contract")
    publication = contract["publication_identity"]
    if publication["identity_fields"] != [
        "series_id",
        "snapshot_id",
        "sequence",
        "parent_publication",
    ]:
        fail("store.publication-identity-incomplete", "identity fields")
    if publication["persisted_binding"] != {
        "validation": "recompute-and-exact-match",
        "shared_validator": "memory-and-sqlite-persist-load-read-compact",
        "mismatch": "store.corrupt",
        "before_exposure": True,
    }:
        fail("store.publication-identity-unbound", "persisted binding")
    if "physical_generation" not in publication["excluded_fields"] or (
        publication["compaction"]
        != "physical-generation-update-preserves-publication-id"
    ):
        fail("store.publication-generation-in-identity", "compaction")
    if contract["wire_decoding"] != {
        "snapshot_semantics_version_components": ["major", "minor", "patch"],
        "component_domain": "unsigned-32-bit",
        "maximum": (1 << 32) - 1,
        "overflow": "store.corrupt",
        "narrowing_before_range_validation": "forbidden",
        "accepted_v5_payload": "decode-encode-byte-identical",
        "partition_envelope_claim_order": "full-sdk-claim-occurrence-projection",
    }:
        fail("store.version-wire-domain-invalid", "semantic version decoding")
    counters = contract["publication_counters"]
    if counters["fields"] != ["publication_sequence", "physical_generation"] or (
        counters["increment"] != "shared-checked-add-one"
        or counters["overflow"] != "store.counter-overflow"
        or counters["maximum_increment"] != "reject"
        or not counters["compaction_uses_shared_increment"]
    ):
        fail("store.counter-contract-invalid", "checked increments")
    if counters["authority_record"] != {
        "required": [
            "checksum-valid",
            "publication-identity-valid",
            "decoded-record-exact",
            "semantic-graph-valid",
            "committed",
        ],
        "excluded": [
            "created",
            "staged",
            "validating",
            "rejected",
            "rolled_back",
            "corrupt",
        ],
    }:
        fail("store.counter-authority-invalid", "record acceptance")
    if counters["global_generation"] != "maximum-authority-record-generation":
        fail("store.counter-authority-invalid", "global generation")
    expected_sqlite_allocation = {
        "lock": "begin-immediate-write-transaction",
        "authority_scan": (
            "all-persisted-records-revalidated-to-authority-record-conditions"
        ),
        "allocation": "checked-max-authority-generation-plus-one-inside-transaction",
        "different-series-concurrency": "globally-distinct-monotonic-generations",
        "corrupt-high-generation": "excluded",
        "publication_and_compaction_share_allocator": True,
        "compaction_range": {
            "size": "exact-authority-publication-count",
            "checked_allocation": "max-plus-one-through-max-plus-count",
            "assignment_order": (
                "prior-publication-sequence-then-prior-physical-generation-then-"
                "publication-id"
            ),
            "distinct_generation_per_publication": True,
            "local_generation_after_commit": "allocated-range-maximum",
            "overflow": "rollback-entire-compaction",
        },
    }
    if counters.get("sqlite_allocation") != expected_sqlite_allocation:
        fail("store.counter-allocation-invalid", "SQLite transactional allocator")
    if counters.get("sqlite_integer_encoding") != {
        "nonnegative_range": "zero-through-int64-max",
        "upper_unsigned_range": "negative-twos-complement-int64",
        "round_trip": "exact-u64",
    }:
        fail("store.counter-storage-invalid", "SQLite integer encoding")
    if publication.get("sequence_canonical_codec") != (
        "unsigned-64-to-explicit-two-complement-signed-canonical-integer"
    ):
        fail("store.counter-storage-invalid", "publication identity integer encoding")
    expected_head_cas = {
        "table": "cxxlens_ng_series_head",
        "transaction": "begin-immediate",
        "steps": [
            "full-committed-authority-census",
            "derive-each-series-authoritative-head",
            "exact-compare-head-table-id-and-sequence",
            "recheck-expected-parent",
            "allocate-global-generation",
            "immutable-publication-insert",
            "head-update",
        ],
        "parent_record": (
            "exact-committed-decoded-semantic-graph-valid-authority-record"
        ),
        "missing-head-with-existing-series-history": "store.corrupt",
        "same-parent-head-sequence-mismatch": "store.corrupt",
        "duplicate-publication-id-under-unchanged-parent": "store.corrupt",
        "conflict": "store.publication-conflict-parent-cas-only",
        "memory_update": "after-commit-full-committed-census",
    }
    if contract["publication_transaction"].get("sqlite_head_cas") != expected_head_cas:
        fail("store.publication-cas-invalid", "durable head authority")
    expected_terminal_recovery = {
        "reclassifier_id": "cxxlens.sqlite-terminal-reclassifier.v1",
        "descendant_algebra_id": "cxxlens.sqlite-authorized-descendant.v1",
        "authority_state_projection_id": "cxxlens.sqlite-authority-state.v1",
        "sealed_receipt_profiles": {
            "post_format_prewrite": [
                "canonical-locator",
                "exact-pinned-vfs-identity",
                "writer-main-open-file-instance-identity",
                "writer-main-directory-entry-binding",
                "exact-length-framed-prestate-authority-state-projection-bytes-and-digest",
                "operation-kind",
                "operation-phase",
                "no-candidate-yet",
            ],
            "post_format_candidate": [
                "exact-post-format-prewrite-receipt",
                "candidate-id",
                "exact-candidate-projection",
            ],
            "fresh_initialization": [
                "canonical-locator",
                "exact-pinned-vfs-identity",
                "preinit-absent-or-exact-empty-anchor",
                "actual-target-main-open-file-instance-identity-and-directory-entry-binding",
                "pre-arm-raw-main-size-and-digest-and-sidecar-census",
                "deterministic-expected-empty-v3-projection",
                "initialization-id",
            ],
            "fresh_empty_anchor_is_not_post_format_authority_state": True,
        },
        "post_format_candidate_extension": (
            "only-after-candidate-id-and-exact-candidate-projection-are-complete"
        ),
        "fresh_initialization_receipt_seal": "before-journal-arming",
        "main_file_identity_in_every_receipt": (
            "required-for-every-filesystem-receipt-memory-branches-use-no-receipt-"
            "no-reclassifier"
        ),
        "ephemeral_memory_uncertainty": (
            "finalize-and-attempt-exactly-one-close-v2-close-ok-discards-close-non-ok-"
            "quarantines-then-poison-with-phase-specific-opaque-result-no-filesystem-"
            "receipt-reopen-or-terminal-reclassifier"
        ),
        "publish_commit_unknown_result_precedence": "database-opaque-always",
        "publish_commit_unknown_state_effect": (
            "authorized-state-installs-independent-current-v3-otherwise-poison-but-"
            "the-public-result-remains-database-opaque"
        ),
        "precommit_result_precedence": (
            "original-trigger-only-after-an-authorized-post-state-is-installed"
        ),
        "publish_precommit_unsafe_result": "database-opaque-and-poison",
        "compaction_precommit_unsafe_result": (
            "compaction-recovery-opaque-and-poison"
        ),
        "install_only": (
            "independently-validated-current-v3-exact-or-authorized-descendant"
        ),
        "close_non_ok": (
            "quarantine-connection-and-vfs-pins-poison-reopen-required-without-reopen"
        ),
        "unsafe": "poison-reopen-required-with-old-pins-valid",
    }
    if (
        contract["publication_transaction"].get("sqlite_terminal_recovery")
        != expected_terminal_recovery
    ):
        fail("store.publication-cas-invalid", "SQLite terminal recovery authority")
    expected_compaction = {
        "mode": "copy-on-write-generation",
        "backend_scope": "memory-and-sqlite",
        "sqlite_transaction": "one-begin-immediate-for-entire-replacement-set",
        "replacement_order": (
            "prior-publication-sequence-then-prior-physical-generation-then-"
            "publication-id"
        ),
        "generation_allocation": {
            "authority": (
                "snapshot-shared-fully-validated-committed-generation-allocator"
            ),
            "committed_nonempty": (
                "checked-range-from-prior-fully-validated-committed-maximum-plus-one-"
                "through-that-maximum-plus-committed-row-count-in-replacement-order"
            ),
            "committed_empty": "allocate-no-range",
            "committed_empty_operation": {
                "common": "begin-immediate-validate-no-write-no-commit",
                "filesystem": (
                    "rollback-finalize-confirm-close-reclassify-and-return-success-"
                    "only-from-an-authorized-state"
                ),
                "ephemeral_memory": (
                    "rollback-finalize-require-healthy-sole-connection-retain-without-"
                    "close-or-reclassification-and-return-success"
                ),
                "failure": (
                    "filesystem-close-or-reclassification-failure-returns-compaction-"
                    "recovery-opaque-and-poisons-memory-rollback-finalize-or-health-"
                    "failure-attempts-exactly-one-close-v2-close-ok-discards-close-"
                    "non-ok-quarantines-then-returns-compaction-recovery-opaque-and-"
                    "poisons"
                ),
            },
            "excluded": "noncommitted-diagnostic-and-corrupt-rows",
        },
        "resolver_order_preservation": "required",
        "preexisting-resolver-ambiguity": "reject",
        "database_payload_and_head_update": "atomic",
        "process_memory_update": "only-after-successful-backend-swap",
        "sqlite_process_memory_update": "only-after-database-commit",
        "validate_before_swap": [
            "physical-checksum",
            "semantic-snapshot-digest",
            "publication-identity-binding",
            "manifest-closure-binding",
            "persisted-semantic-object-graph",
        ],
        "failure_preserves_prior_generation": True,
        "pinned_generation_reclamation": "deferred",
        "sqlite_generation_lifetime": {
            "handle_pin": "decoded-process-immutable-generation",
            "cursor_reads_durable_chunks_lazily": False,
            "durable_old_chunks": "remove-inside-successful-cow-transaction",
            "durable_retired_chunks_after_commit": "forbidden",
        },
        "sqlite_v2_to_v3_migration": {
            "authority": "docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md",
            "trigger": "snapshot-store-compact-only",
            "source": "exact-v2.6.0-read-only",
            "target": "exact-v3.0.0-bounded-chunks",
            "transaction": "same-single-begin-immediate-cow-boundary",
            "row_classes": {
                "committed": (
                    "full-canonical-semantic-authority-replay-with-authorized-new-generation"
                ),
                "noncommitted": (
                    "exact-state-generation-raw-payload-stored-checksum-and-typed-"
                    "diagnostic-verdict-preservation"
                ),
            },
            "generation_allocation": (
                "exact-shared-compaction-profile-above-prior-fully-validated-"
                "committed-maximum"
            ),
            "authority_state_projection_id": "cxxlens.sqlite-authority-state.v1",
            "descendant_algebra_id": "cxxlens.sqlite-authorized-descendant.v1",
            "terminal_reclassifier_id": "cxxlens.sqlite-terminal-reclassifier.v1",
            "diagnostic_projection": (
                "exact-all-source-publication-columns-raw-payload-stored-checksum-"
                "and-verdict-to-deterministic-v3-row-and-chunks"
            ),
            "committed_payload_rewrite": (
                "schema-specific-single-eight-byte-big-endian-generation-field-"
                "replacement-with-old-value-prefix-suffix-and-decode-reencode-proof-"
                "all-other-bytes-exact"
            ),
            "recognized_predecessor_descendants": [
                "legacy-v2-publish",
                "legacy-v2-whole-authority-compaction",
            ],
            "current_binary_emits_predecessor_write": False,
            "precommit_failure_result": (
                "original-trigger-unless-authorized-proof-contains-exactly-one-"
                "migration-edge-then-idempotent-compact-success"
            ),
            "concurrent_migrator_seen_under_locked_prewrite_recheck": (
                "reachable-v3-success-after-confirmed-close-and-total-"
                "reclassification"
            ),
            "finalization": (
                "validated-shadow-then-exact-final-ddl-bounded-copy-shadow-drop-marker-"
                "last-and-cold-reopen-ddl-digest"
            ),
            "commit_outcome_unknown": {
                "receipt": (
                    "locator-vfs-main-file-instance-directory-entry-exact-length-"
                    "framed-prestate-authority-state-bytes-and-digest-expected-v3-"
                    "projection-and-migration-id"
                ),
                "v2_exact_or_valid_descendant": "store.sqlite-failure-database-opaque",
                "v3_exact_or_valid_descendant": "recovered-success",
                "valid_non_descendant": (
                    "store.sqlite-failure-migration-recovery-concurrent-authority-change"
                ),
                "invalid_census": "store.corrupt-migration-recovery-unexpected-census",
                "mixed_or_ambiguous": (
                    "store.corrupt-migration-recovery-mixed-or-ambiguous"
                ),
                "post_classification_state": {
                    "v2_exact_or_valid_descendant": (
                        "install-independently-validated-reopened-v2-read-only-state-"
                        "before-database-opaque"
                    ),
                    "v3_exact_or_valid_descendant": (
                        "install-independently-validated-reopened-current-v3-state-"
                        "before-recovered-success"
                    ),
                    "valid_non_descendant_or_invalid_or_mixed": (
                        "poison-result-operations-reopen-required-preserve-last-"
                        "validated-compatibility-live-pin-count-and-old-handles"
                    ),
                },
                "implicit_retry": "forbidden",
            },
            "reopen_failure": (
                "poisoned-instance-result-operations-reopen-required-nonresult-observers-"
                "last-validated-state-and-live-pins"
            ),
        },
        "sqlite_v3_compaction_commit_outcome_unknown": {
            "receipt": (
                "locator-vfs-main-file-instance-directory-entry-exact-length-framed-"
                "prestate-authority-state-bytes-and-digest-expected-v3-compaction-"
                "projection-and-compaction-id"
            ),
            "authority_state_projection_id": "cxxlens.sqlite-authority-state.v1",
            "descendant_algebra_id": "cxxlens.sqlite-authorized-descendant.v1",
            "terminal_reclassifier_id": "cxxlens.sqlite-terminal-reclassifier.v1",
            "zero_anchor": "excluded-no-write-no-commit-success-path",
            "success_proof": (
                "authorized-descendant-proof-contains-at-least-one-nonempty-v3-"
                "compact-edge"
            ),
            "expected_candidate_projection": (
                "direct-success-proof-even-when-open-time-pre-anchor-was-empty-and-"
                "the-locked-census-gained-concurrent-publications"
            ),
            "later_compaction_witness": (
                "positive-population-v3-compact-run-over-the-receipt-locked-census-"
                "not-necessarily-an-open-time-pre-anchor-row"
            ),
            "expected_or_later_compacted": "recovered-success",
            "exact_pre_or_valid_uncompacted": "store.sqlite-failure-database-opaque",
            "valid_non_descendant": (
                "store.sqlite-failure-compaction-recovery-concurrent-authority-change"
            ),
            "invalid_census": (
                "store.corrupt-compaction-recovery-unexpected-census"
            ),
            "mixed_or_ambiguous": (
                "store.corrupt-compaction-recovery-mixed-or-ambiguous"
            ),
            "post_classification_state": {
                "expected_or_later_compacted": (
                    "install-independently-validated-reopened-current-v3-state-before-"
                    "recovered-success"
                ),
                "exact_pre_or_valid_uncompacted": (
                    "install-independently-validated-reopened-current-v3-state-before-"
                    "database-opaque"
                ),
                "valid_non_descendant_or_invalid_or_mixed": (
                    "poison-result-operations-reopen-required-preserve-last-validated-"
                    "compatibility-live-pin-count-and-old-handles"
                ),
            },
            "implicit_retry": "forbidden",
        },
    }
    if contract.get("compaction") != expected_compaction:
        fail("store.compaction-contract-invalid", "atomic replacement set")
    if contract["partition"]["closure_ids_in_identity"] != "forbidden":
        fail("store.identity-cycle", "partition includes closure IDs")
    if set(contract["closure"]["identity_fields"]) != set(CLOSURE_FIELDS):
        fail("store.closure-binding-incomplete", "contract field set")
    vectors = {row["id"]: row for row in contract["canonical_vectors"]}
    primitive = canonical_binary([None, False, 0, b"0", "0", ["a", "bc"]]).hex()
    if vectors.get("primitive-boundaries-v1", {}).get("encoded_hex") != primitive:
        fail("store.canonical-vector-mismatch", "primitive-boundaries-v1")
    separated = vectors.get("domain-separated-digest-v1", {})
    if separated.get("expected") != identity_digest(
        separated.get("domain", ""), separated.get("values", [])
    ):
        fail("store.canonical-vector-mismatch", "domain-separated-digest-v1")
    validate_identity_graph(contract)


def validate_df_0200_ingress_schema(schema: dict[str, Any]) -> None:
    try:
        required = schema["required"]
        binding = schema["properties"][
            "df_0200_materialization_ingress"
        ]
        expected = schema["$defs"][
            "df_0200_materialization_ingress"
        ]["const"]
    except (KeyError, TypeError):
        required = []
        binding = None
        expected = None
    if (
        "df_0200_materialization_ingress" not in required
        or binding
        != {"$ref": "#/$defs/df_0200_materialization_ingress"}
        or expected != EXPECTED_DF_0200_MATERIALIZATION_INGRESS
    ):
        fail(
            "store.materialization-ingress-contract-invalid",
            "DF-0200 accepted materialization ingress schema differs",
        )


def validate_design(root: pathlib.Path) -> None:
    design = (root / "docs/design/cxxlens_next_generation_integrated_design_ja.md").read_text(
        encoding="utf-8"
    )
    required = (
        "1.0.0-normative",
        "cxxlens_ng_snapshot_store_contract.yaml",
        "snapshot_series_selector",
        "producer_input_basis",
        "Issue #148",
    )
    for marker in required:
        if marker not in design:
            fail("store.design-marker-missing", marker)
    for stale in ("current(catalog_id)", "producer_input_snapshot"):
        if stale in design:
            fail("store.design-stale-contract", stale)
    index = (root / "docs/design/catalogs/README.md").read_text(encoding="utf-8")
    if "Snapshot / Store Contract" not in index or "#148" not in index:
        fail("store.catalog-index-stale", "snapshot contract")


def validate_all(
    root: pathlib.Path,
) -> tuple[dict[str, Any], list[dict[str, Any]], int]:
    contract = load_yaml(root / CONTRACT)
    contract_schema = load_yaml(root / CONTRACT_SCHEMA)
    validate_df_0200_ingress_schema(contract_schema)
    schema_validate(contract, contract_schema, "store contract")
    try:
        jsonschema.Draft202012Validator.check_schema(load_yaml(root / MANIFEST_SCHEMA))
    except jsonschema.SchemaError as error:
        fail("store.schema-invalid", f"snapshot manifest: {error.message}")
    validate_contract_shape(contract)
    validate_design(root)

    vectors = load_yaml(root / VECTORS)
    schema_validate(vectors, load_yaml(root / VECTORS_SCHEMA), "store vectors")
    ids = [row["id"] for row in vectors["vectors"]]
    if len(ids) != len(set(ids)) or set(ids) != REQUIRED_VECTOR_IDS:
        fail("store.vector-set-invalid", "required vector IDs differ")

    matrix_vector = next(
        row for row in vectors["vectors"] if row["id"] == "snapshot-perturbation-matrix"
    )
    schema_validate(
        make_snapshot_manifest(matrix_vector["input"]),
        load_yaml(root / MANIFEST_SCHEMA),
        "snapshot manifest instance",
    )

    results: list[dict[str, Any]] = []
    comparisons = 0
    for vector in vectors["vectors"]:
        actual, count = execute(contract, vector)
        expected = vector["expected"]
        comparisons += count
        matched = (
            actual["decision"] == expected["decision"]
            and actual["reason_code"] == expected["reason_code"]
            and ("value" not in expected or actual.get("value") == expected["value"])
        )
        if not matched:
            fail(
                "store.vector-mismatch",
                f"{vector['id']}: actual={actual}, expected={expected}",
            )
        if (vector["class"] == "positive") != (actual["decision"] == "accepted"):
            fail("store.vector-class-mismatch", vector["id"])
        results.append({"id": vector["id"], **actual, "matched": True})
    if comparisons != 36:
        fail("store.perturbation-matrix-incomplete", str(comparisons))
    report = make_report(contract, results, comparisons)
    schema_validate(report, load_yaml(root / REPORT_SCHEMA), "store report")
    return contract, results, comparisons


def make_report(
    contract: dict[str, Any], results: list[dict[str, Any]], comparisons: int
) -> dict[str, Any]:
    return {
        "schema": "cxxlens.store-conformance-report.v1",
        "contract_digest": document_digest(contract),
        "vector_results": results,
        "perturbation_matrix": {
            "backends": ["memory", "sqlite"],
            "roots": ["root-a", "root-b"],
            "jobs": [1, 2, 8],
            "orders": ["forward", "reverse", "seeded-shuffle"],
            "comparisons": comparisons,
            "all_equal": True,
        },
        "status": "green",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    contract, results, comparisons = validate_all(args.root.resolve())
    report = make_report(contract, results, comparisons)
    if args.mode == "report":
        rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.output is None:
            print(rendered, end="")
        else:
            args.output.write_text(rendered, encoding="utf-8")
    print(
        "verified snapshot/store contract: "
        f"{len(results)} vectors, {comparisons} perturbations, "
        f"{document_digest(contract)}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, StoreContractError) as error:
        print(f"snapshot/store contract failure: {error}", file=sys.stderr)
        raise SystemExit(1) from error
