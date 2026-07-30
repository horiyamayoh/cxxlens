#!/usr/bin/env python3
"""Fail-closed validation for the accepted NG SQLite physical store contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys
from typing import Any, NoReturn

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONTRACT = pathlib.Path("schemas/cxxlens_ng_sqlite_store_contract.yaml")
CONTRACT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_sqlite_store_contract.schema.yaml"
)
SNAPSHOT_CONTRACT = pathlib.Path(
    "schemas/cxxlens_ng_snapshot_store_contract.yaml"
)

# These digests cover the complete parsed YAML objects. They are intentionally
# independent of the schema so a coordinated contract/schema weakening remains
# fail closed while formatting-only YAML changes remain non-semantic.
EXPECTED_CONTRACT_DIGEST = (
    "sha256:62b610fda31475f3a1eea1b2b1734c6e9839e8cf8ae5d38311a4bc29a976ee17"
)
EXPECTED_SCHEMA_DIGEST = (
    "sha256:579b45268e4ab41bba8b0d7754b29880512f35b033acd78845208ee7cd735f21"
)

EXPECTED_SNAPSHOT_BINDING = (
    "sha256:f4854777dd0c5ab50139e2debe5e2be7a00e25bb4a8588285f228e0cf1f225ac"
)

EXPECTED_SAME_PROCESS_WRITER_MAPPING_LEASE_PROPOSAL_DIGEST = (
    "sha256:a3cbda5086921c6e804cc6bd054a5f638900ee7d177ee35dbac79ae1c83abca5"
)

SOURCE_SHM_READONLY_CAPABILITY: dict[str, Any] = {
    "id": "sqlite-source-shm-readonly-unix-uri-v1",
    "availability_gate": (
        "establish-after-bound-nonmutating-census-identifies-readable-wal-and-"
        "shm-and-before-first-underlying-sqlite-source-xOpen-shm-map-or-authority-"
        "read-otherwise-fail-closed-with-no-post-close-or-private-copy-fallback"
    ),
    "uri": {
        "exact_template": (
            "file:<uppercase-percent-encoded-canonical-absolute-path>?mode=ro&"
            "cache=private&readonly_shm=1"
        ),
        "path_encoding": (
            "canonical-absolute-path-with-every-uri-delimiter-and-non-unreserved-"
            "byte-percent-encoded-using-uppercase-hex"
        ),
        "parameters_in_order": ["mode-ro", "cache-private", "readonly_shm-1"],
        "forbidden": [
            "vfs-parameter",
            "immutable-parameter",
            "user-uri-or-query",
            "unknown-parameter",
        ],
    },
    "open_flags": [
        "SQLITE_OPEN_READONLY",
        "SQLITE_OPEN_URI",
        "SQLITE_OPEN_PRIVATECACHE",
        "SQLITE_OPEN_FULLMUTEX",
    ],
    "omitted_open_flags": ["SQLITE_OPEN_CREATE"],
    "required_runtime_symbols": [
        "sqlite3_sourceid",
        "sqlite3_uri_parameter",
        "sqlite3_uri_key",
    ],
    "vfs_admissibility": (
        "loader-origin-proven-sqlite-unix-default-vfs-or-typed-exact-equivalent-"
        "with-the-same-qualified-nonmutating-shm-contract"
    ),
    "pre_source_behavioral_qualification": {
        "scope": "exact-runtime-plus-vfs-implementation-plus-filesystem-profile",
        "fixture": (
            "source-private-scratch-wal-and-shm-with-no-access-to-the-target-source"
        ),
        "scratch_namespace_binding": (
            "retained-descriptor-relative-producer-cold-and-active-locators-with-"
            "candidate-only-exact-full-path-preservation-no-host-path-reresolution"
        ),
        "target_filesystem_binding": (
            "held-main-wal-and-shm-object-filesystem-profiles-must-all-exactly-"
            "equal-the-retained-parent-and-scratch-profile"
        ),
        "proves": [
            "first-map-no-shm-initialize-truncate-extend-create-delete-or-resize",
            "later-map-no-shm-initialize-truncate-extend-create-delete-or-resize",
            "cantinit-null-heap-wal-index-route",
            "readonly-nonnull-mapped-wal-index-retry-route",
        ],
        "timing": (
            "complete-after-bound-nonmutating-sidecar-census-and-before-first-"
            "underlying-sqlite-source-xOpen"
        ),
        "name-or-uri-spelling-alone": "insufficient",
    },
    "qualification_unavailable_or_failed": {
        "result": {
            "code": "store.backend-unavailable",
            "field": "sqlite",
            "detail": "source-shm-readonly-qualification",
        },
        "effects": "no-underlying-source-xOpen-shm-map-authority-read-or-fallback",
    },
    "source_open_callback_receipt": {
        "timing": (
            "validate-in-owned-main-xOpen-callback-before-delegating-underlying-"
            "source-xOpen-or-performing-source-authority-read-or-shm-map"
        ),
        "fields": [
            "canonical-absolute-main-path",
            "retained-parent-fd-anchored-delegated-main-locator",
            "target-namespace-epoch-token",
            "exact-mode-ro",
            "exact-cache-private",
            "exact-readonly_shm-1",
            "absent-vfs",
            "absent-immutable",
            "absent-user-or-unknown-query",
            "pinned-owned-vfs-alias-and-underlying-identity",
        ],
        "mismatch": (
            "fail-without-delegating-underlying-source-xOpen-and-with-no-authority-"
            "read-shm-map-or-fallback"
        ),
    },
    "target_namespace_epoch": {
        "start": "after-qualification-before-target-xFullPathname-or-xOpen",
        "native_resolution": (
            "main-wal-and-shm-resolve-only-through-one-retained-parent-fd-anchored-"
            "locator-while-logical-canonical-uri-remains-the-callback-authority"
        ),
        "entry_kind": (
            "main-wal-and-shm-must-be-direct-regular-parent-directory-entries-with-"
            "no-symlink-or-other-indirection"
        ),
        "entry_kind_gate": (
            "seal-and-validate-in-bound-source-census-before-main-header-or-any-other-"
            "target-source-read-and-before-native-callback"
        ),
        "logical_host_reresolution": (
            "forbidden-after-epoch-start-native-resolution-and-census-use-only-"
            "retained-parent-fd-and-held-object-receipts"
        ),
        "watch": (
            "namespace-only-create-delete-move-self-ignored-and-queue-overflow-no-"
            "content-modify-or-attrib"
        ),
        "validation": (
            "before-and-after-native-map-and-before-eager-read-transaction-end-with-"
            "fd-relative-exact-census"
        ),
        "loss_or_event": (
            "release-any-native-mapping-with-nonremoving-unmap-and-fail-closed"
        ),
    },
    "shm_map_state_machine": {
        "pre_delegate_source_identity": (
            "current-shm-object-and-directory-entry-must-exactly-match-the-sealed-"
            "source-census-otherwise-zero-native-map-calls"
        ),
        "post_delegate_source_identity": (
            "exact-recheck-required-drift-releases-any-native-mapping-with-"
            "nonremoving-unmap-and-fails-closed"
        ),
        "first_and_later_extend_zero": (
            "delegate-every-call-to-the-qualified-native-vfs"
        ),
        "caller_extend_one": (
            "delegate-to-the-qualified-native-vfs-as-extend-zero-on-first-and-"
            "later-calls-never-pass-the-extension-request"
        ),
        "cantinit_null": "preserve-SQLITE_READONLY_CANTINIT-and-null",
        "readonly_nonnull": (
            "preserve-exact-SQLITE_READONLY-and-the-native-nonnull-mapping"
        ),
        "expected_writer_attach_transition": (
            "SQLITE_READONLY_CANTINIT-null-to-exact-SQLITE_READONLY-nonnull"
        ),
        "any_native_ok": (
            "backend-protocol-violation-fail-closed-never-translate-to-readonly"
        ),
        "same_process_writer_mapping_lease_proposal": {
            "__canonical_sha256__": (
                EXPECTED_SAME_PROCESS_WRITER_MAPPING_LEASE_PROPOSAL_DIGEST
            )
        },
        "readonly_null": "normalize-to-SQLITE_READONLY_CANTINIT-and-null",
        "permanent_delegation_suppression": "forbidden",
        "reset": "successful-delegated-xShmUnmap-only",
        "generic_nonprofile_extend_zero_ok": (
            "remains-legal-outside-this-qualified-readonly-shm-profile"
        ),
    },
    "heap_wal_index_recovery": {
        "trigger": "authentic-SQLITE_READONLY_CANTINIT-with-null-page-zero",
        "connection": "same-sqlite-connection-no-close-or-reopen",
        "lock": (
            "same-connection-WAL_READ_LOCK-0-held-through-complete-eager-decode"
        ),
        "receipt": (
            "held-main-wal-shm-identities-directory-bindings-wal-header-and-salt-"
            "read-lock-zero-and-complete-decoded-logical-projection"
        ),
    },
    "forbidden_fallbacks": [
        "post-close-endpoint-or-digest-only-private-copy",
        "different-connection-reopen",
        "arbitrary-sqlite-error-fallback",
        "unleased-main-wal-copy",
    ],
    "source_effects": (
        "no-main-wal-journal-byte-change-and-no-shm-initialize-truncate-extend-"
        "create-delete-or-resize"
    ),
}

ACTIVE_EXISTING_PROBE_PROFILE: dict[str, Any] = {
    "main_flags": [
        "SQLITE_OPEN_READONLY",
        "SQLITE_OPEN_URI",
        "SQLITE_OPEN_PRIVATECACHE",
        "SQLITE_OPEN_FULLMUTEX",
    ],
    "omitted": ["SQLITE_OPEN_CREATE"],
    "uri": (
        "application-generated-exact-file-uppercase-percent-encoded-canonical-"
        "absolute-path-mode-ro-cache-private-readonly_shm-1"
    ),
    "uri_forbidden": [
        "vfs-parameter",
        "immutable-parameter",
        "user-uri-or-query",
        "unknown-parameter",
    ],
    "vfs": "explicit-owned-forwarding-alias-argument-never-uri-parameter",
    "sidecar_create": "forbidden",
}

ACCEPTED_EMPTY_NORMALIZATION_SOURCE_ANCHOR_RECEIPT = [
    "canonical-locator",
    "exact-pinned-vfs-identity",
    "pinned-sqlite-runtime-identity-and-version",
    "exact-normalization-effect-grammar-profile-receipt",
    "accepted-empty-private-recovery-stable-source-receipt",
    "preinit-exact-empty-wal-header-anchor",
    "pre-coordination-zero-wal-branch-absent-create-or-preexisting-bound-size-zero-open",
    "actual-normalizer-main-open-file-instance-identity-and-directory-entry-binding",
    "retained-authenticated-parent-directory-capability-and-continuous-namespace-epoch",
    "pre-coordination-main-size-and-sha256-and-sidecar-census",
    (
        "pre-coordination-decoded-main-page-size-and-file-offset-zero-database-page-"
        "one-sha256-and-exact-header-byte-range-zero-through-one-hundred-exclusive"
    ),
    "immutable-held-pre-main-exact-byte-snapshot-with-length-and-streaming-byte-receipt",
    "exact-empty-logical-projection",
    "normalization-id",
    (
        "deterministic-expected-post-whole-main-exact-byte-projection-plus-size-and-"
        "sha256-derived-before-effect-from-the-immutable-pre-main-byte-snapshot-by-"
        "streaming-copy-with-only-the-authorized-page-one-field-patch-plus-the-"
        "complete-expected-rollback-empty-logical-projection-and-sidecar-census"
    ),
]

ACCEPTED_EMPTY_NORMALIZATION_RECEIPT = [
    "exact-accepted-empty-normalization-source-anchor-receipt",
    "exact-normalization-effect-grammar-profile-receipt",
    (
        "exact-accepted-empty-normalization-coordination-sequence-two-receipt-with-"
        "denied-sequence-one-prerequisite-and-armed-after-exclusive-true"
    ),
    "later-repeated-exclusive-xLock-source-anchor-and-same-main-entry-recheck",
    (
        "coordination-zero-wal-open-flags-file-instance-identity-directory-entry-"
        "size-and-sha256"
    ),
    "planned-normalization-candidate-id",
]

ACCEPTED_EMPTY_NORMALIZATION_COMPLETED_EDGE_RECEIPT = [
    "exact-accepted-empty-normalization-pre-effect-full-receipt",
    "exact-normalization-effect-grammar-profile-receipt",
    (
        "exact-accepted-empty-normalization-full-sequence-three-receipt-with-"
        "coordination-sequence-two-prerequisite-and-armed-after-exclusive-true"
    ),
    "exact-normalization-bounded-effect-transcript-receipt",
    "exact-coordination-wal-delete-retained-authenticated-parent-fsync-receipt",
    "exact-journal-creation-retained-authenticated-parent-fsync-receipt",
    "confirmed-single-delete-normalization-transition-result",
    "exact-terminal-journal-delete-retained-authenticated-parent-fsync-receipt",
    "exactly-one-confirmed-connection-close",
    (
        "same-main-and-entry-plus-streaming-exact-post-main-byte-equality-to-the-"
        "sealed-deterministic-expected-projection-with-size-and-sha256-only-"
        "acceleration-plus-complete-post-structural-logical-validation-and-sidecar-"
        "census"
    ),
    "sealed-normalization-edge-id",
]

ACCEPTED_EMPTY_NORMALIZATION_OPEN_PROFILE: dict[str, Any] = {
    "main_flags": [
        "SQLITE_OPEN_READWRITE",
        "SQLITE_OPEN_PRIVATECACHE",
        "SQLITE_OPEN_FULLMUTEX",
    ],
    "omitted": ["SQLITE_OPEN_CREATE", "SQLITE_OPEN_URI"],
    "main_precondition": (
        "same-anchored-existing-main-and-directory-entry-bound-to-the-confirmed-"
        "accepted-empty-private-recovery-source-receipt-with-exact-empty-header-"
        "two-two-and-either-no-sidecars-or-one-preexisting-exact-bound-size-zero-wal"
    ),
    "sidecar_create": (
        "only-the-named-zero-wal-accepted-empty-normalization-coordination-effect-"
        "after-the-first-exclusive-callback-seals-the-source-anchor-and-publishes-"
        "coordination-sequence-two-WAL-absent-means-create-and-seal-new-object-"
        "identity-preexisting-size-zero-WAL-means-open-and-seal-the-same-rechecked-"
        "object-identity"
    ),
}

ACCEPTED_EMPTY_NORMALIZATION: dict[str, Any] = {
    "operation": "accepted-empty-normalization",
    "precondition": (
        "prior-accepted-empty-private-recovery-and-confirmed-close-proof-bound-to-"
        "the-stable-source-receipt-same-main-identity-exact-logical-empty-wal-"
        "header-and-either-no-sidecars-or-one-exact-bound-size-zero-wal"
    ),
    "checkpoint": (
        "not-applicable-in-this-normalization-phase-no-explicit-checkpoint-or-busy-"
        "tuple"
    ),
    "page_size_decode": (
        "big-endian-header-bytes-sixteen-through-seventeen-value-one-means-65536-"
        "otherwise-value-must-be-a-power-of-two-from-512-through-32768"
    ),
    "whole_main_bound": (
        "pre-main-byte-count-is-a-positive-integral-multiple-of-the-decoded-page-"
        "size-and-matches-the-complete-valid-sqlite-structure-header-page-count-and-"
        "exact-logical-empty-proof"
    ),
    "connection": (
        "bound-vfs-read-write-no-create-private-cache-fullmutex-initially-write-"
        "denying"
    ),
    "locking_mode": (
        "set-and-confirm-connection-local-exclusive-before-the-first-wal-touch"
    ),
    "gate_stage_order": [
        (
            "initial-write-denying-through-actual-read-write-open-version-limit-"
            "main-identity-and-exact-empty-recheck"
        ),
        "install-pending-accepted-empty-normalization-coordination-arm",
        (
            "first-underlying-exclusive-xLock-callback-rechecks-under-the-held-lock-"
            "seals-the-source-anchor-and-publishes-accepted-empty-normalization-"
            "coordination-sequence-two"
        ),
        (
            "exact-zero-byte-wal-open-while-that-exclusive-lock-remains-continuously-"
            "held"
        ),
        "install-post-coordination-full-arm-request",
        (
            "later-repeated-underlying-pending-full-arm-exclusive-xLock-success-"
            "rechecks-and-seals-the-pre-effect-full-receipt-with-a-planned-"
            "normalization-candidate-id-before-publishing-full-sequence-three"
        ),
    ],
    "coordination_effect": (
        "exact-SQLITE_OPEN_READWRITE-CREATE-WAL-create-or-open-of-one-same-"
        "directory-zero-byte-wal-under-the-continuously-held-exclusive-lock-with-"
        "captured-file-identity-entry-size-and-sha256-no-shm-journal-main-or-wal-"
        "byte-write-truncate-sync-or-authority-effect"
    ),
    "coordination_observation": (
        "immutable-pre-coordination-source-anchor-plus-exact-accepted-empty-"
        "normalization-coordination-stage-receipt-and-zero-wal-open-flags-file-"
        "instance-identity-directory-entry-zero-size-and-sha256-sufficient-for-post-"
        "close-classification-before-the-normalization-receipt-is-sealed"
    ),
    "sealed_receipt": (
        "transaction.recovery_model.terminal_reclassification."
        "sealed_receipt_profiles.accepted_empty_normalization"
    ),
    "completed_edge_receipt": (
        "transaction.recovery_model.terminal_reclassification."
        "sealed_receipt_profiles.accepted_empty_normalization_completed_edge"
    ),
    "normalization_transition": (
        "execute-exactly-one-transaction-free-pragma-journal-mode-delete-after-"
        "full-arm-require-the-single-result-delete-and-never-control-on-diagnostic-"
        "prose"
    ),
    "allowed_effects": [
        (
            "exact-coordination-zero-byte-wal-create-open-followed-after-full-arm-"
            "by-close-and-unlink"
        ),
        (
            "one-same-directory-rollback-journal-lifecycle-create-write-the-derived-"
            "large-sector-record-set-of-exact-original-bound-page-images-sync-"
            "invalidate-with-the-observed-zero-header-write-sync-close-and-unlink"
        ),
        (
            "one-full-bound-page-write-for-each-derived-large-sector-record-page-in-"
            "ascending-page-number-order-with-page-one-equal-to-the-sealed-post-"
            "projection-and-every-other-page-byte-exact-to-its-preimage-plus-one-main-"
            "xSync-flags-two-normal-after-SQLITE_FCNTL_SYNC-under-the-confirmed-"
            "connection-local-synchronous-FULL-setting-for-the-sqlite-internal-wal-"
            "to-rollback-header-transition"
        ),
        (
            "necessary-nonauthority-lock-and-file-control-effects-bound-to-the-"
            "receipt-plus-the-exact-retained-authenticated-parent-directory-journal-"
            "creation-coordination-wal-delete-and-terminal-journal-delete-durability-"
            "receipts-with-platform-path-trace-only-nonauthoritative-evidence"
        ),
    ],
    "forbidden_effects": [
        "main-replacement",
        "main-size-change",
        "any-main-page-outside-the-derived-large-sector-record-set-write",
        "any-non-page-one-main-byte-change",
        "any-wal-byte-write-truncate-or-sync",
        "shm-create-or-effect",
        "any-second-or-nonrollback-journal",
        "any-main-or-journal-truncate",
        "schema",
        "metadata",
        "format-marker",
        "semantic-or-diagnostic-authority",
        "payload-publication-head-or-publication-counter-authority",
        "arbitrary-sql-fallback-or-retry",
        "raw-header-edit-outside-sqlite",
    ],
    "wal_effect_trace": (
        "xOpen-flags-524294-readwrite-create-wal-then-after-full-arm-xFileControl-"
        "PERSIST_WAL-10-xClose-immediate-retained-parent-relative-current-wal-leaf-"
        "regular-identity-byte-check-wrapper-known-name-xDelete-syncDir-zero-with-"
        "zero-xWrite-xTruncate-or-xSync-then-full-fsync-the-retained-authenticated-"
        "parent-before-returning-the-delete-callback-or-journal-create"
    ),
    "journal_sector_profile": {
        "maximum_sector_size": 65_536,
        "runtime_binding": (
            "maximum-sector-size-65536-is-an-exact-pinned-sqlite-pager-"
            "implementation-bound-not-a-stable-public-file-format-guarantee"
        ),
        "writer_effective_S": (
            "temporary-or-powersafe-overwrite-pager-forces-S-512-otherwise-raw-"
            "xSectorSize-less-than-32-maps-to-512-raw-greater-than-65536-maps-to-"
            "65536-and-every-other-raw-value-is-unchanged"
        ),
        "writer_admission": (
            "effective-S-is-a-power-of-two-from-32-through-65536-before-installing-"
            "the-pending-coordination-request"
        ),
        "writer_receipt": (
            "raw-xSectorSize-effective-S-temporary-status-exact-"
            "xDeviceCharacteristics-profile-and-exact-vfs-backend-token-sealed-"
            "before-the-pending-coordination-request"
        ),
        "effect_grammar_admission": (
            "require-the-exact-separately-qualified-xDeviceCharacteristics-and-SQLite-"
            "build-profile-before-installing-the-pending-coordination-request-SQLITE_"
            "IOCAP_SAFE_APPEND-SEQUENTIAL-atomic-family-BATCH_ATOMIC-or-UNDELETABLE_"
            "WHEN_OPEN-and-any-other-trace-altering-bit-or-build-option-require-a-"
            "separate-header-sync-journal-bypass-finalization-and-crash-family-grammar-"
            "and-never-inherit-the-default-matrix"
        ),
        "no_contract_default": (
            "fixed-S-is-forbidden-and-the-observed-pinned-S-512-is-qualification-"
            "evidence-only"
        ),
        "cold_parser_source": (
            "big-endian-u32-sector-size-field-at-journal-header-bytes-20-through-23"
        ),
        "cold_parser_admission": (
            "first-header-parsed-S-is-a-power-of-two-from-32-through-65536-before-"
            "any-layout-arithmetic"
        ),
        "all_header_format_requirement": (
            "every-header-segment-has-the-same-S-and-decoded-page-size"
        ),
        "exact_match": (
            "actual-header-bytes-20-through-23-decode-to-the-same-S-used-for-header-"
            "padding-record-offsets-and-length-validation"
        ),
        "header_layout": (
            "exact-28-byte-big-endian-header-fields-followed-by-an-opaque-"
            "uninterpreted-region-through-byte-S-exclusive-with-the-entire-header-"
            "region-exactly-S-bytes"
        ),
        "writer_header_chunks": (
            "exact-S-byte-header-region-written-in-minimum-of-decoded-page-size-and-"
            "S-byte-chunks-after-writer-admission"
        ),
        "page_record_layout": (
            "for-header-offset-H-and-zero-based-record-r-record-start-is-H-plus-S-"
            "plus-r-times-open-decoded-page-size-plus-8-close-page-number-is-at-"
            "record-start-page-image-is-at-record-start-plus-4-checksum-is-at-"
            "record-start-plus-4-plus-decoded-page-size-and-record-end-is-record-"
            "start-plus-8-plus-decoded-page-size"
        ),
        "large_sector_record_set": (
            "let-P-be-decoded-page-size-Q-be-S-divided-by-P-when-S-is-greater-than-P-"
            "otherwise-one-L-be-the-SQLite-pending-byte-locking-page-and-E-be-the-"
            "ascending-pages-one-through-minimum-of-database-page-count-and-Q-"
            "excluding-L-then-R-is-cardinality-E-the-journal-nRec-is-R-record-r-"
            "contains-page-E-r-and-the-complete-one-header-journal-length-is-S-plus-R-"
            "times-open-P-plus-8-close"
        ),
        "large_sector_main_projection": (
            "SQLite-may-write-every-page-in-E-page-one-is-the-deterministic-post-page-"
            "one-and-every-other-page-is-byte-exact-to-its-sealed-preimage-so-the-"
            "whole-main-projection-still-differs-only-in-the-authorized-page-one-fields"
        ),
        "next_header_layout": (
            "next-header-H-is-the-smallest-S-multiple-at-or-after-the-prior-record-"
            "end-and-its-first-record-starts-at-H-plus-S"
        ),
        "checksum_authority": (
            "high-probability-incomplete-write-guard-only-never-a-substitute-for-"
            "exact-original-page-one-bytes"
        ),
        "cold_parser_scope": (
            "format-classifier-input-only-never-recovery-or-success-authority-"
            "without-a-separately-accepted-receiptless-crash-profile"
        ),
        "invalid-or-mismatched-S": (
            "reject-candidate-before-offset-arithmetic-private-recovery-or-source-"
            "effect"
        ),
    },
    "effect_grammar_profile_receipt": (
        "bind-an-exact-layer-discriminator-plus-loaded-SQLite-DSO-source-id-hash-"
        "build-options-VFS-backend-token-raw-xSectorSize-effective-S-temporary-status-"
        "xDeviceCharacteristics-device-filesystem-profile-decoded-P-database-page-"
        "count-N-and-derived-Q-E-R-the-disposable-qualification-layer-additionally-"
        "binds-the-nonforgeable-fixture-capability-private-root-identity-lifetime-"
        "harness-build-toolchain-run-plan-exact-proposal-review-receipt-and-fresh-"
        "qualification-run-and-candidate-report-IDs-without-requiring-its-not-yet-"
        "produced-report-and-never-authorizes-production-the-production-layer-cannot-"
        "accept-that-capability-and-binds-an-already-accepted-canonical-qualification-"
        "report-digest-and-distinct-production-profile-review-receipt-seal-before-the-"
        "pending-coordination-request-and-inherit-the-receipt-through-the-full-receipt-"
        "bounded-effect-transcript-completed-edge-and-handoff"
    ),
    # The nested production profile remains non-authorizing while the accepted
    # disposable qualification layer stays fail-closed through this digest.
    "receiptless_crash_profile_draft": {
        "__canonical_sha256__": (
            "sha256:988a6a894080a5976e736f2e2508366451585e31c3f120ebbe5d1c11de19a06a"
        )
    },
    "rollback_journal_effect_trace": (
        "xOpen-flags-2054-readwrite-create-main_journal-write-the-exact-S-byte-"
        "incomplete-header-in-minimum-of-P-and-S-byte-chunks-then-for-each-derived-"
        "large-sector-record-page-in-ascending-order-xWrite-4-page-number-xWrite-P-"
        "exact-preimage-xWrite-4-checksum-xSync-flags-2-normal-xWrite-12-at-zero-"
        "valid-magic-and-nRec-R-xSync-flags-2-normal-then-for-each-derived-record-"
        "page-xWrite-P-at-page-offset-with-page-one-postimage-and-every-other-page-"
        "byte-exact-xFileControl-SYNC-21-return-SQLITE_NOTFOUND-12-xSync-flags-2-"
        "normal-xWrite-28-at-zero-zero-header-invalidation-xSync-flags-18-normal-"
        "dataonly-xFileControl-COMMIT_PHASETWO-22-return-SQLITE_NOTFOUND-12-xSync-"
        "flags-2-normal-xClose-then-immediate-retained-parent-relative-current-"
        "journal-leaf-regular-identity-byte-check-wrapper-known-name-xDelete-syncDir-"
        "zero-with-no-xTruncate-and-after-wrapper-delete-success-before-returning-"
        "the-callback-full-fsync-the-retained-authenticated-parent"
    ),
    "parent_directory_effect_trace": (
        "separate-linux-unix-vfs-platform-syscall-evidence-after-the-first-journal-"
        "fdatasync-and-before-the-valid-journal-header-write-opens-the-same-path-"
        "parent-directory-performs-fdatasync-and-closes-it-but-unix-VFS-ignores-"
        "directory-fsync-failure-and-this-observation-is-never-the-per-operation-"
        "durability-receipt"
    ),
    "normalizer_parent_durability_receipt": (
        "after-the-underlying-first-journal-xSync-succeeds-and-before-returning-that-"
        "wrapper-callback-or-arming-the-valid-header-or-any-main-write-full-fsync-the-"
        "preopened-retained-authenticated-parent-directory-fd-require-observed-success-"
        "and-seal-the-same-parent-entry-journal-object-and-namespace-epoch-receipt-"
        "failure-enters-post-journal-pre-main-effect-totality-with-no-valid-header-or-"
        "main-write"
    ),
    "normalizer_delete_identity_limit": (
        "for-each-coordination-wal-or-terminal-journal-xDelete-callback-immediately-"
        "reobserve-through-the-retained-authenticated-parent-fd-that-the-current-"
        "authenticated-known-leaf-is-direct-regular-and-matches-the-sealed-identity-"
        "and-bytes-then-the-wrapper-performs-the-actual-unlink-through-that-same-"
        "retained-parent-capability-relative-known-leaf-with-no-host-path-delete-"
        "delegation-this-never-claims-conditional-or-exact-object-deletion-any-rebind-"
        "or-unexpected-namespace-event-from-final-check-through-xDelete-or-uncertain-"
        "delete-outcome-is-post-effect-durability-and-authority-opaque-with-no-bounded-"
        "poststate-retry-second-snapshot-handoff-edge-seal-or-success-and-a-VFS-or-"
        "platform-unable-to-provide-this-effect-fails-before-arm"
    ),
    "normalizer_coordination_wal_delete_durability_receipt": (
        "after-the-wrapper-retained-parent-relative-coordination-wal-xDelete-syncDir-"
        "zero-succeeds-and-before-returning-that-callback-or-creating-the-journal-full-"
        "fsync-the-same-preopened-retained-authenticated-parent-directory-fd-require-"
        "observed-success-and-bind-the-known-wal-name-delete-event-operation-token-and-"
        "continuous-namespace-epoch-failure-or-unknown-after-delete-is-durability-and-"
        "authority-opaque-with-no-handoff"
    ),
    "normalizer_final_delete_durability_receipt": (
        "after-the-wrapper-retained-parent-relative-terminal-journal-xDelete-syncDir-"
        "zero-succeeds-and-before-returning-that-callback-closing-the-normalizer-or-"
        "sealing-the-completed-edge-full-fsync-the-same-preopened-retained-"
        "authenticated-parent-directory-fd-require-observed-success-and-bind-the-"
        "known-journal-name-delete-event-current-main-operation-token-and-continuous-"
        "namespace-epoch-failure-or-unknown-after-delete-is-durability-opaque-with-no-"
        "handoff"
    ),
    "vfs_parent_sync_admission": {
        "pinned_default_linux_unix": (
            "require-the-exact-parent-directory-effect-trace-plus-the-separate-"
            "normalizer-parent-durability-receipt-normalizer-coordination-wal-delete-"
            "durability-receipt-and-normalizer-final-delete-durability-receipt"
        ),
        "supplied_or_exact_equivalent": (
            "require-independently-qualified-typed-durable-journal-creation-"
            "coordination-wal-delete-and-final-journal-delete-parent-namespace-sync-"
            "equivalents-bound-to-the-exact-vfs-backend-token-retained-authenticated-"
            "parent-fd-current-operation-receipt-and-delete-identity-limit"
        ),
        "qualification_unavailable_or_failed": (
            "fail-before-installing-the-pending-coordination-request-or-any-"
            "normalization-effect"
        ),
        "generic-supplied-vfs-inherits-linux-strace-claim": "forbidden",
    },
    "main_effect_trace": (
        "for-each-derived-large-sector-record-page-in-ascending-order-exactly-one-"
        "xWrite-decoded-page-size-at-file-offset-open-page-number-minus-one-close-"
        "times-P-with-page-one-the-sealed-expected-post-image-and-every-other-page-"
        "the-sealed-byte-exact-preimage-then-xFileControl-SYNC-21-exact-SQLITE_"
        "NOTFOUND-12-and-xSync-flags-2-normal-no-size-change-or-write-outside-the-"
        "derived-set"
    ),
    "normalization_file_control_trace": (
        "each-arming-HAS_MOVED-20-call-uses-a-local-moved-integer-initialized-to-"
        "exact-zero-and-requires-SQLITE_OK-output-zero-the-pending-full-arm-xLock-"
        "exclusive-success-second-call-seals-the-pre-effect-full-receipt-and-"
        "publishes-full-sequence-three-later-PERSIST_WAL-10-MMAP_SIZE-18-HAS_MOVED-"
        "20-and-after-main-sync-COMMIT_PHASETWO-22-PRAGMA-14-HAS_MOVED-20"
    ),
    "normalization_bounded_effect_transcript_receipt": (
        "bind-the-normalization-operation-token-exact-effect-grammar-profile-receipt-"
        "and-record-every-permitted-wal-journal-main-lock-file-control-sync-delete-"
        "parent-sync-close-callback-in-exact-order-with-input-role-flags-offset-bytes-"
        "and-result-output-plus-the-three-parent-durability-receipts-and-require-no-"
        "missing-duplicate-extra-or-out-of-grammar-effect-before-completed-edge-seal"
    ),
    "post_main_raw_projection": (
        "pre-normalization-main-bytes-exact-except-sqlite-defined-page-one-fields-"
        "read-and-write-version-bytes-18-through-19-equal-one-change-counter-bytes-"
        "24-through-27-equal-the-big-endian-u32-modulo-two-to-the-thirty-two-"
        "successor-of-the-pre-change-counter-version-valid-for-bytes-92-through-95-"
        "equal-that-post-change-counter-and-write-library-version-bytes-96-through-"
        "99-equal-the-big-endian-u32-sqlite3_libversion_number-from-the-exact-"
        "pinned-runtime-all-other-bytes-byte-exact"
    ),
    "observed_pinned_runtime_diff": (
        "only-ranges-18-through-20-27-through-28-and-95-through-96-with-two-to-one-"
        "one-to-two-and-one-to-two-respectively"
    ),
    "post_main_validation": (
        "unchanged-size-valid-positive-integer-page-count-complete-valid-rollback-"
        "mode-sqlite-structure-exact-logical-empty-zero-application-id-user-objects-"
        "metadata-and-semantic-or-diagnostic-authority-plus-streaming-exact-byte-"
        "equality-of-the-entire-post-main-to-the-sealed-deterministic-expected-"
        "projection-with-size-and-sha256-only-acceleration-plus-sidecar-census-"
        "captured-for-the-fresh-anchor"
    ),
    "success_postcondition": (
        "only-after-the-exact-normalization-bounded-effect-transcript-and-"
        "coordination-wal-delete-journal-creation-and-terminal-journal-delete-"
        "retained-authenticated-parent-fsync-receipts-finalize-and-exactly-one-"
        "confirmed-close-post-close-total-reclassification-seals-the-completed-"
        "normalization-edge-receipt-binding-the-planned-candidate-and-full-sequence-"
        "three-to-the-same-main-and-entry-unchanged-main-size-valid-positive-integer-"
        "page-count-valid-complete-rollback-mode-sqlite-structure-header-read-and-"
        "write-version-one-zero-application-id-zero-user-objects-metadata-and-"
        "semantic-or-diagnostic-authority-wal-shm-journal-absence-and-streaming-"
        "exact-whole-main-byte-equality-to-the-sealed-expected-post-projection-with-"
        "size-and-sha256-only-acceleration"
    ),
    "handoff": (
        "one-planned-nonretry-handoff-to-ordinary-fresh-initialization-requires-"
        "the-original-preinit-anchor-plus-the-exact-normalization-bounded-effect-"
        "transcript-and-coordination-wal-delete-journal-creation-and-terminal-"
        "journal-delete-retained-authenticated-parent-fsync-receipts-plus-"
        "the-separately-sealed-post-close-completed-physical-normalization-edge-"
        "receipt-plus-the-streaming-exact-post-main-byte-projection-and-sidecar-"
        "census-with-size-and-sha256-only-acceleration-before-the-fresh-receipt-is-"
        "sealed-the-pre-effect-full-receipt-or-planned-candidate-alone-never-"
        "authorizes-handoff"
    ),
    "cold_restart_or_interrupted_handoff": (
        "only-for-a-quiescent-accepted-completed-callback-boundary-trace-with-no-"
        "rebind-or-uncertain-delete-and-with-an-accepted-qualified-receiptless-"
        "profile-a-crash-before-or-after-the-final-delete-parent-fsync-cold-"
        "classifies-as-FI-or-FO-and-enters-only-precreate_census.rollback_header_"
        "exact_empty_candidate-with-no-process-receipt-success-or-operation-history-"
        "inference-rebind-or-uncertain-outcome-uses-the-unbounded-remnant-failure-"
        "rule-and-without-that-profile-fail-closed"
    ),
    "failure_totality": {
        "receipt_drift_before_effect": (
            "return-the-source_drift_result-with-no-store-and-no-arm"
        ),
        "sqlite_open_non_ok": (
            "apply-transaction.connection_lifecycle.sqlite_open_profiles."
            "common_non_ok_return_cleanup"
        ),
        "pre_effect_no_receipt_or_persistent_effect_after_successful_open": (
            "when-no-persistent-effect-occurred-whether-or-not-a-pending-request-"
            "source-anchor-or-coordination-sequence-two-receipt-exists-return-the-"
            "exact-original-triggering-result-and-no-store-after-exactly-one-"
            "confirmed-close-close-non-ok-applies-the-opaque-quarantine-rule"
        ),
        "after_coordination_effect_before_normalization_receipt_seal": (
            "finalize-all-statements-no-transaction-rollback-inference-attempt-"
            "exactly-one-close-v2-and-only-after-close-ok-classify-from-the-immutable-"
            "pre-coordination-source-anchor-plus-exact-gate-and-zero-wal-observation-"
            "and-latest-stage-receipt-never-assume-a-sealed-normalization-edge"
        ),
        "after_normalization_receipt_seal_or_transition_attempt": (
            "finalize-all-statements-no-transaction-rollback-inference-attempt-"
            "exactly-one-close-v2-and-only-after-close-ok-run-the-total-reclassifier-"
            "from-the-pre-effect-full-receipt-and-full-sequence-three-receipt-never-"
            "infer-a-physical-edge-or-success-from-the-planned-candidate"
        ),
        (
            "coordination_wal_or_terminal_journal_final_check_rebind_delete_unknown_"
            "or_parent_fsync_failure"
        ): (
            "return-store.sqlite-failure-sqlite-initialization-recovery-durability-"
            "opaque-treat-authority-as-opaque-with-no-store-bounded-namespace-"
            "poststate-retry-second-snapshot-handoff-edge-seal-or-further-classifier-"
            "effect-and-require-a-later-cold-invocation-to-classify-any-observed-"
            "remnant-fail-closed"
        ),
        "close_non_ok_or_unknown": (
            "quarantine-connection-and-runtime-vfs-pins-no-reopen-or-unregister-"
            "return-store.sqlite-failure-sqlite-initialization-recovery-opaque-and-"
            "no-store"
        ),
        "same_identity_normalized_empty_with_operation_edge": (
            "internal-normalization-completed-continue-fresh-initialization-with-no-"
            "store-yet"
        ),
        "same_identity_prestate_or_normalized_empty_without_operation_edge": (
            "return-store.sqlite-failure-sqlite-initialization-recovery-opaque-and-"
            "no-store"
        ),
        "valid_non_descendant_or_replaced_main": (
            "return-store.sqlite-failure-sqlite-initialization-recovery-concurrent-"
            "authority-change-and-no-store"
        ),
        "invalid_or_mixed": (
            "return-store.corrupt-sqlite-initialization-recovery-partial-or-mixed-"
            "authority-and-no-store"
        ),
        "observation_or_reclassifier_unavailable": (
            "return-store.sqlite-failure-sqlite-initialization-recovery-opaque-and-"
            "no-store"
        ),
        "terminal_reclassifier_source_effect": (
            "forbidden-no-repair-cleanup-or-second-normalization"
        ),
        "post_normalization_fresh_pre_effect_failure": (
            "when-normalization-close-postcondition-are-confirmed-and-fresh-has-"
            "made-no-persistent-effect-return-the-exact-fresh-trigger-and-no-store-"
            "without-claiming-zero-whole-invocation-effect-fresh-close-non-ok-and-"
            "post-arm-paths-retain-existing-fresh-totality"
        ),
    },
    "implicit_retry_or_second_snapshot": "forbidden",
    "qualification": {
        "matrix": (
            "exact-static-and-shared-Cxxlens-qualification-runners-bound-to-the-same-"
            "loaded-SQLite-DSO-identity-source-id-hash-VFS-build-and-device-profile-"
            "not-an-inferred-static-SQLite-runtime"
        ),
        "observed_fixture_page_size": 4096,
        "page_size_vectors": [
            "512-header-512-file-512",
            "1024-header-1024-file-1024",
            "2048-header-2048-file-2048",
            "4096-header-4096-file-4096",
            "8192-header-8192-file-8192",
            "16384-header-16384-file-16384",
            "32768-header-32768-file-32768",
            "65536-header-1-file-65536",
        ],
        "page_size_boundary_qualification_required": [
            "512-header-512-file-512",
            "65536-header-1-file-65536",
        ],
        "sector_page_count_matrix_required": (
            "for-every-supported-P-and-every-qualified-effective-S-let-Q-be-S-div-P-"
            "when-S-is-greater-than-P-otherwise-one-and-cover-the-deduplicated-"
            "positive-database-page-count-set-one-Q-minus-one-Q-and-Q-plus-one-then-"
            "mechanically-prove-L-equals-floor-0x40000000-div-P-plus-one-is-greater-"
            "than-Q-throughout-the-admitted-domain-and-reject-a-synthetic-record-"
            "containing-L"
        ),
        "journal_sector_pinned_default_vector": (
            "qualification-only-raw-xSectorSize-512-effective-S-512-incomplete-"
            "header-single-512-byte-write-at-zero-page-number-one-at-512-page-one-"
            "at-516-and-checksum-at-516-plus-decoded-page-size-never-a-contract-"
            "default"
        ),
        "journal_sector_parameterized_vectors_required": [
            "raw-16-effective-512",
            "raw-32-effective-32",
            "raw-4096-effective-4096",
            "raw-65536-effective-65536",
            "raw-131072-effective-65536",
            "raw-4096-powersafe-overwrite-effective-512",
            "S-65536-page-size-4096-header-written-as-sixteen-4096-byte-chunks",
            (
                "S-65536-page-size-4096-page-count-at-least-16-nRec-16-record-pages-"
                "one-through-16-and-main-writes-page-one-postimage-plus-pages-two-"
                "through-16-exact-preimages"
            ),
        ],
        "journal_sector_negative_vectors_required": [
            "effective-non-power-of-two",
            "parsed-S-below-32",
            "parsed-S-above-65536",
            "header-field-S-mismatch",
            "header-region-length-not-equal-to-S",
            "record-offset-derived-from-512-instead-of-S",
            "record-length-or-page-size-mismatch",
            "padding-bytes-treated-as-authority",
            "fixed-nRec-one-when-S-exceeds-page-size",
            "omitted-large-sector-preimage",
            "injected-locking-page-L-record",
            "main-write-outside-derived-record-set",
            "non-page-one-main-byte-change",
        ],
        "observed_fixture_pre_main_sha256": (
            "e3ba06536f7dbba337dee3c1c5f01b43660ce276abb54c5cee2d5defc5b970aa"
        ),
        "observed_fixture_post_main_sha256": (
            "bd70f69256dee6875161b88a66f56baaf057e8f064a01108d11428b2d7a7b071"
        ),
        "counter_vectors": [
            (
                "pre-change-counter-5-pre-version-valid-for-9-post-change-counter-6-"
                "post-version-valid-for-6"
            ),
            (
                "pre-change-counter-4294967295-pre-version-valid-for-305419896-post-"
                "change-counter-0-post-version-valid-for-0"
            ),
        ],
        "write_library_version_vector": (
            "pre-write-library-version-1-pre-change-counter-7-pre-version-valid-for-"
            "3-post-write-library-version-3045001-post-change-counter-8-post-version-"
            "valid-for-8"
        ),
        "multi_page_freelist_vector": {
            "page_count": 492,
            "page_size": 4096,
            "byte_count": 2_015_232,
            "freelist_pages": 491,
            "pre_main_sha256": (
                "bc708af76e44d33510e7f13224227e34a7a2730baf5e55a5b27c2cdf43c849c8"
            ),
            "post_main_sha256": (
                "11add905bcdca94b67c443e7b74276f2c771f4924ee8a30170db1551af7f00a2"
            ),
            "projection": (
                "unchanged-size-and-page-one-only-authorized-fields-patched-with-"
                "pages-two-through-492-byte-exact"
            ),
        },
        "required": [
            "pre-and-post-exact-file-family-effect-trace",
            (
                "faults-before-and-after-first-coordination-callback-post-"
                "coordination-pre-full-zero-wal-gap-and-before-and-after-second-"
                "full-arm-callback"
            ),
            "skipped-duplicate-reordered-and-wrong-prerequisite-arm-fail-closed",
            (
                "faults-at-every-parameterized-header-chunk-record-field-journal-sync-"
                "valid-header-main-page-write-main-sync-invalidation-close-"
                "coordination-wal-delete-parent-sync-terminal-journal-delete-parent-"
                "sync-and-creation-parent-sync-boundary-including-FI-to-FO"
            ),
            (
                "receiptless-nonhot-hot-invalidated-absent-and-zero-wal-cold-reopen-"
                "and-recrash-idempotence"
            ),
            (
                "rebind-at-every-known-name-unlink-and-wrapper-xDelete-final-check-"
                "boundary"
            ),
            (
                "same-process-shared-and-reserved-separate-process-lock-holder-and-"
                "unrelated-same-inode-fd-close-hazard"
            ),
            "post-normalization-fresh-journal-transition-fault",
            "cold-reopen-exact-outcome",
            (
                "no-effect-replacement-nonempty-mixed-unrecognized-journal-and-"
                "receipt-drift-cases"
            ),
        ],
    },
}

ACCEPTED_EMPTY_EFFECT_GATE_STAGES: dict[str, Any] = {
    "wal_shm_coordination_only": {
        "accepted_empty_normalization_coordination": "forbidden",
        "semantics_expansion_for_accepted_empty_normalization": "forbidden",
    },
    "accepted_empty_normalization_coordination": {
        "operation": "accepted-empty-normalization",
        "entry": (
            "after-the-sealed-accepted-empty-source-anchor-and-before-the-first-wal-"
            "touch"
        ),
        "has_moved_output_totality": (
            "before-each-arming-callback-SQLITE_FCNTL_HAS_MOVED-20-call-initialize-"
            "a-local-moved-integer-to-exact-zero-then-require-exact-SQLITE_OK-and-"
            "output-zero"
        ),
        "coordination_arming_callback": (
            "first-underlying-xLock-exclusive-success-then-local-moved-integer-"
            "initialized-to-exact-zero-before-SQLITE_FCNTL_HAS_MOVED-20-require-"
            "exact-SQLITE_OK-and-output-zero-then-synchronous-same-main-entry-size-"
            "sha256-sidecar-census-and-exact-empty-recheck-under-the-continuously-"
            "held-exclusive-lock-seal-the-immutable-source-anchor-and-only-then-"
            "publish-accepted-empty-normalization-coordination-sequence-two"
        ),
        "sole_persistent_allowance": {
            "exclusive_lock": (
                "verified-held-continuously-from-the-coordination-arming-callback"
            ),
            "flags_integer": 524294,
            "flags": [
                "SQLITE_OPEN_READWRITE",
                "SQLITE_OPEN_CREATE",
                "SQLITE_OPEN_WAL",
            ],
            "object": "exact-one-same-bound-directory-zero-byte-wal-create-or-open",
            "receipt": (
                "captured-wal-file-instance-identity-directory-entry-size-zero-and-"
                "sha256"
            ),
        },
        "otherwise_permitted": [
            "nonpersistent-identity-observation",
            "nonpersistent-lock-observation",
            "nonpersistent-file-control-observation",
        ],
        "forbidden": [
            "main-or-wal-byte-write",
            "main-or-wal-truncate",
            "main-or-wal-sync",
            "main-or-wal-delete",
            "shm-or-rollback-journal-create",
            (
                "schema-metadata-format-marker-semantic-diagnostic-payload-head-"
                "counter-or-process-state-authority-effect"
            ),
        ],
        "full_arming_callback": (
            "later-repeated-underlying-pending-full-arm-xLock-exclusive-success-"
            "while-the-same-exclusive-lock-is-continuously-held-then-local-moved-"
            "integer-initialized-to-exact-zero-before-SQLITE_FCNTL_HAS_MOVED-20-"
            "require-exact-SQLITE_OK-and-output-zero-then-synchronous-same-main-"
            "entry-source-anchor-coordination-sequence-two-zero-wal-receipt-and-"
            "exact-empty-recheck-seal-the-pre-effect-full-receipt-with-only-a-"
            "planned-normalization-candidate-id-and-only-then-publish-the-pending-"
            "full-arm"
        ),
        "full_arming_failure": (
            "any-xLock-HAS_MOVED-or-recheck-failure-leaves-full-unarmed-and-enters-"
            "accepted-empty-normalization-failure-totality-after-close"
        ),
        "receipt_sequence": {
            "denied": (
                "sequence-one-prior-to-pending-request-install-armed-after-"
                "exclusive-false"
            ),
            "coordination": (
                "sequence-two-prerequisite-denied-sequence-one-armed-after-"
                "exclusive-true-only-after-first-HAS_MOVED-source-recheck-and-"
                "source-anchor-seal"
            ),
            "full": (
                "sequence-three-prerequisite-coordination-sequence-two-armed-after-"
                "exclusive-true-only-after-zero-wal-plus-source-recheck-and-pre-"
                "effect-full-receipt-seal"
            ),
            "skipped-duplicate-reordered-or-wrong-prerequisite": "fail-closed",
        },
        "lock_trace": (
            "shared-then-first-exclusive-then-zero-wal-open-then-second-exclusive-"
            "then-zero-wal-close-delete-then-unlock-shared-none-with-no-unlock-"
            "between-the-two-exclusive-callbacks"
        ),
        "next_stage": (
            "only-existing-fully-armed-from-the-underlying-exclusive-xLock-callback-"
            "after-exact-recheck-and-normalization-receipt-seal"
        ),
    },
    "fully_armed": {
        "accepted_empty_normalization_entry": (
            "only-from-accepted-empty-normalization-coordination-after-the-"
            "underlying-exclusive-xLock-callback-rechecks-and-seals-the-receipt"
        )
    },
}

ACCEPTED_EMPTY_TYPED_CONTROL: dict[str, Any] = {
    "operation": "accepted-empty-normalization",
    "transition_fault_boundary": "journal-normalization",
    "terminal_phase": "normalization-transition",
    "normalization_close_fault_event": (
        "accepted-empty-normalization-plus-connection-close"
    ),
    "subsequent_fresh_close_fault_event": "fresh-initialization-plus-connection-close",
    "ordinal_or_total_as_phase_identity": "forbidden",
    "operation_or_phase_aliasing": "forbidden",
}

ACCEPTED_EMPTY_NORMALIZATION_PHYSICAL_EDGE: dict[str, Any] = {
    "id": "cxxlens.sqlite-accepted-empty-normalization-edge.v1",
    "applicability": "accepted-empty-normalization-only",
    "source_class": "exact-logical-empty-preauthority",
    "pre_physical_state": (
        "same-main-header-two-two-exact-empty-and-either-no-sidecars-before-"
        "coordination-or-one-bound-size-zero-wal-used-as-the-coordination-object"
    ),
    "post_physical_state": (
        "same-main-complete-valid-rollback-mode-exact-empty-and-no-sidecars"
    ),
    "seal_timing": (
        "only-after-coordination-wal-delete-journal-creation-and-terminal-journal-"
        "delete-retained-authenticated-parent-fsync-finalize-exactly-one-confirmed-"
        "close-and-post-close-total-reclassification"
    ),
    "candidate_receipt_presence_alone": "never-edge-or-success-authority",
    "proof": (
        "exact-completed-edge-receipt-binding-the-planned-candidate-plus-source-"
        "coordination-full-arm-and-exact-normalization-bounded-effect-transcript-"
        "receipts-to-the-same-main-and-entry-plus-all-three-parent-durability-"
        "receipts-plus-streaming-exact-byte-compare-against-the-sealed-expected-post_"
        "main_raw_projection-with-size-and-sha256-only-acceleration-plus-complete-"
        "post-structural-logical-validation-and-sidecar-census"
    ),
    "with_edge_terminal_class": "authorized-post-state-with-operation-edge",
    "prestate_terminal_class": "authorized-pre-state",
    "normalized_without_edge_terminal_class": (
        "authorized-post-state-without-operation-edge"
    ),
    "exact_empty_early_return_before_physical_edge_test": "forbidden",
    "store_authority_descendant_algebra_membership": "forbidden",
    "state_install_or_public_store_success": "forbidden",
}

NORMALIZED_EMPTY_INTERRUPTED_HANDOFF: dict[str, Any] = {
    "raw_candidate": (
        "existing-nonzero-main-header-read-and-write-version-one-and-wal-shm-"
        "journal-absent"
    ),
    "private_classification": (
        "bounded-held-main-only-copy-opened-with-transaction.connection_lifecycle."
        "sqlite_open_profiles.quiescent_private_snapshot-for-complete-valid-sqlite-"
        "structure-and-exact-logical-empty-proof"
    ),
    "private_close": (
        "exactly-one-confirmed-close-before-source-recheck-or-fresh-route"
    ),
    "private_open_or_close_failure": {
        "open_non_ok": (
            "apply-transaction.connection_lifecycle.sqlite_open_profiles."
            "common_non_ok_return_cleanup-and-return-the-selected-open-error-with-no-"
            "source-effect-or-fresh-receipt"
        ),
        "close_non_ok_or_unknown": (
            "quarantine-the-private-connection-and-runtime-vfs-pins-return-store."
            "sqlite-failure-sqlite-initialization-recovery-opaque-with-no-source-"
            "effect-or-fresh-receipt"
        ),
    },
    "source_recheck": (
        "same-main-entry-size-sha256-and-sidecar-census-exact-to-the-pre-copy-receipt"
    ),
    "drift_result": {
        "code": "store.sqlite-failure",
        "field": "sqlite-initialization-sidecar",
        "detail": "concurrent-source-change",
    },
    "source_effects": "none",
    "precedence": (
        "before-the-nonzero-main-unconditional-header-two-two-gate-and-before-"
        "expected-wal"
    ),
    "accepted_empty": (
        "zero-application-id-user-objects-metadata-and-semantic-or-diagnostic-"
        "authority-enters-ordinary-fresh-initialization-as-an-exact-empty-anchor-"
        "with-a-new-fresh-receipt-no-process-receipt-success-inference"
    ),
    "nonempty_v2_current_or_unknown": (
        "never-relax-the-v2-or-current-v3-header-two-two-wal-oracle-route-to-the-"
        "existing-exact-format-incompatible-corrupt-or-sqlite-journal-mode-expected-"
        "wal-result"
    ),
}


# These requirements intentionally duplicate the safety-critical Option A
# projection instead of deriving it from the schema.  The full-document
# digests protect the complete accepted authority; this projection remains an
# executable, reviewable oracle for the recovery/open semantics.
OPTION_A_REQUIREMENTS: tuple[tuple[tuple[str, ...], Any], ...] = (
    (
        ("runtime", "missing_runtime", "cases", "unsupported_platform"),
        {
            "code": "store.backend-unavailable",
            "field": "sqlite",
            "detail": "platform",
        },
    ),
    (
        ("runtime", "required_symbols", "source_shm_readonly"),
        ["sqlite3_sourceid", "sqlite3_uri_parameter", "sqlite3_uri_key"],
    ),
    (
        ("runtime", "missing_runtime", "source_shm_readonly_symbols"),
        (
            "use-the-existing-required-symbol-missing-tuple-only-after-active-wal-"
            "census"
        ),
    ),
    (
        (
            "runtime",
            "capability_preflight",
            "active_wal_source_shm_readonly_order",
        ),
        {
            "steps": [
                (
                    "bind-nonmutating-census-and-classify-readable-wal-plus-shm-"
                    "with-v2-wal-header-before-the-branch-specific-symbol-gate"
                ),
                (
                    "resolve-sqlite3-sourceid-sqlite3-uri-parameter-and-sqlite3-"
                    "uri-key-from-the-same-pinned-runtime-handle"
                ),
                (
                    "qualify-the-exact-runtime-vfs-and-filesystem-profile-on-target-"
                    "independent-scratch-before-underlying-source-xOpen"
                ),
                (
                    "validate-the-exact-internal-uri-receipt-in-the-owned-main-"
                    "xOpen-callback-before-delegation"
                ),
                "open-and-eagerly-decode-on-the-same-connection-and-held-read-lock",
            ],
            "quiescent_exact_v2": (
                "no-source-shm-readonly-symbol-version-or-behavioral-"
                "qualification-gate"
            ),
        },
    ),
    (
        (
            "compatibility",
            "predecessor_v2",
            "read_path_strategy",
            "active_wal",
            "open",
        ),
        (
            "sqlite-open-v2-application-generated-strict-source-uri-readonly-uri-"
            "privatecache-fullmutex-no-create-explicit-owned-vfs"
        ),
    ),
    (
        (
            "compatibility",
            "predecessor_v2",
            "read_path_strategy",
            "active_wal",
            "source_shm_readonly_capability",
        ),
        SOURCE_SHM_READONLY_CAPABILITY,
    ),
    (
        (
            "transaction",
            "connection_lifecycle",
            "sqlite_open_profiles",
            "active_existing_probe",
        ),
        ACTIVE_EXISTING_PROBE_PROFILE,
    ),
    (
        (
            "transaction",
            "connection_lifecycle",
            "sqlite_open_profiles",
            "accepted_empty_normalization",
        ),
        ACCEPTED_EMPTY_NORMALIZATION_OPEN_PROFILE,
    ),
    (
        ("runtime", "capability_preflight", "error_precedence"),
        {
            "common_locator": [
                "empty-or-invalid-locator",
                "existing-library-base-symbol-or-platform-tuple",
            ],
            "filesystem_common": [
                "source-vfs-lifetime-and-observation-capability",
                "vfs-canonicalization",
                "namespace-observation-io",
                "sidecar-topology",
                "regular-file-or-backend-equivalent-object-kind",
                "source-open-and-base-format-discriminator",
            ],
            "exact_v2_existing": [
                "common-locator-and-filesystem-prefix",
                "exact-v2-schema-codec-storage-class-and-eager-row-classification",
            ],
            "unknown_or_mixed_existing": [
                "common-locator-and-filesystem-prefix",
                "base-discriminator-terminal-result",
            ],
            "declared_current_v3_existing": [
                "common-locator-and-filesystem-prefix",
                "v3-required-symbols",
                "sqlite-runtime-version",
                "current-v3-read-only-full-validation",
                "existing-main-read-write-open-with-create-and-uri-omitted",
                "bound-vfs-read-write-mode-proof",
                "actual-connection-sqlite-limit-length",
            ],
            "filesystem_fresh": [
                "common-locator-and-filesystem-prefix",
                "v3-required-symbols",
                "sqlite-runtime-version",
                "explicit-alias-scratch-memory-open-and-limit",
                "exclusive-bootstrap-create-file-and-parent-durability",
                "fresh-main-read-write-open-with-create-and-uri-omitted",
                "bound-vfs-read-write-mode-proof",
                "xLock-journal-transition",
            ],
            "migration_or_recovery": [
                "common-locator-and-filesystem-prefix",
                "clean-source-profile",
                "v3-required-symbols",
                "sqlite-runtime-version",
                "source-main-read-write-no-create-open",
                "bound-vfs-read-write-mode-proof",
                "actual-connection-sqlite-limit-length",
                "begin-immediate",
            ],
            "ephemeral_memory": [
                "common-locator",
                "v3-required-symbols",
                "sqlite-runtime-version",
                "loader-origin-default-vfs-alias-binding",
                "sole-memory-read-write-create-open",
                "actual-target-sqlite-limit-length",
            ],
        },
    ),
    (
        (
            "transaction",
            "connection_lifecycle",
            "sqlite_open_profiles",
            "common_non_ok_return_cleanup",
        ),
        {
            "applicability": (
                "every-sqlite3-open-v2-call-in-every-profile-including-probes-"
                "private-copies-writers-accepted-empty-normalization-fresh-recovery-"
                "scratch-and-ephemeral-memory"
            ),
            "null_handle": (
                "return-the-selected-profile-open-error-with-no-close-retry-"
                "reclassifier-or-authority-effect"
            ),
            "nonnull_handle": (
                "prepare-or-step-is-forbidden-after-the-non-ok-return-finalize-no-"
                "statements-and-attempt-exactly-one-sqlite3-close-v2-before-return"
            ),
            "close_ok": (
                "discard-the-handle-and-return-the-selected-profile-open-error-with-"
                "no-retry-reclassifier-or-new-authority-effect"
            ),
            "close_non_ok_or_unknown": (
                "quarantine-the-handle-and-associated-runtime-vfs-pins-do-not-"
                "unregister-reopen-or-retry-and-return-the-selected-profile-open-"
                "error-with-no-store"
            ),
            "fresh_nonexistent_exception": (
                "the-already-durable-raw-bootstrap-zero-byte-main-may-remain-but-is-"
                "never-format-or-store-authority"
            ),
        },
    ),
    (
        (
            "transaction",
            "connection_lifecycle",
            "existing_v3",
            "failure_effects",
        ),
        (
            "apply-sqlite_open_profiles.common_non_ok_return_cleanup-for-an-open-"
            "non-ok-otherwise-apply-runtime.capability_preflight.recheck_failure_"
            "effects-for-the-reached-pre-effect-phase"
        ),
    ),
    (
        (
            "transaction",
            "connection_lifecycle",
            "fresh_or_empty",
            "open_failure_effects",
        ),
        "apply-sqlite_open_profiles.common_non_ok_return_cleanup",
    ),
    (
        (
            "transaction",
            "fresh_v3_initialization",
            "guards",
            "filesystem",
            "nonexistent_creation_bootstrap",
            "environmental_create_or_open_failure_effects",
        ),
        (
            "raw-create-failure-retains-no-open-handle-target-sqlite-open-non-ok-"
            "applies-transaction.connection_lifecycle.sqlite_open_profiles.common_"
            "non_ok_return_cleanup"
        ),
    ),
    (
        (
            "transaction",
            "connection_lifecycle",
            "read_write_fallback_to_read_only",
        ),
        {
            "proof": (
                "owned-forwarding-vfs-zero-initializes-local-out-flags-passes-its-"
                "nonnull-address-to-underlying-xOpen-records-input-main-db-and-"
                "readwrite-request-and-only-after-success-records-the-returned-"
                "flags-and-requires-them-not-to-contain-SQLITE_OPEN_READONLY"
            ),
            "local_out_flags_before_call": "exact-integer-zero",
            "do_not_require_output_echo": (
                "SQLITE_OPEN_MAIN_DB-and-SQLITE_OPEN_READWRITE-are-input-role-and-"
                "request-only"
            ),
            "supplied_vfs": (
                "the-owned-alias-records-the-same-underlying-xOpen-input-and-output-"
                "proof"
            ),
            "ephemeral_memory": (
                "exempt-no-filesystem-main-and-the-sole-read-write-create-branch-is-"
                "separately-owned"
            ),
            "failure": {
                "code": "store.sqlite-failure",
                "field": "open",
                "detail": "read-write-required",
            },
            "failure_effects": (
                "finalize-and-attempt-exactly-one-close-v2-on-the-unarmed-connection-"
                "no-sqlite-journal-recovery-schema-metadata-or-store-effect-no-"
                "fallback-or-retry-the-fresh-nonexistent-branch-may-retain-only-its-"
                "already-durable-raw-bootstrap-zero-byte-main-close-non-ok-"
                "quarantines-the-connection-and-runtime-vfs-pins"
            ),
            "timing": (
                "after-successful-filesystem-main-open-and-before-limit-synchronous-"
                "arming-lock-journal-recovery-schema-or-data-effect"
            ),
        },
    ),
    (
        (
            "runtime",
            "capability_preflight",
            "recheck_failure_effects",
        ),
        {
            "filesystem_before_sealed_operation_or_recovery_receipt": (
                "finalize-no-live-statement-and-attempt-exactly-one-sqlite3-close-"
                "v2-close-ok-returns-the-selected-pre-effect-gate-error-with-no-"
                "store-install-reclassifier-or-retry-close-non-ok-quarantines-the-"
                "connection-and-runtime-vfs-pins-the-fresh-nonexistent-branch-may-"
                "retain-only-its-already-durable-raw-bootstrap-zero-byte-main"
            ),
            "filesystem_after_sealed_receipt_or_coordination_effect": (
                "finalize-all-statements-attempt-one-rollback-if-a-transaction-may-"
                "remain-then-attempt-exactly-one-sqlite3-close-v2-close-ok-"
                "delegates-to-the-receipt-aware-operation-phase-classifier-close-"
                "non-ok-quarantines-and-poisons-without-reopen-or-retry"
            ),
            "ephemeral_memory_before_authority": (
                "finalize-all-statements-and-attempt-exactly-one-sqlite3-close-v2-"
                "close-ok-discards-the-sole-database-and-returns-the-selected-pre-"
                "effect-gate-error-close-non-ok-quarantines-the-connection-and-"
                "runtime-vfs-pins-no-filesystem-receipt-reopen-reclassifier-or-retry"
            ),
            "after_begin_immediate_epoch_check": (
                "delegate-to-transaction.connection_lifecycle.mutation_epoch_"
                "recheck.drift_effects"
            ),
        },
    ),
    (
        (
            "transaction",
            "connection_lifecycle",
            "mutation_epoch_recheck",
        ),
        {
            "existing_v3_publish_or_compaction": {
                "lock": (
                    "begin-immediate-before-any-schema-metadata-payload-or-head-write"
                ),
                "first_check": (
                    "held-main-header-read-and-write-version-bytes-2-and-connection-"
                    "pragma-journal-mode-exact-wal"
                ),
                "then": "exact-format-schema-all-row-head-and-counter-anchor",
            },
            "fresh_filesystem_initialization": {
                "before_transaction": (
                    "open-the-bootstrapped-main-read-write-no-create-with-effects-"
                    "denied-seal-exact-empty-identity-size-and-digest-then-on-the-"
                    "underlying-vfs-xLock-exclusive-success-recheck-that-receipt-and-"
                    "only-then-arm-journal-header-and-sidecar-effects-for-set-and-"
                    "verify-wal-outside-any-transaction"
                ),
                "transaction": (
                    "begin-immediate-then-recheck-wal-header-empty-identity-and-"
                    "authority-anchor-before-ddl-or-metadata"
                ),
            },
            "ephemeral_memory_initialization": {
                "transaction": (
                    "begin-immediate-on-the-sole-connection-then-recheck-memory-"
                    "journal-and-empty-authority-before-ddl-or-metadata"
                ),
            },
            "migration_or_preauthority_recovery": (
                "use-the-named-exclusive-lease-journal-transition-and-post-lock-"
                "revalidation-before-any-schema-or-data-write"
            ),
            "drift_result": {
                "code": "store.sqlite-failure",
                "field": "sqlite-journal-mode",
                "detail": "drift-before-write",
            },
            "drift_effects": {
                "common": (
                    "perform-zero-schema-metadata-payload-head-counter-or-process-"
                    "state-write-finalize-the-current-statement-attempt-one-rollback-"
                    "if-the-transaction-may-remain-and-never-retry"
                ),
                "filesystem_current_v3_publish": (
                    "delegate-to-transaction.publish.precommit_failure-using-the-"
                    "sealed-post_format_prewrite-receipt-healthy-confirmed-rollback-"
                    "may-return-the-drift-error-uncertain-cleanup-requires-confirmed-"
                    "close-before-reclassification-close-non-ok-quarantines-and-"
                    "poisons"
                ),
                "filesystem_current_v3_compaction": (
                    "delegate-to-compaction.failure-using-the-sealed-post_format_"
                    "prewrite-receipt-healthy-confirmed-rollback-may-return-the-"
                    "drift-error-uncertain-cleanup-requires-confirmed-close-before-"
                    "reclassification-close-non-ok-quarantines-and-poisons"
                ),
                "fresh_filesystem_initialization": (
                    "delegate-to-transaction.initialization.precommit_failure-using-"
                    "the-already-sealed-fresh_initialization-receipt-and-its-exactly-"
                    "one-close-gate-before-any-reclassification"
                ),
                "migration_or_preauthority_recovery": (
                    "delegate-to-migration.source_connection.precommit_failure-or-"
                    "the-matching-recovery-phase-rule-using-its-sealed-receipt-and-"
                    "confirmed-close-gate"
                ),
                "ephemeral_memory_initialization_or_mutation": (
                    "delegate-to-the-matching-memory-precommit-rule-with-no-"
                    "filesystem-receipt-or-reclassifier-uncertainty-finalizes-and-"
                    "attempts-exactly-one-close-v2-close-ok-discards-close-non-ok-"
                    "quarantines"
                ),
            },
        },
    ),
    (
        (
            "transaction",
            "connection_lifecycle",
            "vfs_binding",
            "filesystem_source",
            "readwrite_open_observation",
        ),
        (
            "zero-initialize-local-out-flags-record-underlying-xOpen-input-main-db-"
            "and-readwrite-request-and-record-returned-pOutFlags-only-after-success-"
            "for-every-filesystem-writer-profile"
        ),
    ),
    (
        (
            "transaction",
            "connection_lifecycle",
            "vfs_binding",
            "filesystem_source",
            "required_observation_capability",
            "required_operations",
        ),
        [
            "retain-authenticated-parent-namespace-capability",
            "observe-leaf-presence-kind-and-stable-identity-without-blocking",
            "hold-and-read-exact-object-bytes-size-and-sha256",
            "observe-exact-main-wal-shm-journal-entry-census",
            "detect-open-handle-versus-current-entry-replacement",
            "exclusive-create-zero-byte-main",
            "full-sync-created-main-object",
            "sync-created-main-parent-namespace",
            (
                "zero-initialize-local-out-flags-record-underlying-xOpen-input-role-"
                "request-and-on-success-returned-pOutFlags"
            ),
        ],
    ),
    (
        (
            "compatibility",
            "predecessor_v2",
            "read_path_strategy",
            "unreadable_sidecar_pair",
        ),
        {
            "code": "store.sqlite-failure",
            "field": "sqlite-sidecar-state",
            "detail": "unreadable-wal-shm-pair",
        },
    ),
    (
        (
            "transaction",
            "fresh_v3_initialization",
            "guards",
            "filesystem",
            "precreate_census",
            "sidecar_presence_precedence",
        ),
        [
            "no-main-plus-any-sidecar-is-orphan-and-rejected",
            (
                "only-after-receiptless-crash-profile-acceptance-and-matching-"
                "qualification-main-plus-one-readable-size-zero-wal-with-no-shm-or-"
                "journal-first-dispatches-exactly-through-receiptless_zero_wal_"
                "dispatch-as-FZ-pre-or-FZ-post"
            ),
            (
                "readable-size-zero-wal-with-main-and-no-shm-or-journal-that-does-"
                "not-pass-the-accepted-FZ-gate-is-rejected-before-private-SQLite-"
                "recovery-normalization-or-source-effect"
            ),
            (
                "only-after-receiptless-crash-profile-acceptance-and-matching-"
                "qualification-main-plus-journal-with-no-wal-or-shm-first-raw-"
                "classifies-exact-nonhot-prefix-hot-or-invalidated-family"
            ),
            "any-journal-with-main-is-journal-present-and-rejected-even-with-wal-or-shm",
            (
                "wal-and-shm-with-main-first-uses-the-ordinary-active-wal-existing-"
                "database-route-and-exact-logical-empty-with-no-format-authority-uses-"
                "the-active-wal-empty-preauthority-route"
            ),
            "shm-without-wal-is-incomplete-and-rejected",
            (
                "wal-without-shm-and-with-main-but-unreadable-wal-is-a-terminal-"
                "unreadable-wal-only-result"
            ),
            (
                "readable-nonzero-wal-without-shm-and-with-main-is-the-only-generic-"
                "preauthority-crash-candidate"
            ),
            "no-sidecar-with-main-is-the-ordinary-quiescent-or-clean-route",
        ],
    ),
    (
        (
            "transaction",
            "fresh_v3_initialization",
            "guards",
            "filesystem",
            "precreate_census",
            "receiptless_zero_wal_dispatch",
        ),
        {
            "raw_candidate": (
                "existing-nonzero-main-plus-one-readable-stable-size-zero-wal-with-"
                "wal-shm-journal-super-journal-and-other-sidecars-otherwise-absent"
            ),
            "activation": (
                "only-after-the-receiptless-crash-profile-is-accepted-and-the-exact-"
                "runtime-vfs-device-filesystem-S-P-page-count-and-recrash-vector-is-"
                "qualified"
            ),
            "exact_pre_route": (
                "complete-valid-wal-header-two-two-exact-empty-main-is-FZ-pre-and-"
                "may-only-seal-a-new-live-normalizer-source-anchor-with-the-existing-"
                "bound-zero-wal-as-coordination-object"
            ),
            "exact_post_route": (
                "complete-valid-rollback-header-one-one-exact-empty-current-main-is-"
                "FZ-post-and-may-only-run-the-typed-known-name-zero-wal-cleanup-then-"
                "enter-rollback_header_exact_empty_candidate-with-the-byte-exact-"
                "current-main-receipt"
            ),
            "rejected": (
                "every-other-main-zero-wal-topology-profile-or-qualification-state-"
                "is-unrecognized-preauthority-state-with-no-private-SQLite-recovery-"
                "normalizer-cleanup-or-source-effect"
            ),
            "cold_operation_history": (
                "never-infer-or-seal-the-prior-normalization-operation-edge"
            ),
            "precedence": (
                "before-preauthority_sidecar_candidate-and-before-any-zero-wal-main-"
                "alone-classification"
            ),
        },
    ),
    (
        (
            "transaction",
            "fresh_v3_initialization",
            "guards",
            "filesystem",
            "precreate_census",
            "preauthority_sidecar_candidate",
            "wal_participation_gate",
            "zero_byte",
        ),
        (
            "forbidden-on-this-generic-route-must-have-been-exhaustively-dispatched-"
            "by-precreate_census.receiptless_zero_wal_dispatch"
        ),
    ),
    (
        (
            "transaction",
            "fresh_v3_initialization",
            "guards",
            "filesystem",
            "precreate_census",
            "wal_header_sidecar_absent_exact_empty_candidate",
            "accepted_empty",
        ),
        (
            "seal-the-accepted-empty-normalization-source-anchor-and-enter-exactly-"
            "one-clean-normalizer-with-no-public-success-inference"
        ),
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_empty_normalization_receiptless_crash_profile_draft",
            "family_partition",
        ),
        [
            "F0-exact-pre-no-sidecar",
            (
                "FZ-exact-pre-or-complete-valid-rollback-exact-empty-current-main-"
                "plus-size-zero-wal"
            ),
            "FP-exact-pre-nonhot-journal-prefix",
            "FH-valid-hot-journal-with-exact-preimages-and-pre-or-post-main",
            (
                "FI-journal-preimages-derive-exact-pre-and-deterministic-post-plus-"
                "first-28-zero-invalidated-journal"
            ),
            "FO-complete-valid-rollback-exact-empty-current-main-no-sidecar",
        ],
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_empty_normalization_receiptless_crash_profile_draft",
            "cold_operation_history_inference",
        ),
        (
            "forbidden-FZ-post-FI-and-FO-prove-only-an-independent-current-rollback-"
            "header-exact-empty-anchor-while-F0-FZ-pre-FP-and-FH-may-only-start-a-"
            "new-live-receipted-normalizer"
        ),
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_empty_normalization_receiptless_crash_profile_draft",
            "disposable_fixture_capability",
        ),
        (
            "harness-minted-nonforgeable-private-root-capability-run-ID-and-internal-"
            "test-only-entrypoint-with-user-and-canonical-locators-rejected-and-no-"
            "production-API-route"
        ),
    ),
    (
        (
            "transaction",
            "fresh_v3_initialization",
            "guards",
            "filesystem",
            "precreate_census",
            "unreadable_wal_only",
        ),
        {
            "code": "store.sqlite-failure",
            "field": "sqlite-initialization-sidecar",
            "detail": "unreadable-wal-only",
        },
    ),
    (
        (
            "transaction",
            "recovery_model",
            "authority_state_projection",
            "committed_generation_maximum",
        ),
        {
            "empty_committed_set": (
                "canonical-tagged-none-iff-the-fully-validated-committed-row-count-"
                "is-zero"
            ),
            "nonempty_committed_set": (
                "canonical-tagged-some-with-the-exact-maximum-logical-u64-generation"
            ),
            "equation_only_origin": (
                "tagged-none-maps-to-u128-zero-only-inside-checked-allocation-and-"
                "reachability-equations-never-for-canonical-state-equality"
            ),
            "zero_distinction": (
                "tagged-some-u64-zero-for-a-nonempty-legacy-state-is-byte-distinct-"
                "from-tagged-none"
            ),
            "malformed": (
                "tagged-none-with-any-committed-row-or-tagged-some-with-zero-"
                "committed-rows-is-an-invalid-census"
            ),
        },
    ),
    (
        (
            "transaction",
            "recovery_model",
            "authorized_descendant_algebra",
            "state_equality",
        ),
        (
            "exact-length-framed-canonical-projection-byte-count-and-byte-for-byte-"
            "comparison-the-sha256-digest-is-only-an-acceleration-key-and-never-"
            "equality-authority"
        ),
    ),
    (
        (
            "transaction",
            "recovery_model",
            "authorized_descendant_algebra",
            "transition_proof",
        ),
        {
            "form": (
                "canonical-run-length-compressed-closed-form-witness-derived-"
                "independently-from-the-sealed-prestate-and-reopened-target"
            ),
            "validation": (
                "validate-logical-extension-format-split-and-final-physical-"
                "projection-then-solve-compact-run-counts-with-checked-arithmetic-"
                "without-replaying-each-edge"
            ),
            "candidate_acceptance": (
                "accept-the-target-iff-at-least-one-row-count-bounded-candidate-"
                "passes-all-arithmetic-rank-format-topology-and-byte-exact-"
                "projection-checks"
            ),
            "canonical_witness_selection": (
                "from-all-accepted-candidates-select-for-deterministic-reporting-"
                "only-the-typed-lexicographically-first-tuple-of-migration-"
                "population-tagged-none-before-zero-through-row-count-last-reset-"
                "kind-none-before-migration-before-current-format-compaction-last-"
                "reset-population-tagged-none-before-zero-through-row-count-"
                "canonical-publication-id-order-and-format-tagged-ascending-"
                "population-run-count-vector-never-use-this-choice-as-operation-"
                "result-authority"
            ),
            "operation_edge_predicates": (
                "compute-each-legacy-v2-compact-migration-and-v3-compact-presence-"
                "bit-as-existence-in-any-valid-representation-of-any-accepted-"
                "structural-candidate-never-from-only-its-canonical-residual-counts-"
                "or-the-canonical-reporting-witness-and-keep-the-exact-expected-"
                "candidate-direct-proof-separate"
            ),
            "added_rows": "target-minus-source-committed-rows-only",
            "diagnostic-row-add-delete-or-rewrite": "forbidden",
            "generation_bound": (
                "no-intermediate-authority-generation-may-exceed-target-maximum"
            ),
            "canonical_publish_order": (
                "for-each-final-compaction-or-migration-candidate-derive-pre-edge-"
                "cross-series-equal-sequence-order-from-the-target-contiguous-"
                "replacement-generation-order-and-verify-series-topology-and-known-"
                "source-generations-post-final-edge-rows-use-strictly-increasing-"
                "target-generation-publication-id-is-only-the-tertiary-tie-break-if-"
                "prior-generations-are-equal"
            ),
            "replacement_rank_invariant": (
                "every-whole-reset-is-a-stable-sort-by-publication-sequence-then-"
                "unique-prior-generation-so-equal-sequence-order-is-publish-chronology-"
                "and-must-match-known-source-order-plus-the-order-derived-from-target-"
                "reset-ranks"
            ),
            "final_compaction_candidates": (
                "enumerate-the-no-current-format-compaction-case-and-each-possible-"
                "designated-last-current-format-compaction-population-k-in-row-"
                "count-order-for-a-designated-k-subtract-exactly-one-k-edge-solve-"
                "only-the-single-pre-final-residual-and-append-k-once-then-require-"
                "the-contiguous-replacement-range-plus-strict-target-generation-"
                "publish-suffix-to-byte-match-target"
            ),
            "migration_candidates": (
                "for-a-v2-source-enumerate-one-migration-population-m-from-source-"
                "row-count-through-target-row-count-require-exactly-one-migration-"
                "for-a-v3-target-and-none-for-a-v2-target-migration-is-the-"
                "designated-last-reset-only-when-no-later-v3-compaction-exists-and-"
                "every-candidate-must-byte-match-its-contiguous-reset-range-plus-"
                "target-generation-publish-suffix"
            ),
            "candidate_cases": {
                "same_format_no_reset": (
                    "require-the-total-compaction-residual-to-be-zero-and-validate-"
                    "only-the-strict-generation-ordered-publish-extension"
                ),
                "same_format_final_compaction_k": (
                    "subtract-the-designated-k-edge-and-solve-the-single-pre-final-"
                    "residual-on-max-one-source-row-count-through-k"
                ),
                "v2_to_v3_migration_last_m": (
                    "subtract-the-single-migration-population-m-and-solve-all-"
                    "compaction-residual-on-max-one-source-row-count-through-m-with-"
                    "m-zero-admitting-only-zero-residual"
                ),
                "v2_to_v3_final_v3_compaction_m_k": (
                    "subtract-the-single-migration-population-m-and-designated-final-"
                    "k-edge-then-solve-the-single-pre-final-residual-on-max-one-"
                    "source-row-count-through-k-with-the-m-boundary-rule"
                ),
                "v3_to_v2": "forbidden",
            },
            "no_reset_target_shape": (
                "preserve-every-source-physical-generation-and-require-every-added-"
                "publication-to-use-the-next-strict-consecutive-generation-above-"
                "the-source-arithmetic-maximum-in-the-validated-canonical-"
                "topological-order"
            ),
            "reset_target_shape": (
                "every-positive-designated-last-reset-candidate-requires-all-target-"
                "committed-generations-to-be-one-contiguous-range-ending-at-the-"
                "target-maximum-the-first-k-target-rows-to-be-in-stable-sequence-"
                "then-prior-chronology-rank-and-contain-every-source-publication-id-"
                "and-the-remaining-rows-to-be-the-strict-consecutive-generation-and-"
                "series-topology-valid-publish-suffix"
            ),
            "zero_population_migration": (
                "format-only-edge-with-no-generation-allocation-and-no-compaction-"
                "denomination"
            ),
            "normalization_completeness": (
                "every-executable-path-maps-to-its-row-count-bounded-migration-"
                "population-and-last-reset-population-and-the-closed-form-counts-"
                "construct-an-observationally-equivalent-executable-path-because-"
                "same-population-resets-are-fixed-points-and-the-v2-v3-population-"
                "intervals-meet-at-the-commuting-migration-boundary"
            ),
            "format_boundary_commutation_validation": (
                "validate-byte-exactly-for-each-committed-row-that-moving-a-"
                "population-m-whole-reset-across-the-registered-migration-preserves-"
                "the-final-v3-logical-and-physical-projection-after-the-designated-"
                "last-reset"
            ),
            "canonical_byte_work": (
                "precompute-each-row-format-parse-logical-projection-and-reset-rank-"
                "once-so-candidate-and-edge-feature-enumeration-does-not-repeat-"
                "canonical-byte-work"
            ),
            "compact_run_equation": {
                "population_interval": (
                    "every-positive-integer-population-a-through-b-reachable-while-"
                    "the-canonical-publish-prefix-grows-zero-population-is-never-a-"
                    "compaction-denomination"
                ),
                "total_residual": (
                    "checked-target-arithmetic-maximum-minus-source-arithmetic-"
                    "maximum-minus-added-publish-count-minus-migration-population-"
                    "where-tagged-none-is-equation-only-zero-and-a-designated-final-"
                    "compaction-is-not-yet-subtracted"
                ),
                "migration_last_boundary": (
                    "when-migration-at-population-m-is-the-last-reset-solve-the-"
                    "entire-compaction-residual-on-max-one-source-row-count-through-"
                    "m-and-assign-every-run-to-v2-the-post-migration-compaction-"
                    "residual-is-exactly-zero"
                ),
                "final_current_format_boundary": (
                    "for-a-designated-final-compaction-at-population-k-subtract-one-"
                    "k-then-solve-the-single-pre-final-residual-on-max-one-source-"
                    "row-count-through-k-for-v2-to-v3-candidates-assign-populations-"
                    "less-than-m-to-v2-greater-than-m-to-v3-and-equal-to-m-to-v2-as-"
                    "the-exact-commuting-boundary-rule-for-same-format-candidates-"
                    "assign-every-run-to-that-format"
                ),
                "boundary_completeness": (
                    "the-v2-interval-ending-at-m-and-v3-interval-starting-at-m-have-"
                    "the-contiguous-union-max-one-source-row-count-through-k-and-"
                    "compactions-at-population-m-commute-with-the-single-migration-"
                    "reset-under-the-stable-rank-and-generation-offset-invariants"
                ),
                "feasibility": (
                    "residual-D-zero-allows-zero-runs-otherwise-q-is-ceil-D-div-b-"
                    "and-the-interval-is-reachable-iff-q-is-at-most-floor-D-div-a"
                ),
                "canonical_counts": (
                    "for-positive-D-let-q-equal-ceil-D-div-b-d-equal-D-div-q-and-r-"
                    "equal-D-mod-q-emit-count-q-minus-r-at-population-d-and-count-r-"
                    "at-population-d-plus-one-omitting-zero-counts-so-the-residual-"
                    "has-at-most-two-populations-and-a-separately-tagged-final-edge-"
                    "makes-at-most-three-never-iterate-q-times"
                ),
                "edge_feature_query": (
                    "for-each-requested-compact-format-enumerate-one-forced-legal-"
                    "population-p-in-that-format-segment-including-m-in-both-v2-and-"
                    "v3-segments-subtract-p-and-run-the-same-constant-time-full-union-"
                    "interval-feasibility-solver-on-the-remainder-or-the-bit-across-"
                    "p-and-structural-candidates-a-designated-final-edge-sets-its-"
                    "format-bit-directly"
                ),
                "designated_final_edge": (
                    "keep-the-designated-last-edge-separate-even-when-a-residual-run-"
                    "has-the-same-population-and-never-coalesce-it"
                ),
                "final_candidate_binding": (
                    "the-no-current-format-compaction-candidate-requires-zero-post-"
                    "last-reset-compaction-residual-a-designated-final-population-k-"
                    "requires-residual-at-least-k-and-appends-that-k-edge-exactly-"
                    "once-after-the-closed-form-pre-final-schedule"
                ),
                "verification": (
                    "checked-u128-sum-of-population-times-run-count-must-equal-the-"
                    "selected-residual-D-and-each-count-schedule-is-applied-"
                    "symbolically-in-ascending-population-order"
                ),
                "symbolic_rank_update": (
                    "for-each-nonzero-population-count-apply-one-full-sequence-prior-"
                    "generation-publication-id-rank-transform-then-add-count-minus-one-"
                    "times-population-to-the-maximum-without-repeating-the-rank-"
                    "transform-because-no-publish-interleaves-inside-the-run-and-the-"
                    "order-is-a-fixed-point"
                ),
            },
            "compact_edge_presence": (
                "per-format-existential-over-all-valid-representations-of-all-"
                "accepted-structural-candidates-using-the-forced-population-query-"
                "or-a-designated-final-edge-never-only-the-canonical-count-vector"
            ),
            "work_bound": (
                "at-most-row-count-plus-one-cubed-checked-structural-candidate-"
                "forced-edge-and-arithmetic-steps-plus-one-precomputed-streaming-"
                "pass-over-canonical-authority-bytes"
            ),
            "storage_bound": (
                "linear-in-row-count-plus-the-already-required-eager-canonical-"
                "authority-state"
            ),
            "generation-distance-loop-or-uncompressed-edge-replay": "forbidden",
            "termination": (
                "row-count-bounded-migration-and-final-reset-candidate-enumeration-"
                "plus-one-closed-form-residual-per-candidate-the-single-zero-row-"
                "migration-format-edge-may-occur-once-and-no-generation-distance-"
                "segment-split-is-enumerated"
            ),
        },
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "sealed_receipt_profiles",
            "fresh_initialization",
        ),
        [
            "canonical-locator",
            "exact-pinned-vfs-identity",
            "preinit-absent-or-exact-empty-anchor",
            "actual-target-main-open-file-instance-identity-and-directory-entry-binding",
            "pre-arm-raw-main-size-and-digest-and-sidecar-census",
            "deterministic-expected-empty-v3-projection",
            "initialization-id",
        ],
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "sealed_receipt_profiles",
            "fresh_empty_anchor_is_not_post_format_authority_state",
        ),
        True,
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "post_close_sidecar_decision_order",
        ),
        [
            "absent-unreadable-or-replaced-main",
            (
                "namespace-observation-io-or-any-nonregular-or-nonequivalent-main-wal-"
                "shm-journal-object"
            ),
            "unstable-main-or-directory-or-sidecar-presence-census",
            "any-present-journal",
            "stable-present-but-unreadable-wal-or-required-shm",
            "readable-wal-plus-readable-shm",
            "readable-wal-without-shm",
            "shm-without-wal",
            "no-sidecars",
        ],
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "routes",
        ),
        {
            "absent-unreadable-main": "reclassification-unavailable",
            "replaced-main-or-directory-binding": "main-identity-changed",
            "namespace-observation-io-or-object-kind-failure": (
                "reclassification-unavailable"
            ),
            "unstable-census": "reclassification-unavailable",
            "any-journal": "unsupported-terminal-sidecar-state",
            "stable-unreadable-wal-or-required-shm": (
                "reclassification-unavailable"
            ),
            "wal-and-shm": (
                "ordinary-active-wal-one-explicit-read-transaction-and-held-lock-route"
            ),
            "wal-only": (
                "stable-held-main-plus-wal-private-copy-and-private-read-write-no-"
                "create-recovery-route"
            ),
            "shm-only": "unsupported-terminal-sidecar-state",
            "no-sidecars": "ordinary-held-main-quiescent-private-copy-route",
        },
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "backend_scope",
        ),
        (
            "filesystem-only-ephemeral-memory-uncertainty-finalizes-and-attempts-"
            "exactly-one-close-v2-close-ok-discards-close-non-ok-quarantines-without-"
            "a-filesystem-receipt-reopen-or-reclassifier"
        ),
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "totality",
        ),
        (
            "every-main-wal-shm-journal-namespace-observation-object-kind-presence-"
            "readability-and-drift-combination-including-stable-unreadable-sidecars-"
            "maps-once-in-the-listed-order"
        ),
    ),
    (
        ("transaction", "recovery_model", "terminal_result_precedence"),
        {
            "result_authority": (
                "select-the-exact-operation-and-phase-block-before-mapping-the-"
                "terminal-classification-to-a-public-result"
            ),
            "generic_terminal_class_is_not_a_public_result": True,
            "close-not-confirmed-or-reclassifier-unavailable": (
                "apply-the-selected-operation-phase-opaque-result-and-unsafe-state-"
                "effect"
            ),
            "main-identity-changed-or-fully-valid-non-descendant": (
                "apply-the-selected-operation-phase-non-descendant-result-and-unsafe-"
                "state-effect"
            ),
            "acquired-invalid-census": (
                "apply-the-selected-operation-phase-invalid-census-result-and-unsafe-"
                "state-effect"
            ),
            "acquired-mixed-format": (
                "apply-the-selected-operation-phase-mixed-or-ambiguous-result-and-"
                "unsafe-state-effect"
            ),
            "unsupported-terminal-sidecar-state": (
                "apply-the-selected-operation-phase-opaque-result-and-unsafe-state-"
                "effect"
            ),
            "acquired-authorized-state": (
                "apply-the-selected-operation-phase-authorized-state-rule"
            ),
            "initialization": {
                "journal-transition-or-precommit-uncertain": (
                    "apply-fresh_v3_initialization-journal-transition-or-precommit-"
                    "phase-exactly"
                ),
                "commit-outcome-unknown": (
                    "apply-fresh_v3_initialization.commit_outcome_unknown-exactly"
                ),
                "successful-handoff": (
                    "require-confirmed-close-before-reopen-or-apply-the-initialization-"
                    "recovery-opaque-quarantine-rule"
                ),
            },
            "accepted-empty-normalization": {
                "pre-persistent-effect-failure": (
                    "after-exactly-one-confirmed-close-return-the-exact-trigger-and-"
                    "no-store-whether-pending-source-anchor-or-coordination-sequence-"
                    "two-receipts-exist-close-non-ok-applies-the-opaque-quarantine-"
                    "rule"
                ),
                "normalization-transition-or-close-uncertain": (
                    "apply-accepted_empty_original_normalization.failure_totality-"
                    "from-the-latest-effect-and-receipt-stage-and-return-no-store"
                ),
                "authorized-post-state-with-operation-edge": (
                    "internal-continue-fresh-initialization-with-no-store-yet"
                ),
                "authorized-pre-state-or-authorized-post-state-without-operation-edge": (
                    "return-store.sqlite-failure-sqlite-initialization-recovery-"
                    "opaque-and-no-store"
                ),
            },
            "publish": {
                "precommit-failure": (
                    "apply-publish.precommit_failure-exactly-original-trigger-only-"
                    "for-authorized-installed-state-otherwise-database-opaque-and-"
                    "poison"
                ),
                "commit-outcome-unknown": (
                    "apply-publish.commit_outcome_unknown-exactly-always-store.sqlite-"
                    "failure-database-opaque-with-authorized-state-install-or-unsafe-"
                    "state-poison"
                ),
            },
            "migration": {
                "locked-before-first-write-observed-reachable-v3": (
                    "rollback-close-reclassify-install-v3-and-return-success"
                ),
                "precommit-failure-v2-reachable-with-no-migration-edge": (
                    "return-exact-original-triggering-result-after-authorized-"
                    "reclassification"
                ),
                "precommit-failure-v3-reachable-with-exactly-one-migration-edge": (
                    "recovered-success-and-install-v3-as-idempotent-compact-postcondition"
                ),
                "commit-outcome-unknown-v2-reachable-with-no-migration-edge": (
                    "store.sqlite-failure-database-opaque-and-install-v2"
                ),
                "commit-outcome-unknown-v3-reachable-with-exactly-one-migration-edge": (
                    "recovered-success-and-install-v3"
                ),
            },
            "compaction": {
                "zero-pre-anchor-filesystem": (
                    "validate-under-begin-immediate-perform-no-write-and-no-commit-"
                    "rollback-finalize-confirm-close-and-return-success-after-"
                    "authorized-reclassification"
                ),
                "zero-pre-anchor-ephemeral-memory": (
                    "validate-under-begin-immediate-perform-no-write-and-no-commit-"
                    "successfully-rollback-finalize-retain-the-healthy-sole-connection-"
                    "without-close-or-reclassification-and-return-success"
                ),
                "nonzero-precommit-failure-proof-containing-v3-compact-edge": (
                    "recovered-success-as-idempotent-compact-postcondition"
                ),
                "nonzero-precommit-failure-proof-with-no-v3-compact-edge": (
                    "exact-original-triggering-result-after-authorized-reclassification"
                ),
                "commit-outcome-unknown-proof-containing-v3-compact-edge": (
                    "recovered-success"
                ),
                "commit-outcome-unknown-proof-with-no-v3-compact-edge": (
                    "store.sqlite-failure-database-opaque"
                ),
            },
        },
    ),
    (
        (
            "transaction",
            "fresh_v3_initialization",
            "guards",
            "filesystem",
            "journal_transition_atomicity",
            "pre_journal_receipt",
        ),
        {
            "capture": (
                "at-the-arming-point-before-enabling-the-first-header-journal-wal-shm-"
                "or-file-control-effect"
            ),
            "fields": [
                "canonical-locator-and-bound-vfs",
                "preinit-absent-or-exact-empty-anchor",
                "actual-target-main-open-file-instance-and-directory-entry-binding",
                "exact-empty-logical-projection",
                "pre-arm-raw-main-size-and-sha256",
                "exact-pre-arm-main-wal-shm-journal-census",
                "initialization-id",
                "deterministic-expected-empty-v3-projection",
            ],
            "authority": (
                "transaction.recovery_model.terminal_reclassification."
                "sealed_receipt_profiles.fresh_initialization"
            ),
        },
    ),
    (
        (
            "transaction",
            "fresh_v3_initialization",
            "guards",
            "filesystem",
            "journal_transition_atomicity",
            "failure_effects",
        ),
        (
            "before-arming-zero-persistent-effect-after-arming-no-schema-metadata-"
            "marker-semantic-or-diagnostic-authority-write-is-attempted-but-main-wal-"
            "header-and-wal-shm-or-journal-residue-may-exist"
        ),
    ),
    (
        (
            "transaction",
            "fresh_v3_initialization",
            "guards",
            "filesystem",
            "journal_transition_atomicity",
            "failure_after_arming",
        ),
        {
            "cleanup": (
                "finalize-all-statements-attempt-one-rollback-only-if-a-transaction-"
                "may-remain-then-attempt-exactly-one-sqlite3-close-v2"
            ),
            "close_ok": (
                "required-before-running-the-total-reclassifier-from-the-pre-journal-"
                "receipt"
            ),
            "close_non_ok_or_unknown": (
                "quarantine-the-connection-and-runtime-vfs-pins-do-not-reopen-or-"
                "unregister-return-store.sqlite-failure-sqlite-initialization-"
                "recovery-opaque-and-return-no-store-instance"
            ),
            "exact_same-identity-logical-empty": (
                "return-store.sqlite-failure-sqlite-initialization-recovery-opaque-"
                "with-no-store-instance"
            ),
            "exact_expected-or-authorized-current-v3": (
                "install-the-independently-validated-current-v3-state-and-return-"
                "recovered-initialization-success"
            ),
            "valid_non_descendant": (
                "return-store.sqlite-failure-sqlite-initialization-recovery-"
                "concurrent-authority-change-with-no-store-instance"
            ),
            "invalid_or_mixed": (
                "return-store.corrupt-sqlite-initialization-recovery-partial-or-"
                "mixed-authority-with-no-store-instance"
            ),
            "observation_or-reclassifier-unavailable": (
                "return-store.sqlite-failure-sqlite-initialization-recovery-opaque-"
                "with-no-store-instance"
            ),
            "source_recovery_checkpoint_sidecar_cleanup_retry-or-second-snapshot": (
                "forbidden"
            ),
        },
    ),
    (
        (
            "transaction",
            "fresh_v3_initialization",
            "precommit_failure",
            "ephemeral_memory_cleanup_or_rollback_uncertain",
        ),
        (
            "finalize-and-attempt-exactly-one-close-v2-close-ok-discards-the-sole-"
            "database-close-non-ok-quarantines-the-connection-and-runtime-vfs-pins-"
            "return-store.sqlite-failure-sqlite-initialization-recovery-opaque-with-"
            "no-store-instance-and-no-filesystem-receipt-reopen-or-reclassifier"
        ),
    ),
    (
        (
            "transaction",
            "fresh_v3_initialization",
            "commit_outcome_unknown",
            "ephemeral_memory",
        ),
        (
            "finalize-and-attempt-exactly-one-close-v2-close-ok-discards-the-sole-"
            "database-close-non-ok-quarantines-the-connection-and-runtime-vfs-pins-"
            "return-store.sqlite-failure-database-opaque-with-no-store-instance-and-"
            "no-filesystem-receipt-reopen-or-reclassifier"
        ),
    ),
    (
        (
            "transaction",
            "publish",
            "commit_outcome_unknown",
            "classifier",
            "same_logical_candidate_with_different_authorized_physical_projection",
        ),
        (
            "one-complete-valid-row-with-the-same-publication-id-and-byte-exact-"
            "immutable-logical-series-snapshot-sequence-parent-state-payload-"
            "projection-whose-different-generation-row-chunks-and-head-position-are-"
            "proved-by-the-authorized-descendant-normal-form-including-concurrent-"
            "publish-or-later-compaction"
        ),
    ),
    (
        (
            "transaction",
            "publish",
            "commit_outcome_unknown",
            "result_precedence",
        ),
        (
            "always-return-the-original-database-opaque-because-the-durable-"
            "publication-outcome-remains-unknown-even-if-reopen-observes-the-candidate"
        ),
    ),
    (
        (
            "transaction",
            "publish",
            "commit_outcome_unknown",
            "ephemeral_memory",
        ),
        (
            "finalize-and-attempt-exactly-one-close-v2-close-ok-discards-the-sole-"
            "connection-close-non-ok-quarantines-it-and-runtime-vfs-pins-poison-store-"
            "and-return-the-same-database-opaque-result-with-no-filesystem-receipt-"
            "reopen-or-terminal-reclassifier"
        ),
    ),
    (
        ("compaction", "zero_committed_authority_operation"),
        {
            "precondition": (
                "begin-immediate-full-mutation-census-proves-zero-fully-validated-"
                "committed-publications"
            ),
            "common_action": (
                "perform-no-schema-metadata-row-chunk-head-or-counter-write-and-do-"
                "not-call-commit"
            ),
            "filesystem_success": (
                "rollback-finalize-confirm-close-run-total-terminal-reclassification-"
                "and-only-an-exact-or-authorized-v3-descendant-installs-the-"
                "independent-state-and-returns-compact-success"
            ),
            "ephemeral_memory_success": (
                "successful-rollback-and-finalization-plus-a-healthy-sole-connection-"
                "retain-that-connection-without-close-or-terminal-reclassification-"
                "and-return-compact-success"
            ),
            "filesystem_close_non_ok_or_reclassification_failure": (
                "quarantine-or-poison-and-return-store.sqlite-failure-compaction-"
                "recovery-opaque"
            ),
            "ephemeral_memory_rollback-finalize-or-health-failure": (
                "finalize-and-attempt-exactly-one-close-v2-close-ok-discards-the-sole-"
                "database-close-non-ok-quarantines-the-connection-and-runtime-vfs-"
                "pins-poison-store-and-return-store.sqlite-failure-compaction-"
                "recovery-opaque-with-no-receipt-reopen-or-reclassifier"
            ),
            "ordinary_commit_outcome_unknown_branch": "unreachable",
        },
    ),
    (
        (
            "compaction",
            "commit_outcome_unknown",
            "recovery_receipt",
            "fields",
        ),
        [
            "canonical-locator-and-vfs",
            "anchored-main-open-object-identity-and-directory-entry-binding",
            (
                "exact-length-framed-pre-v3-authority-state-projection-bytes-and-"
                "digest"
            ),
            (
                "exact-length-framed-locked-census-and-deterministic-expected-v3-"
                "compaction-projection-bytes-and-digest"
            ),
            "candidate-compaction-id",
        ],
    ),
    (
        (
            "compaction",
            "commit_outcome_unknown",
            "exact_descendant_classifier",
            "expected_or_later_compacted",
        ),
        (
            "exact-receipt-expected-candidate-projection-is-sufficient-even-when-the-"
            "open-time-pre-anchor-was-empty-otherwise-the-authorized-normal-form-from-"
            "the-pre-anchor-must-contain-a-nonempty-whole-authority-compaction-edge-"
            "and-causal-authorship-is-not-required"
        ),
    ),
    (
        (
            "compaction",
            "commit_outcome_unknown",
            "exact_descendant_classifier",
            "observable_compaction",
        ),
        (
            "exact-expected-candidate-projection-or-at-least-one-positive-population-"
            "v3-compact-run-count-over-the-receipt-locked-census-the-witness-need-not-"
            "contain-an-open-time-pre-anchor-row"
        ),
    ),
    (
        (
            "compaction",
            "failure",
            "rollback_or_close_uncertain",
            "ephemeral_memory_rollback-finalize-or-connection-health-uncertain",
        ),
        (
            "finalize-and-attempt-exactly-one-close-v2-close-ok-discards-the-sole-"
            "connection-close-non-ok-quarantines-it-and-runtime-vfs-pins-with-no-"
            "filesystem-receipt-reopen-or-terminal-reclassifier-return-store.sqlite-"
            "failure-compaction-recovery-opaque-and-poison-the-store"
        ),
    ),
    (
        ("compaction", "commit_outcome_unknown", "ephemeral_memory"),
        (
            "for-a-nonzero-write-attempt-or-any-commit-rollback-finalize-or-connection-"
            "health-uncertainty-discard-connection-poison-store-and-return-store."
            "sqlite-failure-compaction-recovery-opaque-the-zero-authority-healthy-"
            "rollback-path-is-the-explicit-no-close-exception"
        ),
    ),
    (
        ("runtime", "capability_preflight", "pre_arm_synchronous", "accepted_empty_normalization_stage"),
        (
            "after-version-limit-locator-main-identity-source-anchor-and-exact-"
            "empty-gates-install-the-pending-coordination-arm-while-effects-remain-"
            "denied-then-set-and-query-connection-local-full-the-first-underlying-"
            "exclusive-xLock-callback-rechecks-and-arms-coordination-before-the-"
            "exact-zero-byte-wal-open-and-no-other-persistent-effect-is-permitted-"
            "before-the-later-full-arm"
        ),
    ),
    (
        (
            "runtime",
            "capability_preflight",
            "pre_arm_synchronous_failure_effects",
            "accepted_empty_normalization_with_or_without_coordination_effect",
        ),
        (
            "close-and-use-accepted_empty_original_normalization.failure_totality-"
            "selecting-pre-effect-or-after-coordination-from-the-latest-stage-evidence"
        ),
    ),
    (
        (
            "runtime",
            "capability_preflight",
            "pre_arm_synchronous_failure_effects",
            "coordination_effect_limit",
        ),
        (
            "wal-only-recovery-stage-new-shm-is-coordination-only-never-authority-"
            "and-no-main-wal-journal-or-store-authority-write-is-permitted-before-"
            "final-arm"
        ),
    ),
    (
        (
            "runtime",
            "capability_preflight",
            "pre_arm_synchronous_failure_effects",
            "accepted_empty_normalization_coordination_effect_limit",
        ),
        (
            "exact-zero-byte-wal-create-or-open-only-under-accepted-empty-"
            "normalization-coordination-with-all-other-persistent-effects-denied-"
            "before-full-arm"
        ),
    ),
    (
        ("transaction", "connection_lifecycle", "effect_gate_stages"),
        ACCEPTED_EMPTY_EFFECT_GATE_STAGES,
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "scope",
        ),
        [
            "initialization-recovery",
            "accepted-empty-normalization",
            "publish",
            "migration",
            "compaction",
        ],
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "sealed_receipt_profiles",
            "accepted_empty_normalization_source_anchor",
        ),
        ACCEPTED_EMPTY_NORMALIZATION_SOURCE_ANCHOR_RECEIPT,
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "sealed_receipt_profiles",
            "accepted_empty_normalization",
        ),
        ACCEPTED_EMPTY_NORMALIZATION_RECEIPT,
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "sealed_receipt_profiles",
            "accepted_empty_normalization_completed_edge",
        ),
        ACCEPTED_EMPTY_NORMALIZATION_COMPLETED_EDGE_RECEIPT,
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_empty_normalization_source_anchor",
        ),
        (
            "candidate-fields-computed-while-effects-are-denied-before-installing-"
            "the-pending-coordination-request-and-immutable-receipt-sealed-inside-"
            "the-first-exclusive-callback-before-coordination-publication-or-any-"
            "persistent-effect"
        ),
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_empty_normalization_source_anchor_profile",
        ),
        (
            "terminal_reclassification.sealed_receipt_profiles."
            "accepted_empty_normalization_source_anchor"
        ),
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_empty_normalization_source_anchor_seal",
        ),
        (
            "pending-coordination-request-may-be-installed-while-effects-remain-"
            "denied-then-after-first-successful-underlying-exclusive-xLock-and-"
            "local-zero-initialized-HAS_MOVED-exact-SQLITE_OK-output-zero-source-"
            "recheck-seal-the-immutable-source-anchor-immediately-before-"
            "publishing-coordination-sequence-two-and-before-zero-wal-or-other-"
            "persistent-effect"
        ),
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_empty_normalization_receipt_seal",
        ),
        (
            "in-the-later-repeated-second-pending-full-arm-exclusive-xLock-callback-"
            "after-prior-exact-coordination-sequence-two-receipt-and-exact-zero-wal-"
            "observation-and-before-full-arm-or-header-effect"
        ),
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_empty_normalization_receipt_extension",
        ),
        (
            "exact-source-anchor-receipt-plus-exact-coordination-sequence-two-"
            "receipt-plus-the-later-repeated-exclusive-lock-recheck-plus-"
            "coordination-zero-wal-observation-plus-planned-normalization-candidate-"
            "id"
        ),
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_empty_normalization_candidate_identity",
        ),
        (
            "planned-normalization-candidate-id-in-the-pre-effect-full-receipt-"
            "never-proves-transition-close-poststate-physical-edge-or-success"
        ),
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_empty_normalization_completed_edge_profile",
        ),
        (
            "terminal_reclassification.sealed_receipt_profiles."
            "accepted_empty_normalization_completed_edge"
        ),
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_empty_normalization_completed_edge_seal",
        ),
        (
            "only-after-the-exact-normalization-bounded-effect-transcript-and-"
            "coordination-wal-delete-journal-creation-and-terminal-journal-delete-"
            "retained-authenticated-parent-fsync-receipts-the-single-delete-"
            "normalization-transition-finalize-exactly-one-confirmed-close-and-post-"
            "close-total-reclassification-bind-the-planned-candidate-source-"
            "coordination-and-full-arm-receipts-to-the-exact-same-identity-poststate"
        ),
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_empty_normalization_operation_identity",
        ),
        (
            "accepted-empty-normalization-operation-plus-journal-normalization-"
            "boundary-plus-normalization-transition-phase-and-generic-connection-"
            "close-distinct-from-fresh-initialization"
        ),
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_empty_normalization_success",
        ),
        (
            "intermediate-only-no-store-and-only-a-sealed-same-identity-normalized-"
            "empty-operation-edge-continues-into-ordinary-fresh-initialization"
        ),
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_empty_normalization_public_success",
        ),
        "forbidden-until-ordinary-fresh-initialization-independently-completes",
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_empty_normalization_physical_edge",
        ),
        ACCEPTED_EMPTY_NORMALIZATION_PHYSICAL_EDGE,
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "typed_control_surface",
            "accepted_empty_normalization",
        ),
        ACCEPTED_EMPTY_TYPED_CONTROL,
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "main_identity",
            "capture",
            "accepted_empty_normalization",
        ),
        {
            "source_anchor": (
                "capture-from-the-actual-normalizer-xOpen-main-handle-and-same-"
                "directory-entry-then-verify-and-seal-inside-the-first-underlying-"
                "exclusive-xLock-callback-before-coordination-publication"
            ),
            "pre_effect_full": (
                "recheck-that-exact-main-file-instance-and-directory-entry-inside-"
                "the-later-repeated-second-exclusive-xLock-callback-before-sealing-"
                "the-pre-effect-full-receipt-and-publishing-full-arm"
            ),
        },
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_reclassification",
            "accepted_routes_require",
            "operation_admission",
            "accepted-empty-normalization",
        ),
        ["exact-logical-empty-preauthority"],
    ),
    (
        (
            "transaction",
            "recovery_model",
            "terminal_result_precedence",
            "accepted-empty-normalization",
        ),
        {
            "pre-persistent-effect-failure": (
                "after-exactly-one-confirmed-close-return-the-exact-trigger-and-no-"
                "store-whether-pending-source-anchor-or-coordination-sequence-two-"
                "receipts-exist-close-non-ok-applies-the-opaque-quarantine-rule"
            ),
            "normalization-transition-or-close-uncertain": (
                "apply-accepted_empty_original_normalization.failure_totality-from-"
                "the-latest-effect-and-receipt-stage-and-return-no-store"
            ),
            "authorized-post-state-with-operation-edge": (
                "internal-continue-fresh-initialization-with-no-store-yet"
            ),
            "authorized-pre-state-or-authorized-post-state-without-operation-edge": (
                "return-store.sqlite-failure-sqlite-initialization-recovery-opaque-"
                "and-no-store"
            ),
        },
    ),
    (
        (
            "transaction",
            "fresh_v3_initialization",
            "guards",
            "filesystem",
            "precreate_census",
            "rollback_header_exact_empty_candidate",
        ),
        NORMALIZED_EMPTY_INTERRUPTED_HANDOFF,
    ),
    (
        (
            "transaction",
            "fresh_v3_initialization",
            "guards",
            "filesystem",
            "precreate_census",
            "preauthority_sidecar_candidate",
            "accepted_empty_original_normalization",
        ),
        ACCEPTED_EMPTY_NORMALIZATION,
    ),
    (
        (
            "transaction",
            "fresh_v3_initialization",
            "guards",
            "filesystem",
            "lease",
        ),
        (
            "after-the-nonexistent-bootstrap-exception-sqlite-exclusive-lock-plus-"
            "main-file-identity-check-before-every-persistent-effect"
        ),
    ),
)


class SQLiteStoreContractError(ValueError):
    """Structured failure raised by the SQLite contract checker."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code


def fail(code: str, message: str) -> NoReturn:
    raise SQLiteStoreContractError(code, message)


def _at_path(value: dict[str, Any], path: tuple[str, ...]) -> Any:
    current: Any = value
    for component in path:
        if not isinstance(current, dict) or component not in current:
            fail(
                "sqlite.option-a-contract-invalid",
                f"required field is missing: {'.'.join(path)}",
            )
        current = current[component]
    return current


def option_a_projection(contract: dict[str, Any]) -> dict[str, Any]:
    """Return the standalone safety-critical Option A contract projection."""

    return {
        ".".join(path): _at_path(contract, path)
        for path, _expected in OPTION_A_REQUIREMENTS
    }


def _matches_exact_requirement(actual: Any, expected: Any) -> bool:
    """Match exact structures, with an explicit canonical-digest leaf escape."""

    if (
        isinstance(expected, dict)
        and set(expected) == {"__canonical_sha256__"}
        and isinstance(expected["__canonical_sha256__"], str)
    ):
        return document_digest(actual) == expected["__canonical_sha256__"]
    if isinstance(expected, dict):
        return (
            isinstance(actual, dict)
            and set(actual) == set(expected)
            and all(
                _matches_exact_requirement(actual[key], value)
                for key, value in expected.items()
            )
        )
    if isinstance(expected, list):
        return (
            isinstance(actual, list)
            and len(actual) == len(expected)
            and all(
                _matches_exact_requirement(actual_item, expected_item)
                for actual_item, expected_item in zip(actual, expected, strict=True)
            )
        )
    return actual == expected


def validate_option_a_contract(contract: dict[str, Any]) -> None:
    """Validate recovery/open semantics independently of the static digest."""

    actual = option_a_projection(contract)
    for path, expected in OPTION_A_REQUIREMENTS:
        label = ".".join(path)
        if not _matches_exact_requirement(actual[label], expected):
            fail(
                "sqlite.option-a-contract-invalid",
                f"critical Option A projection differs: {label}",
            )
    open_profiles = _at_path(
        contract,
        ("transaction", "connection_lifecycle", "sqlite_open_profiles"),
    )
    if not isinstance(open_profiles, dict) or (
        set(open_profiles) - {"common_non_ok_return_cleanup"}
        != set(SQLITE_OPEN_PROFILE_NAMES)
    ):
        fail(
            "sqlite.option-a-contract-invalid",
            "sqlite3_open_v2 profile census differs",
        )
    validate_option_a_vectors()


def authority_state_bytes_equal(left: bytes, right: bytes) -> bool:
    """Canonical bytes, rather than their acceleration digest, own equality."""

    if not isinstance(left, bytes) or not isinstance(right, bytes):
        fail(
            "sqlite.authority-state-invalid",
            "authority state equality requires exact byte strings",
        )
    return len(left) == len(right) and left == right


def _checked_nonnegative_integer(value: Any, label: str, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        fail("sqlite.normal-form-invalid", f"{label} is not an integer")
    if value < 0 or value > maximum:
        fail("sqlite.normal-form-invalid", f"{label} is out of range")
    return value


SQLITE_OPEN_PROFILE_NAMES = (
    "active_existing_probe",
    "quiescent_private_snapshot",
    "existing_current_v3_writer",
    "accepted_empty_normalization",
    "fresh_filesystem_after_raw_bootstrap",
    "wal_only_private_recovery",
    "wal_only_source_recovery_or_v2_compact",
    "scratch_or_ephemeral_memory",
)


def sqlite_open_non_ok_cleanup(
    profile: str,
    *,
    handle_present: bool,
    close_result: str | None,
) -> dict[str, Any]:
    """Model the common non-OK ``sqlite3_open_v2`` cleanup totality."""

    if profile not in SQLITE_OPEN_PROFILE_NAMES:
        fail("sqlite.open-cleanup-invalid", "unknown sqlite3_open_v2 profile")
    if not isinstance(handle_present, bool):
        fail("sqlite.open-cleanup-invalid", "handle presence is not boolean")
    if not handle_present:
        if close_result is not None:
            fail(
                "sqlite.open-cleanup-invalid",
                "a null handle cannot have a close result",
            )
        return {
            "close_attempts": 0,
            "quarantined": False,
            "retry": False,
            "reclassifier": False,
            "result": "selected-profile-open-error",
        }

    if close_result not in {"ok", "non-ok", "unknown"}:
        fail(
            "sqlite.open-cleanup-invalid",
            "a nonnull handle requires one exact close result",
        )
    return {
        "close_attempts": 1,
        "quarantined": close_result != "ok",
        "retry": False,
        "reclassifier": False,
        "result": "selected-profile-open-error",
    }


def readwrite_open_observation(
    *,
    input_flags: frozenset[str],
    local_out_flags_before_call: int,
    sqlite_result: str,
    returned_out_flags: frozenset[str] | None,
) -> str:
    """Check zero-init, input-role, and success-only output-flag authority."""

    if not isinstance(input_flags, frozenset):
        fail("sqlite.open-observation-invalid", "input flags are not a frozen set")
    if not {"SQLITE_OPEN_MAIN_DB", "SQLITE_OPEN_READWRITE"}.issubset(
        input_flags
    ):
        fail(
            "sqlite.open-observation-invalid",
            "the input role and read-write request are incomplete",
        )
    if (
        isinstance(local_out_flags_before_call, bool)
        or not isinstance(local_out_flags_before_call, int)
        or local_out_flags_before_call != 0
    ):
        fail(
            "sqlite.open-observation-invalid",
            "local pOutFlags storage was not exact integer zero before xOpen",
        )
    if sqlite_result == "non-ok":
        if returned_out_flags is not None:
            fail(
                "sqlite.open-observation-invalid",
                "non-OK xOpen output flags are not authority",
            )
        return "open-non-ok-no-returned-flags"
    if sqlite_result != "ok" or not isinstance(returned_out_flags, frozenset):
        fail(
            "sqlite.open-observation-invalid",
            "successful xOpen requires one returned flag set",
        )
    if "SQLITE_OPEN_READONLY" in returned_out_flags:
        return "store.sqlite-failure/open/read-write-required"
    return "read-write-proved"


def committed_generation_maximum(
    generations: tuple[int, ...],
) -> tuple[str] | tuple[str, int]:
    """Encode the committed maximum with a byte-distinct none/some tag."""

    if not isinstance(generations, tuple):
        fail("sqlite.generation-maximum-invalid", "generations are not a tuple")
    checked = tuple(
        _checked_nonnegative_integer(
            generation, "committed generation", (1 << 64) - 1
        )
        for generation in generations
    )
    if not checked:
        return ("none",)
    return ("some", max(checked))


def validate_committed_generation_maximum(
    tagged_maximum: tuple[Any, ...], *, committed_row_count: int
) -> int:
    """Validate the tag/count invariant and return its equation-only origin."""

    count = _checked_nonnegative_integer(
        committed_row_count, "committed row count", (1 << 64) - 1
    )
    if tagged_maximum == ("none",):
        if count != 0:
            fail(
                "sqlite.generation-maximum-invalid",
                "tagged none has committed rows",
            )
        return 0
    if (
        isinstance(tagged_maximum, tuple)
        and len(tagged_maximum) == 2
        and tagged_maximum[0] == "some"
    ):
        maximum = _checked_nonnegative_integer(
            tagged_maximum[1], "committed generation maximum", (1 << 64) - 1
        )
        if count == 0:
            fail(
                "sqlite.generation-maximum-invalid",
                "tagged some has no committed rows",
            )
        return maximum
    fail("sqlite.generation-maximum-invalid", "malformed generation maximum tag")


def compressed_compaction_schedule(
    delta: int,
    minimum_population: int,
    maximum_population: int,
    *,
    designated_final_population: int | None = None,
) -> tuple[tuple[tuple[int, int], ...], int | None]:
    """Solve one format segment without generation-distance iteration.

    The first tuple is the ascending run-length-compressed residual schedule.
    A designated final edge is returned separately so it cannot be accidentally
    coalesced into an earlier run of the same population.
    """

    u128_max = (1 << 128) - 1
    remaining = _checked_nonnegative_integer(delta, "delta", u128_max)
    lower = _checked_nonnegative_integer(
        minimum_population, "minimum population", u128_max
    )
    upper = _checked_nonnegative_integer(
        maximum_population, "maximum population", u128_max
    )
    if lower == 0 or lower > upper:
        fail(
            "sqlite.normal-form-invalid",
            "population interval must be positive and ordered",
        )

    final = designated_final_population
    if final is not None:
        final = _checked_nonnegative_integer(final, "final population", u128_max)
        if final < lower or final > upper or final > remaining:
            fail(
                "sqlite.normal-form-unreachable",
                "designated final population is outside the reachable segment",
            )
        remaining -= final

    if remaining == 0:
        return (), final

    # ceil(remaining / upper) without an overflowing addition.
    minimum_run_count = 1 + (remaining - 1) // upper
    maximum_run_count = remaining // lower
    if minimum_run_count > maximum_run_count:
        fail(
            "sqlite.normal-form-unreachable",
            "compaction delta has no population schedule",
        )

    run_count = minimum_run_count
    base_population, upper_population_count = divmod(remaining, run_count)
    lower_population_count = run_count - upper_population_count
    if base_population < lower or base_population > upper:
        fail(
            "sqlite.normal-form-unreachable",
            "quotient population lies outside the segment interval",
        )
    if upper_population_count and base_population == upper:
        fail(
            "sqlite.normal-form-unreachable",
            "remainder would exceed the maximum population",
        )

    counts: list[tuple[int, int]] = []
    if lower_population_count:
        counts.append((base_population, lower_population_count))
    if upper_population_count:
        counts.append((base_population + 1, upper_population_count))
    if len(counts) > 3:
        fail(
            "sqlite.normal-form-invalid",
            "canonical schedule exceeded the three-count bound",
        )
    if sum(population * count for population, count in counts) != remaining:
        fail(
            "sqlite.normal-form-invalid",
            "canonical schedule does not reconstruct the residual delta",
        )
    return tuple(counts), final


def _reachable_compaction_schedule(
    delta: int, lower: int, upper: int
) -> tuple[tuple[int, int], ...] | None:
    """Return one closed-form residual schedule, or ``None`` if unreachable."""

    if delta == 0:
        return ()
    if lower <= 0 or upper < lower:
        return None
    try:
        schedule, final = compressed_compaction_schedule(delta, lower, upper)
    except SQLiteStoreContractError as error:
        if error.code == "sqlite.normal-form-unreachable":
            return None
        raise
    if final is not None:
        fail(
            "sqlite.normal-form-invalid",
            "a residual schedule unexpectedly contained a final edge",
        )
    return schedule


def forced_population_reachable(
    delta: int, lower: int, upper: int, forced_population: int
) -> bool:
    """Ask existential edge presence by forcing one row-bounded population."""

    u128_max = (1 << 128) - 1
    checked_delta = _checked_nonnegative_integer(delta, "delta", u128_max)
    checked_lower = _checked_nonnegative_integer(lower, "lower population", u128_max)
    checked_upper = _checked_nonnegative_integer(upper, "upper population", u128_max)
    forced = _checked_nonnegative_integer(
        forced_population, "forced population", u128_max
    )
    if (
        checked_lower == 0
        or checked_upper < checked_lower
        or forced < checked_lower
        or forced > checked_upper
        or forced > checked_delta
    ):
        return False
    return (
        _reachable_compaction_schedule(
            checked_delta - forced, checked_lower, checked_upper
        )
        is not None
    )


def _candidate_sort_key(candidate: dict[str, Any]) -> tuple[Any, ...]:
    migration = candidate["migration_population"]
    last_population = candidate["last_reset_population"]
    kind_rank = {
        "none": 0,
        "migration": 1,
        "current-format-compaction": 2,
    }[candidate["last_reset_kind"]]
    format_rank = {"legacy-v2-compact": 0, "v3-compact": 1}
    return (
        (0, 0) if migration is None else (1, migration),
        kind_rank,
        (0, 0) if last_population is None else (1, last_population),
        candidate["publication_id_order"],
        tuple(
            (format_rank[format_name], population, count)
            for format_name, population, count in candidate[
                "format_tagged_residual_schedule"
            ]
        ),
    )


def scalar_authorized_descendant_summary(
    *,
    source_format: str,
    target_format: str,
    source_row_count: int,
    source_generation_maximum: tuple[Any, ...],
    target_row_count: int,
    target_generation_maximum: tuple[Any, ...],
) -> dict[str, Any]:
    """Execute the row-bounded scalar core of the structural candidate proof.

    Logical extension, rank, topology, and byte projection are authority gates in
    the YAML contract.  This independent vector oracle exercises the remaining
    candidate enumeration, compressed arithmetic, migration boundary, reporting
    witness, and existential per-format edge features.  Its loops are bounded
    only by row populations; generation distance is represented by run counts.
    """

    if source_format not in {"v2", "v3"} or target_format not in {"v2", "v3"}:
        fail("sqlite.normal-form-invalid", "unknown physical format")
    source_rows = _checked_nonnegative_integer(
        source_row_count, "source row count", (1 << 64) - 1
    )
    target_rows = _checked_nonnegative_integer(
        target_row_count, "target row count", (1 << 64) - 1
    )
    source_maximum = validate_committed_generation_maximum(
        source_generation_maximum, committed_row_count=source_rows
    )
    target_maximum = validate_committed_generation_maximum(
        target_generation_maximum, committed_row_count=target_rows
    )
    if target_rows < source_rows:
        return {
            "accepted": False,
            "accepted_candidates": (),
            "canonical_reporting_witness": None,
            "edge_presence": {
                "legacy-v2-compact": False,
                "migrate-v2-v3": False,
                "v3-compact": False,
            },
        }
    if source_format == "v3" and target_format == "v2":
        return {
            "accepted": False,
            "accepted_candidates": (),
            "canonical_reporting_witness": None,
            "edge_presence": {
                "legacy-v2-compact": False,
                "migrate-v2-v3": False,
                "v3-compact": False,
            },
        }

    publish_count = target_rows - source_rows
    candidates: list[dict[str, Any]] = []
    lower = max(1, source_rows)

    def append_candidate(
        *,
        case: str,
        migration_population: int | None,
        last_reset_kind: str,
        last_reset_population: int | None,
        residual_schedule: tuple[tuple[int, int], ...],
        format_tagged_schedule: tuple[tuple[str, int, int], ...],
        designated_final_edge: tuple[str, int] | None,
        forced_populations: dict[str, tuple[int, ...]],
    ) -> None:
        edge_presence = {
            "legacy-v2-compact": bool(
                forced_populations.get("legacy-v2-compact", ())
            ),
            "migrate-v2-v3": migration_population is not None,
            "v3-compact": bool(forced_populations.get("v3-compact", ())),
        }
        if designated_final_edge is not None:
            edge_presence[designated_final_edge[0]] = True
        candidates.append(
            {
                "case": case,
                "migration_population": migration_population,
                "last_reset_kind": last_reset_kind,
                "last_reset_population": last_reset_population,
                "publication_id_order": (),
                "residual_schedule": residual_schedule,
                "format_tagged_residual_schedule": format_tagged_schedule,
                "designated_final_edge": designated_final_edge,
                "forced_edge_populations": forced_populations,
                "edge_presence": edge_presence,
            }
        )

    if source_format == target_format:
        residual = target_maximum - source_maximum - publish_count
        if residual >= 0:
            if residual == 0:
                append_candidate(
                    case="same_format_no_reset",
                    migration_population=None,
                    last_reset_kind="none",
                    last_reset_population=None,
                    residual_schedule=(),
                    format_tagged_schedule=(),
                    designated_final_edge=None,
                    forced_populations={},
                )
            compact_format = (
                "legacy-v2-compact" if source_format == "v2" else "v3-compact"
            )
            for final_population in range(lower, target_rows + 1):
                if residual < final_population:
                    continue
                pre_final_delta = residual - final_population
                schedule = _reachable_compaction_schedule(
                    pre_final_delta, lower, final_population
                )
                if schedule is None:
                    continue
                forced = tuple(
                    population
                    for population in range(lower, final_population + 1)
                    if forced_population_reachable(
                        pre_final_delta, lower, final_population, population
                    )
                )
                append_candidate(
                    case="same_format_final_compaction_k",
                    migration_population=None,
                    last_reset_kind="current-format-compaction",
                    last_reset_population=final_population,
                    residual_schedule=schedule,
                    format_tagged_schedule=tuple(
                        (compact_format, population, count)
                        for population, count in schedule
                    ),
                    designated_final_edge=(compact_format, final_population),
                    forced_populations={compact_format: forced},
                )
    elif source_format == "v2" and target_format == "v3":
        for migration_population in range(source_rows, target_rows + 1):
            total_residual = (
                target_maximum
                - source_maximum
                - publish_count
                - migration_population
            )
            if total_residual < 0:
                continue

            if migration_population == 0:
                migration_last_schedule = () if total_residual == 0 else None
            else:
                migration_last_schedule = _reachable_compaction_schedule(
                    total_residual, lower, migration_population
                )
            if migration_last_schedule is not None:
                forced_v2 = tuple(
                    population
                    for population in range(lower, migration_population + 1)
                    if forced_population_reachable(
                        total_residual,
                        lower,
                        migration_population,
                        population,
                    )
                )
                append_candidate(
                    case="v2_to_v3_migration_last_m",
                    migration_population=migration_population,
                    last_reset_kind="migration",
                    last_reset_population=migration_population,
                    residual_schedule=migration_last_schedule,
                    format_tagged_schedule=tuple(
                        ("legacy-v2-compact", population, count)
                        for population, count in migration_last_schedule
                    ),
                    designated_final_edge=None,
                    forced_populations={"legacy-v2-compact": forced_v2},
                )

            for final_population in range(
                max(1, migration_population), target_rows + 1
            ):
                if total_residual < final_population:
                    continue
                pre_final_delta = total_residual - final_population
                schedule = _reachable_compaction_schedule(
                    pre_final_delta, lower, final_population
                )
                if schedule is None:
                    continue
                legacy_upper = min(migration_population, final_population)
                forced_v2 = tuple(
                    population
                    for population in range(lower, legacy_upper + 1)
                    if forced_population_reachable(
                        pre_final_delta, lower, final_population, population
                    )
                )
                v3_lower = max(lower, migration_population)
                forced_v3 = tuple(
                    population
                    for population in range(v3_lower, final_population + 1)
                    if forced_population_reachable(
                        pre_final_delta, lower, final_population, population
                    )
                )
                append_candidate(
                    case="v2_to_v3_final_v3_compaction_m_k",
                    migration_population=migration_population,
                    last_reset_kind="current-format-compaction",
                    last_reset_population=final_population,
                    residual_schedule=schedule,
                    format_tagged_schedule=tuple(
                        (
                            "legacy-v2-compact"
                            if population <= migration_population
                            else "v3-compact",
                            population,
                            count,
                        )
                        for population, count in schedule
                    ),
                    designated_final_edge=("v3-compact", final_population),
                    forced_populations={
                        "legacy-v2-compact": forced_v2,
                        "v3-compact": forced_v3,
                    },
                )

    accepted = tuple(sorted(candidates, key=_candidate_sort_key))
    edge_presence = {
        edge: any(candidate["edge_presence"][edge] for candidate in accepted)
        for edge in (
            "legacy-v2-compact",
            "migrate-v2-v3",
            "v3-compact",
        )
    }
    return {
        "accepted": bool(accepted),
        "accepted_candidates": accepted,
        "canonical_reporting_witness": accepted[0] if accepted else None,
        "edge_presence": edge_presence,
    }


def canonical_replacement_order(rows: list[dict[str, Any]]) -> tuple[str, ...]:
    """Return the cross-series whole-authority replacement order."""

    checked: list[tuple[int, int, str]] = []
    for row in rows:
        try:
            sequence = row["sequence"]
            generation = row["generation"]
            publication_id = row["publication_id"]
        except (KeyError, TypeError) as error:
            fail("sqlite.normal-form-invalid", f"replacement row is incomplete: {error}")
        _checked_nonnegative_integer(sequence, "sequence", (1 << 64) - 1)
        _checked_nonnegative_integer(generation, "generation", (1 << 64) - 1)
        if not isinstance(publication_id, str) or not publication_id:
            fail(
                "sqlite.normal-form-invalid",
                "publication ID must be a nonempty string",
            )
        checked.append((sequence, generation, publication_id))
    if len({publication_id for _sequence, _generation, publication_id in checked}) != len(
        checked
    ):
        fail("sqlite.normal-form-invalid", "publication IDs are not unique")
    return tuple(row[2] for row in sorted(checked))


def format_reset_projection(operators: list[str]) -> dict[str, Any]:
    """Validate the exact legacy-v2/migration/current-v3 operator split."""

    phase = "v2"
    migration_count = 0
    last_reset: str | None = None
    v2_compactions = 0
    v3_compactions = 0
    for operator in operators:
        if operator == "migrate-v2-v3":
            if phase != "v2" or migration_count:
                fail("sqlite.normal-form-invalid", "migration edge is not unique")
            phase = "v3"
            migration_count = 1
            last_reset = operator
        elif operator == "legacy-v2-publish":
            if phase != "v2":
                fail("sqlite.normal-form-invalid", "v2 publish follows migration")
        elif operator == "legacy-v2-compact":
            if phase != "v2":
                fail("sqlite.normal-form-invalid", "v2 compact follows migration")
            v2_compactions += 1
            last_reset = operator
        elif operator == "v3-publish":
            if phase != "v3":
                fail("sqlite.normal-form-invalid", "v3 publish precedes migration")
        elif operator == "v3-compact":
            if phase != "v3":
                fail("sqlite.normal-form-invalid", "v3 compact precedes migration")
            v3_compactions += 1
            last_reset = operator
        else:
            fail("sqlite.normal-form-invalid", f"unknown transition operator: {operator}")
    if migration_count != 1:
        fail("sqlite.normal-form-invalid", "v3 target requires one migration edge")
    return {
        "migration_count": migration_count,
        "v2_compactions": v2_compactions,
        "v3_compactions": v3_compactions,
        "last_reset": last_reset,
    }


def compaction_recovery_verdict(
    *,
    open_anchor_population: int,
    locked_census_population: int,
    exact_expected_candidate: bool,
    compact_edge_populations: tuple[int, ...],
) -> str:
    """Classify the exact-candidate/locked-census compaction success proof."""

    maximum = (1 << 64) - 1
    opened = _checked_nonnegative_integer(
        open_anchor_population, "open anchor population", maximum
    )
    locked = _checked_nonnegative_integer(
        locked_census_population, "locked census population", maximum
    )
    if locked < opened:
        fail(
            "sqlite.normal-form-invalid",
            "locked census cannot delete open-anchor authority",
        )
    if not isinstance(exact_expected_candidate, bool):
        fail("sqlite.normal-form-invalid", "expected-candidate flag is not boolean")
    if exact_expected_candidate:
        return "recovered-success"
    for population in compact_edge_populations:
        checked = _checked_nonnegative_integer(
            population, "compact edge population", maximum
        )
        if checked > 0 and checked >= locked:
            return "recovered-success"
    return "store.sqlite-failure/database/opaque"


def decode_normalization_page_size(encoded: int) -> int:
    """Decode SQLite header bytes 16..17 for the normalization oracle."""

    if isinstance(encoded, bool) or not isinstance(encoded, int):
        fail("sqlite.normalization-vector-invalid", "page size is not an integer")
    if encoded == 1:
        return 65_536
    if encoded < 512 or encoded > 32_768 or encoded & (encoded - 1):
        fail("sqlite.normalization-vector-invalid", "invalid encoded page size")
    return encoded


def normalization_expected_post_main(
    pre_main: bytes,
    *,
    pinned_libversion_number: int,
) -> bytes:
    """Build the deterministic SQLite-owned page-one normalization projection."""

    if not isinstance(pre_main, bytes) or len(pre_main) < 100:
        fail("sqlite.normalization-vector-invalid", "main image is too short")
    page_size = decode_normalization_page_size(
        int.from_bytes(pre_main[16:18], "big")
    )
    if len(pre_main) == 0 or len(pre_main) % page_size:
        fail(
            "sqlite.normalization-vector-invalid",
            "main size is not a positive integral page multiple",
        )
    header_page_count = int.from_bytes(pre_main[28:32], "big")
    if header_page_count != len(pre_main) // page_size:
        fail(
            "sqlite.normalization-vector-invalid",
            "header page count does not match the whole main image",
        )
    runtime_version = _checked_nonnegative_integer(
        pinned_libversion_number,
        "pinned sqlite3_libversion_number",
        (1 << 32) - 1,
    )
    post_counter = (int.from_bytes(pre_main[24:28], "big") + 1) & 0xFFFF_FFFF
    post = bytearray(pre_main)
    post[18:20] = b"\x01\x01"
    post[24:28] = post_counter.to_bytes(4, "big")
    post[92:96] = post_counter.to_bytes(4, "big")
    post[96:100] = runtime_version.to_bytes(4, "big")
    return bytes(post)


def effective_sqlite_sector_size(
    raw_xsector_size: int,
    *,
    temporary_file: bool = False,
    powersafe_overwrite: bool = False,
) -> int:
    """Model the exact pinned pager's current sector-size selection."""

    if isinstance(raw_xsector_size, bool) or not isinstance(raw_xsector_size, int):
        fail("sqlite.normalization-vector-invalid", "xSectorSize is not an integer")
    if not isinstance(temporary_file, bool) or not isinstance(
        powersafe_overwrite, bool
    ):
        fail(
            "sqlite.normalization-vector-invalid",
            "sector-size device profile is not boolean",
        )
    if temporary_file or powersafe_overwrite:
        return 512
    if raw_xsector_size < 32:
        return 512
    if raw_xsector_size > 65_536:
        return 65_536
    return raw_xsector_size


def normalization_journal_layout(
    *,
    raw_xsector_size: int,
    decoded_page_size: int,
    header_offset: int = 0,
    record_index: int = 0,
    temporary_file: bool = False,
    powersafe_overwrite: bool = False,
) -> dict[str, int]:
    """Derive the live normalization journal layout from effective sector size S."""

    sector_size = effective_sqlite_sector_size(
        raw_xsector_size,
        temporary_file=temporary_file,
        powersafe_overwrite=powersafe_overwrite,
    )
    if sector_size & (sector_size - 1):
        fail(
            "sqlite.normalization-vector-invalid",
            "effective journal sector size is not a power of two",
        )
    if (
        isinstance(decoded_page_size, bool)
        or not isinstance(decoded_page_size, int)
        or decoded_page_size < 512
        or decoded_page_size > 65_536
        or decoded_page_size & (decoded_page_size - 1)
    ):
        fail("sqlite.normalization-vector-invalid", "invalid journal page size")
    if (
        isinstance(header_offset, bool)
        or not isinstance(header_offset, int)
        or header_offset < 0
        or header_offset % sector_size != 0
    ):
        fail("sqlite.normalization-vector-invalid", "invalid journal header offset")
    if (
        isinstance(record_index, bool)
        or not isinstance(record_index, int)
        or record_index < 0
    ):
        fail("sqlite.normalization-vector-invalid", "invalid journal record index")
    record_size = decoded_page_size + 8
    record_start = header_offset + sector_size + record_index * record_size
    record_end = record_start + record_size
    header_chunk_size = min(decoded_page_size, sector_size)
    return {
        "sector_size": sector_size,
        "header_offset": header_offset,
        "header_field_end": header_offset + 28,
        "header_padding_begin": header_offset + 28,
        "header_padding_end": header_offset + sector_size,
        "header_write_length": sector_size,
        "header_chunk_size": header_chunk_size,
        "header_chunk_count": sector_size // header_chunk_size,
        "record_index": record_index,
        "record_size": record_size,
        "page_number_offset": record_start,
        "page_image_offset": record_start + 4,
        "checksum_offset": record_start + 4 + decoded_page_size,
        "record_end": record_end,
        "next_header_offset": ((record_end + sector_size - 1) // sector_size)
        * sector_size,
    }


def normalization_large_sector_record_set(
    *,
    raw_xsector_size: int,
    decoded_page_size: int,
    database_page_count: int,
    temporary_file: bool = False,
    powersafe_overwrite: bool = False,
) -> dict[str, Any]:
    """Derive pagerWriteLargeSector's exact page set and one-header length."""

    layout = normalization_journal_layout(
        raw_xsector_size=raw_xsector_size,
        decoded_page_size=decoded_page_size,
        temporary_file=temporary_file,
        powersafe_overwrite=powersafe_overwrite,
    )
    if (
        isinstance(database_page_count, bool)
        or not isinstance(database_page_count, int)
        or database_page_count <= 0
    ):
        fail("sqlite.normalization-vector-invalid", "invalid database page count")
    locking_page = (0x4000_0000 // decoded_page_size) + 1
    sector_size = layout["sector_size"]
    pages_per_sector = (
        sector_size // decoded_page_size
        if sector_size > decoded_page_size
        else 1
    )
    upper = min(database_page_count, pages_per_sector)
    record_pages = tuple(
        page for page in range(1, upper + 1) if page != locking_page
    )
    if not record_pages:
        fail("sqlite.normalization-vector-invalid", "empty journal record set")
    record_size = decoded_page_size + 8
    return {
        "sector_size": sector_size,
        "page_size": decoded_page_size,
        "database_page_count": database_page_count,
        "locking_page": locking_page,
        "pages_per_sector": pages_per_sector,
        "record_pages": record_pages,
        "record_count": len(record_pages),
        "journal_size": sector_size + len(record_pages) * record_size,
        "main_write_offsets": tuple(
            (page - 1) * decoded_page_size for page in record_pages
        ),
    }


def normalization_record_pages_match(
    *,
    raw_xsector_size: int,
    decoded_page_size: int,
    database_page_count: int,
    observed_pages: tuple[int, ...],
) -> bool:
    """Match a parsed record sequence to E, including locking-page exclusion."""

    if not isinstance(observed_pages, tuple) or any(
        isinstance(page, bool) or not isinstance(page, int) or page <= 0
        for page in observed_pages
    ):
        fail("sqlite.normalization-vector-invalid", "invalid observed record pages")
    expected = normalization_large_sector_record_set(
        raw_xsector_size=raw_xsector_size,
        decoded_page_size=decoded_page_size,
        database_page_count=database_page_count,
    )
    return observed_pages == expected["record_pages"]


def normalization_reconstruct_pager_nonce(
    stored_checksum: bytes,
    page_image: bytes,
) -> int:
    """Invert the pinned pager_cksum sparse sum for one journal record."""

    if not isinstance(stored_checksum, bytes) or len(stored_checksum) != 4:
        fail("sqlite.normalization-vector-invalid", "checksum is not big-endian u32")
    if (
        not isinstance(page_image, bytes)
        or len(page_image) < 512
        or len(page_image) > 65_536
        or len(page_image) & (len(page_image) - 1)
    ):
        fail("sqlite.normalization-vector-invalid", "invalid checksum page image")
    sparse_sum = 0
    index = len(page_image) - 200
    while index > 0:
        sparse_sum = (sparse_sum + page_image[index]) & 0xFFFF_FFFF
        index -= 200
    return (int.from_bytes(stored_checksum, "big") - sparse_sum) & 0xFFFF_FFFF


def parse_normalization_journal_header_sector(
    header_sector: bytes,
    *,
    expected_sector_size: int,
    expected_page_size: int,
) -> dict[str, int]:
    """Fail closed on cold journal S/header-boundary mismatches."""

    if not isinstance(header_sector, bytes) or len(header_sector) < 28:
        fail("sqlite.normalization-vector-invalid", "journal header is too short")
    if (
        isinstance(expected_sector_size, bool)
        or not isinstance(expected_sector_size, int)
        or expected_sector_size < 32
        or expected_sector_size > 65_536
        or expected_sector_size & (expected_sector_size - 1)
    ):
        fail("sqlite.normalization-vector-invalid", "invalid expected sector size")
    parsed_sector_size = int.from_bytes(header_sector[20:24], "big")
    if (
        parsed_sector_size < 32
        or parsed_sector_size > 65_536
        or parsed_sector_size & (parsed_sector_size - 1)
        or parsed_sector_size != expected_sector_size
        or len(header_sector) != parsed_sector_size
    ):
        fail("sqlite.normalization-vector-invalid", "journal sector size mismatch")
    parsed_page_size = int.from_bytes(header_sector[24:28], "big")
    if parsed_page_size != expected_page_size:
        fail("sqlite.normalization-vector-invalid", "journal page size mismatch")
    return normalization_journal_layout(
        raw_xsector_size=parsed_sector_size,
        decoded_page_size=parsed_page_size,
    )


def normalization_arm_sequence(events: tuple[str, ...]) -> dict[str, Any]:
    """Model the exact denied -> coordination -> full arming order."""

    expected = (
        "install-pending-coordination-request",
        "first-exclusive-has-moved-zero-source-anchor-seal",
        "zero-wal-open",
        "install-pending-full-request",
        "second-exclusive-has-moved-zero-pre-effect-receipt-seal",
    )
    if not isinstance(events, tuple) or events != expected:
        fail(
            "sqlite.normalization-vector-invalid",
            "normalization arm sequence is skipped, duplicated, or reordered",
        )
    return {
        "denied": {
            "sequence": 1,
            "prerequisite": None,
            "armed_after_exclusive": False,
        },
        "coordination": {
            "sequence": 2,
            "prerequisite": 1,
            "armed_after_exclusive": True,
            "source_anchor_sealed": True,
        },
        "full": {
            "sequence": 3,
            "prerequisite": 2,
            "armed_after_exclusive": True,
            "pre_effect_receipt_sealed": True,
        },
    }


def normalization_terminal_edge(
    *,
    planned_candidate: bool,
    full_sequence_receipt: bool,
    effect_grammar_profile_receipt: bool,
    bounded_effect_transcript_receipt: bool,
    coordination_wal_delete_parent_sync_receipt: bool,
    journal_creation_parent_sync_receipt: bool,
    transition_result_delete: bool,
    terminal_journal_delete_parent_sync_receipt: bool,
    confirmed_close: bool,
    exact_expected_post: bool,
) -> str:
    """Keep a pre-effect candidate distinct from a post-close physical edge."""

    fields = (
        planned_candidate,
        full_sequence_receipt,
        effect_grammar_profile_receipt,
        bounded_effect_transcript_receipt,
        coordination_wal_delete_parent_sync_receipt,
        journal_creation_parent_sync_receipt,
        transition_result_delete,
        terminal_journal_delete_parent_sync_receipt,
        confirmed_close,
        exact_expected_post,
    )
    if any(not isinstance(value, bool) for value in fields):
        fail("sqlite.normalization-vector-invalid", "edge input is not boolean")
    if all(fields):
        return "sealed-post-close-physical-edge"
    return "no-edge-no-handoff"


def validate_option_a_vectors() -> None:
    """Run bounded executable witnesses for the compressed recovery model."""

    for profile in SQLITE_OPEN_PROFILE_NAMES:
        null_cleanup = sqlite_open_non_ok_cleanup(
            profile, handle_present=False, close_result=None
        )
        closed_cleanup = sqlite_open_non_ok_cleanup(
            profile, handle_present=True, close_result="ok"
        )
        quarantined_cleanup = sqlite_open_non_ok_cleanup(
            profile, handle_present=True, close_result="non-ok"
        )
        if (
            null_cleanup["close_attempts"] != 0
            or closed_cleanup["close_attempts"] != 1
            or closed_cleanup["quarantined"]
            or not quarantined_cleanup["quarantined"]
            or null_cleanup["retry"]
            or closed_cleanup["retry"]
            or quarantined_cleanup["retry"]
        ):
            fail(
                "sqlite.option-a-vector-mismatch",
                f"sqlite3_open_v2 cleanup differs for {profile}",
            )

    for encoded, decoded in (
        (1, 65_536),
        (512, 512),
        (4096, 4096),
        (8192, 8192),
        (32_768, 32_768),
    ):
        if decode_normalization_page_size(encoded) != decoded:
            fail("sqlite.option-a-vector-mismatch", "page-size decode differs")
    for invalid in (0, 2, 511, 513, 32_767, 65_535):
        try:
            decode_normalization_page_size(invalid)
        except SQLiteStoreContractError:
            pass
        else:
            fail("sqlite.option-a-vector-mismatch", "invalid page size was accepted")

    for encoded, byte_count in ((512, 512), (32_768, 32_768), (1, 65_536)):
        pre = bytearray(byte_count)
        pre[16:18] = encoded.to_bytes(2, "big")
        pre[18:20] = b"\x02\x02"
        pre[24:28] = (5).to_bytes(4, "big")
        pre[28:32] = (1).to_bytes(4, "big")
        pre[92:96] = (9).to_bytes(4, "big")
        pre[96:100] = (1).to_bytes(4, "big")
        post = normalization_expected_post_main(
            bytes(pre), pinned_libversion_number=3_045_001
        )
        if (
            post[18:20] != b"\x01\x01"
            or int.from_bytes(post[24:28], "big") != 6
            or int.from_bytes(post[92:96], "big") != 6
            or int.from_bytes(post[96:100], "big") != 3_045_001
            or post[100:] != bytes(pre[100:])
        ):
            fail("sqlite.option-a-vector-mismatch", "post-main projection differs")
    wrapping = bytearray(4096)
    wrapping[16:18] = (4096).to_bytes(2, "big")
    wrapping[24:28] = (0xFFFF_FFFF).to_bytes(4, "big")
    wrapping[28:32] = (1).to_bytes(4, "big")
    wrapping[92:96] = (0x1234_5678).to_bytes(4, "big")
    wrapped = normalization_expected_post_main(
        bytes(wrapping), pinned_libversion_number=3_045_001
    )
    if wrapped[24:28] != b"\0\0\0\0" or wrapped[92:96] != b"\0\0\0\0":
        fail("sqlite.option-a-vector-mismatch", "counter wrap projection differs")

    for raw_sector, expected_sector in (
        (16, 512),
        (32, 32),
        (512, 512),
        (4096, 4096),
        (65_536, 65_536),
        (131_072, 65_536),
    ):
        layout = normalization_journal_layout(
            raw_xsector_size=raw_sector,
            decoded_page_size=4096,
        )
        if layout != {
            "sector_size": expected_sector,
            "header_offset": 0,
            "header_field_end": 28,
            "header_padding_begin": 28,
            "header_padding_end": expected_sector,
            "header_write_length": expected_sector,
            "header_chunk_size": min(4096, expected_sector),
            "header_chunk_count": expected_sector // min(4096, expected_sector),
            "record_index": 0,
            "record_size": 4104,
            "page_number_offset": expected_sector,
            "page_image_offset": expected_sector + 4,
            "checksum_offset": expected_sector + 4 + 4096,
            "record_end": expected_sector + 8 + 4096,
            "next_header_offset": (
                (expected_sector + 8 + 4096 + expected_sector - 1)
                // expected_sector
            )
            * expected_sector,
        }:
            fail("sqlite.option-a-vector-mismatch", "journal S layout differs")
    psow_layout = normalization_journal_layout(
        raw_xsector_size=4096,
        decoded_page_size=4096,
        powersafe_overwrite=True,
    )
    if psow_layout["sector_size"] != 512:
        fail("sqlite.option-a-vector-mismatch", "PSOW sector selection differs")
    page_sizes = (512, 1024, 2048, 4096, 8192, 16_384, 32_768, 65_536)
    effective_sectors = tuple(1 << exponent for exponent in range(5, 17))
    for page_size in page_sizes:
        page_layout = normalization_journal_layout(
            raw_xsector_size=512,
            decoded_page_size=page_size,
        )
        if (
            page_layout["record_size"] != page_size + 8
            or page_layout["header_chunk_size"] != min(512, page_size)
        ):
            fail("sqlite.option-a-vector-mismatch", "legal page size vector differs")
        for effective_sector in effective_sectors:
            pages_per_sector = (
                effective_sector // page_size if effective_sector > page_size else 1
            )
            locking_page = (0x4000_0000 // page_size) + 1
            if locking_page <= pages_per_sector:
                fail(
                    "sqlite.option-a-vector-mismatch",
                    "locking-page exclusion proof differs",
                )
            counts = {
                count
                for count in (
                    1,
                    pages_per_sector - 1,
                    pages_per_sector,
                    pages_per_sector + 1,
                )
                if count > 0
            }
            for database_pages in counts:
                boundary = normalization_large_sector_record_set(
                    raw_xsector_size=effective_sector,
                    decoded_page_size=page_size,
                    database_page_count=database_pages,
                )
                expected_pages = tuple(
                    range(1, min(database_pages, pages_per_sector) + 1)
                )
                if boundary["record_pages"] != expected_pages:
                    fail(
                        "sqlite.option-a-vector-mismatch",
                        "parameterized S/P/N record set differs",
                    )
    wide_layout = normalization_journal_layout(
        raw_xsector_size=65_536,
        decoded_page_size=4096,
    )
    if (
        wide_layout["header_chunk_size"] != 4096
        or wide_layout["header_chunk_count"] != 16
    ):
        fail("sqlite.option-a-vector-mismatch", "wide journal header chunks differ")
    large_sector = normalization_large_sector_record_set(
        raw_xsector_size=65_536,
        decoded_page_size=4096,
        database_page_count=66,
    )
    if (
        large_sector["record_pages"] != tuple(range(1, 17))
        or large_sector["record_count"] != 16
        or large_sector["journal_size"] != 131_200
        or large_sector["main_write_offsets"] != tuple(range(0, 65_536, 4096))
    ):
        fail("sqlite.option-a-vector-mismatch", "large-sector record set differs")
    for database_pages, expected_count in ((1, 1), (15, 15), (16, 16), (17, 16)):
        boundary = normalization_large_sector_record_set(
            raw_xsector_size=65_536,
            decoded_page_size=4096,
            database_page_count=database_pages,
        )
        if boundary["record_count"] != expected_count:
            fail("sqlite.option-a-vector-mismatch", "large-sector count boundary differs")
    actual_locking_page = large_sector["locking_page"]
    if actual_locking_page <= large_sector["pages_per_sector"]:
        fail("sqlite.option-a-vector-mismatch", "locking page proof differs")
    if normalization_record_pages_match(
        raw_xsector_size=65_536,
        decoded_page_size=4096,
        database_page_count=16,
        observed_pages=large_sector["record_pages"] + (actual_locking_page,),
    ):
        fail("sqlite.option-a-vector-mismatch", "injected locking page was accepted")
    page_image = bytes((index * 37 + 11) & 0xFF for index in range(4096))
    expected_nonce = 0x1234_5678
    sparse_sum = sum(page_image[index] for index in range(4096 - 200, 0, -200))
    stored_checksum = ((expected_nonce + sparse_sum) & 0xFFFF_FFFF).to_bytes(4, "big")
    if (
        normalization_reconstruct_pager_nonce(stored_checksum, page_image)
        != expected_nonce
    ):
        fail("sqlite.option-a-vector-mismatch", "pager checksum nonce differs")
    ordinary_sector = normalization_large_sector_record_set(
        raw_xsector_size=512,
        decoded_page_size=4096,
        database_page_count=492,
    )
    if ordinary_sector["record_pages"] != (1,):
        fail("sqlite.option-a-vector-mismatch", "ordinary sector record set differs")
    for invalid_count in (0, -1, True):
        try:
            normalization_large_sector_record_set(
                raw_xsector_size=65_536,
                decoded_page_size=4096,
                database_page_count=invalid_count,
            )
        except SQLiteStoreContractError:
            pass
        else:
            fail(
                "sqlite.option-a-vector-mismatch",
                "invalid large-sector database page count was accepted",
            )
    for raw_sector in (33, 48, 1000, 65_535):
        try:
            normalization_journal_layout(
                raw_xsector_size=raw_sector,
                decoded_page_size=4096,
            )
        except SQLiteStoreContractError:
            pass
        else:
            fail(
                "sqlite.option-a-vector-mismatch",
                "non-power-of-two effective sector size was accepted",
            )

    for sector_size in (32, 512, 4096, 65_536):
        header = bytearray(sector_size)
        header[20:24] = sector_size.to_bytes(4, "big")
        header[24:28] = (4096).to_bytes(4, "big")
        parsed = parse_normalization_journal_header_sector(
            bytes(header),
            expected_sector_size=sector_size,
            expected_page_size=4096,
        )
        if parsed["page_number_offset"] != sector_size:
            fail("sqlite.option-a-vector-mismatch", "cold journal offset differs")

    valid_header = bytearray(4096)
    valid_header[20:24] = (4096).to_bytes(4, "big")
    valid_header[24:28] = (4096).to_bytes(4, "big")
    opaque_padding = bytearray(valid_header)
    opaque_padding[28] = 1
    opaque_padding[-1] = 0xA5
    opaque_parsed = parse_normalization_journal_header_sector(
        bytes(opaque_padding),
        expected_sector_size=4096,
        expected_page_size=4096,
    )
    if (
        opaque_parsed["header_padding_begin"] != 28
        or opaque_parsed["header_padding_end"] != 4096
    ):
        fail("sqlite.option-a-vector-mismatch", "opaque header boundary differs")
    invalid_headers = []
    mismatched_field = bytearray(valid_header)
    mismatched_field[20:24] = (512).to_bytes(4, "big")
    invalid_headers.append((bytes(mismatched_field), 4096, 4096))
    non_power_field = bytearray(valid_header)
    non_power_field[20:24] = (48).to_bytes(4, "big")
    invalid_headers.append((bytes(non_power_field[:48]), 48, 4096))
    below_minimum_field = bytearray(32)
    below_minimum_field[20:24] = (16).to_bytes(4, "big")
    below_minimum_field[24:28] = (4096).to_bytes(4, "big")
    invalid_headers.append((bytes(below_minimum_field), 32, 4096))
    above_maximum_field = bytearray(65_536)
    above_maximum_field[20:24] = (65_537).to_bytes(4, "big")
    above_maximum_field[24:28] = (4096).to_bytes(4, "big")
    invalid_headers.append((bytes(above_maximum_field), 65_536, 4096))
    invalid_headers.append((bytes(valid_header[:-1]), 4096, 4096))
    invalid_headers.append((bytes(valid_header) + b"\0", 4096, 4096))
    wrong_page_size = bytearray(valid_header)
    wrong_page_size[24:28] = (8192).to_bytes(4, "big")
    invalid_headers.append((bytes(wrong_page_size), 4096, 4096))
    for header, expected_sector, expected_page in invalid_headers:
        try:
            parse_normalization_journal_header_sector(
                header,
                expected_sector_size=expected_sector,
                expected_page_size=expected_page,
            )
        except SQLiteStoreContractError:
            pass
        else:
            fail("sqlite.option-a-vector-mismatch", "invalid journal S was accepted")

    exact_events = (
        "install-pending-coordination-request",
        "first-exclusive-has-moved-zero-source-anchor-seal",
        "zero-wal-open",
        "install-pending-full-request",
        "second-exclusive-has-moved-zero-pre-effect-receipt-seal",
    )
    receipts = normalization_arm_sequence(exact_events)
    if (
        receipts["denied"]["armed_after_exclusive"]
        or receipts["coordination"]["prerequisite"] != 1
        or receipts["full"]["prerequisite"] != 2
    ):
        fail("sqlite.option-a-vector-mismatch", "arm receipts differ")
    invalid_sequences = (
        exact_events[:-1],
        exact_events + (exact_events[-1],),
        (exact_events[1], exact_events[0], *exact_events[2:]),
    )
    for invalid_sequence in invalid_sequences:
        try:
            normalization_arm_sequence(invalid_sequence)
        except SQLiteStoreContractError:
            pass
        else:
            fail("sqlite.option-a-vector-mismatch", "invalid arm order was accepted")

    if normalization_terminal_edge(
        planned_candidate=True,
        full_sequence_receipt=True,
        effect_grammar_profile_receipt=True,
        bounded_effect_transcript_receipt=False,
        coordination_wal_delete_parent_sync_receipt=False,
        journal_creation_parent_sync_receipt=False,
        transition_result_delete=False,
        terminal_journal_delete_parent_sync_receipt=False,
        confirmed_close=False,
        exact_expected_post=False,
    ) != "no-edge-no-handoff":
        fail("sqlite.option-a-vector-mismatch", "candidate implied completed edge")
    if normalization_terminal_edge(
        planned_candidate=True,
        full_sequence_receipt=True,
        effect_grammar_profile_receipt=True,
        bounded_effect_transcript_receipt=True,
        coordination_wal_delete_parent_sync_receipt=True,
        journal_creation_parent_sync_receipt=True,
        transition_result_delete=True,
        terminal_journal_delete_parent_sync_receipt=True,
        confirmed_close=True,
        exact_expected_post=True,
    ) != "sealed-post-close-physical-edge":
        fail("sqlite.option-a-vector-mismatch", "completed edge was not sealed")
    complete_edge_inputs = {
        "planned_candidate": True,
        "full_sequence_receipt": True,
        "effect_grammar_profile_receipt": True,
        "bounded_effect_transcript_receipt": True,
        "coordination_wal_delete_parent_sync_receipt": True,
        "journal_creation_parent_sync_receipt": True,
        "transition_result_delete": True,
        "terminal_journal_delete_parent_sync_receipt": True,
        "confirmed_close": True,
        "exact_expected_post": True,
    }
    for required_field in (
        "planned_candidate",
        "full_sequence_receipt",
        "effect_grammar_profile_receipt",
        "bounded_effect_transcript_receipt",
        "coordination_wal_delete_parent_sync_receipt",
        "journal_creation_parent_sync_receipt",
        "transition_result_delete",
        "terminal_journal_delete_parent_sync_receipt",
        "confirmed_close",
        "exact_expected_post",
    ):
        incomplete_edge = dict(complete_edge_inputs)
        incomplete_edge[required_field] = False
        if normalization_terminal_edge(**incomplete_edge) != "no-edge-no-handoff":
            fail(
                "sqlite.option-a-vector-mismatch",
                f"missing {required_field} sealed completed edge",
            )

    writer_input = frozenset(
        {"SQLITE_OPEN_MAIN_DB", "SQLITE_OPEN_READWRITE"}
    )
    readwrite_proved = readwrite_open_observation(
        input_flags=writer_input,
        local_out_flags_before_call=0,
        sqlite_result="ok",
        returned_out_flags=frozenset(),
    )
    readonly_rejected = readwrite_open_observation(
        input_flags=writer_input,
        local_out_flags_before_call=0,
        sqlite_result="ok",
        returned_out_flags=frozenset({"SQLITE_OPEN_READONLY"}),
    )
    non_ok_ignores_output = readwrite_open_observation(
        input_flags=writer_input,
        local_out_flags_before_call=0,
        sqlite_result="non-ok",
        returned_out_flags=None,
    )

    tagged_none = committed_generation_maximum(())
    tagged_some_zero = committed_generation_maximum((0,))
    none_equation_origin = validate_committed_generation_maximum(
        tagged_none, committed_row_count=0
    )
    some_zero_equation_origin = validate_committed_generation_maximum(
        tagged_some_zero, committed_row_count=1
    )
    generation_tags_are_byte_distinct = not authority_state_bytes_equal(
        canonical_json(tagged_none), canonical_json(tagged_some_zero)
    )

    huge_schedule = compressed_compaction_schedule(1 << 63, 1, 1)
    designated_final = compressed_compaction_schedule(
        11, 2, 4, designated_final_population=3
    )
    replacement = canonical_replacement_order(
        [
            {"sequence": 7, "generation": 12, "publication_id": "X"},
            {"sequence": 7, "generation": 11, "publication_id": "Y"},
        ]
    )
    reset = format_reset_projection(
        ["legacy-v2-compact", "migrate-v2-v3"]
    )
    pre_empty_success = compaction_recovery_verdict(
        open_anchor_population=0,
        locked_census_population=1,
        exact_expected_candidate=False,
        compact_edge_populations=(1,),
    )
    pre_empty_uncompacted = compaction_recovery_verdict(
        open_anchor_population=0,
        locked_census_population=1,
        exact_expected_candidate=False,
        compact_edge_populations=(),
    )
    same_format_no_reset = scalar_authorized_descendant_summary(
        source_format="v3",
        target_format="v3",
        source_row_count=1,
        source_generation_maximum=("some", 1),
        target_row_count=2,
        target_generation_maximum=("some", 2),
    )
    same_format_final = scalar_authorized_descendant_summary(
        source_format="v3",
        target_format="v3",
        source_row_count=1,
        source_generation_maximum=("some", 1),
        target_row_count=1,
        target_generation_maximum=("some", 2),
    )
    migration_last = scalar_authorized_descendant_summary(
        source_format="v2",
        target_format="v3",
        source_row_count=1,
        source_generation_maximum=("some", 1),
        target_row_count=1,
        target_generation_maximum=("some", 2),
    )
    zero_population_migration = scalar_authorized_descendant_summary(
        source_format="v2",
        target_format="v3",
        source_row_count=0,
        source_generation_maximum=("none",),
        target_row_count=0,
        target_generation_maximum=("none",),
    )
    two_representation_counterexample = scalar_authorized_descendant_summary(
        source_format="v2",
        target_format="v3",
        source_row_count=1,
        source_generation_maximum=("some", 1),
        target_row_count=1,
        target_generation_maximum=("some", 3),
    )
    migration_boundary = scalar_authorized_descendant_summary(
        source_format="v2",
        target_format="v3",
        source_row_count=1,
        source_generation_maximum=("some", 1),
        target_row_count=2,
        target_generation_maximum=("some", 6),
    )
    boundary_candidate = next(
        candidate
        for candidate in migration_boundary["accepted_candidates"]
        if candidate["case"] == "v2_to_v3_final_v3_compaction_m_k"
        and candidate["migration_population"] == 1
        and candidate["last_reset_population"] == 2
    )
    huge_generation_distance = scalar_authorized_descendant_summary(
        source_format="v3",
        target_format="v3",
        source_row_count=1,
        source_generation_maximum=("some", 1),
        target_row_count=1,
        target_generation_maximum=("some", (1 << 63) + 1),
    )
    rejected_non_descendant = scalar_authorized_descendant_summary(
        source_format="v3",
        target_format="v3",
        source_row_count=1,
        source_generation_maximum=("some", 1),
        target_row_count=1,
        target_generation_maximum=("some", 0),
    )
    rejected_reverse_format = scalar_authorized_descendant_summary(
        source_format="v3",
        target_format="v2",
        source_row_count=1,
        source_generation_maximum=("some", 1),
        target_row_count=1,
        target_generation_maximum=("some", 1),
    )
    if (
        readwrite_proved != "read-write-proved"
        or readonly_rejected
        != "store.sqlite-failure/open/read-write-required"
        or non_ok_ignores_output != "open-non-ok-no-returned-flags"
        or tagged_none == tagged_some_zero
        or none_equation_origin != 0
        or some_zero_equation_origin != 0
        or not generation_tags_are_byte_distinct
        or huge_schedule != (((1, 1 << 63),), None)
        or designated_final != (((4, 2),), 3)
        or replacement != ("Y", "X")
        or reset
        != {
            "migration_count": 1,
            "v2_compactions": 1,
            "v3_compactions": 0,
            "last_reset": "migrate-v2-v3",
        }
        or pre_empty_success != "recovered-success"
        or pre_empty_uncompacted != "store.sqlite-failure/database/opaque"
        or authority_state_bytes_equal(b"canonical-left", b"canonical-right")
        or same_format_no_reset["canonical_reporting_witness"]["case"]
        != "same_format_no_reset"
        or same_format_no_reset["edge_presence"]["v3-compact"]
        or same_format_final["canonical_reporting_witness"]["case"]
        != "same_format_final_compaction_k"
        or not same_format_final["edge_presence"]["v3-compact"]
        or migration_last["canonical_reporting_witness"]["case"]
        != "v2_to_v3_migration_last_m"
        or migration_last["edge_presence"]
        != {
            "legacy-v2-compact": False,
            "migrate-v2-v3": True,
            "v3-compact": False,
        }
        or zero_population_migration["canonical_reporting_witness"][
            "migration_population"
        ]
        != 0
        or zero_population_migration["edge_presence"]
        != {
            "legacy-v2-compact": False,
            "migrate-v2-v3": True,
            "v3-compact": False,
        }
        or two_representation_counterexample["canonical_reporting_witness"][
            "case"
        ]
        != "v2_to_v3_migration_last_m"
        or two_representation_counterexample["edge_presence"]
        != {
            "legacy-v2-compact": True,
            "migrate-v2-v3": True,
            "v3-compact": True,
        }
        or boundary_candidate["format_tagged_residual_schedule"]
        != (("legacy-v2-compact", 1, 1),)
        or boundary_candidate["forced_edge_populations"]
        != {"legacy-v2-compact": (1,), "v3-compact": (1,)}
        or huge_generation_distance["canonical_reporting_witness"][
            "residual_schedule"
        ]
        != ((1, (1 << 63) - 1),)
        or len(huge_generation_distance["accepted_candidates"]) != 1
        or rejected_non_descendant["accepted"]
        or rejected_non_descendant["canonical_reporting_witness"] is not None
        or rejected_reverse_format["accepted"]
    ):
        fail(
            "sqlite.option-a-vector-mismatch",
            "compressed recovery-model executable vectors differ",
        )


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as error:
        fail("sqlite.document-invalid", f"{path}: {error}")
    if not isinstance(value, dict):
        fail("sqlite.document-invalid", f"{path}: root is not an object")
    return value


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")


def document_digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value)).hexdigest()


def validate_schema_document(schema: dict[str, Any]) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
    except jsonschema.SchemaError as error:
        fail("sqlite.schema-invalid", error.message)


def schema_validate(
    value: Any,
    schema: dict[str, Any],
    label: str = "SQLite store contract",
) -> None:
    try:
        jsonschema.Draft202012Validator(schema).validate(value)
    except jsonschema.ValidationError as error:
        fail("sqlite.schema-invalid", f"{label}: {error.message}")


def validate_exact_schema(schema: dict[str, Any]) -> None:
    actual = document_digest(schema)
    if actual != EXPECTED_SCHEMA_DIGEST:
        fail(
            "sqlite.schema-drift",
            (
                "canonical schema digest differs: "
                f"expected={EXPECTED_SCHEMA_DIGEST}, actual={actual}"
            ),
        )


def validate_exact_contract(contract: dict[str, Any]) -> None:
    actual = document_digest(contract)
    if actual != EXPECTED_CONTRACT_DIGEST:
        fail(
            "sqlite.contract-drift",
            (
                "canonical contract digest differs: "
                f"expected={EXPECTED_CONTRACT_DIGEST}, actual={actual}"
            ),
        )


def snapshot_binding_projection(snapshot: dict[str, Any]) -> dict[str, Any]:
    try:
        ingress = snapshot["df_0200_materialization_ingress"]
        compaction = snapshot["compaction"]
        format_compatibility = snapshot["format_compatibility"]
        terminal = snapshot["publication_transaction"]["sqlite_terminal_recovery"]
        terminal_receipts = terminal["sealed_receipt_profiles"]
        return {
            "authority": {
                "sqlite_decision_adr": snapshot["authority"][
                    "sqlite_decision_adr"
                ],
                "sqlite_decision_issue": snapshot["authority"][
                    "sqlite_decision_issue"
                ],
            },
            "sqlite_backend": ingress["sqlite_backend"],
            "capacity_decision": ingress["sqlite_capacity_decision"],
            "compatibility": ingress["compatibility"],
            "shared_generation_allocation": compaction[
                "generation_allocation"
            ],
            "sqlite_generation_lifetime": compaction[
                "sqlite_generation_lifetime"
            ],
            "sqlite_v2_to_v3_migration": compaction[
                "sqlite_v2_to_v3_migration"
            ],
            "sqlite_v3_compaction_commit_outcome_unknown": compaction[
                "sqlite_v3_compaction_commit_outcome_unknown"
            ],
            "format_compatibility": format_compatibility,
            "sqlite_terminal_recovery_normalization": {
                "sealed_receipt_profiles": {
                    "accepted_empty_normalization_source_anchor": terminal_receipts[
                        "accepted_empty_normalization_source_anchor"
                    ],
                    "accepted_empty_normalization": terminal_receipts[
                        "accepted_empty_normalization"
                    ],
                    "accepted_empty_normalization_completed_edge": terminal_receipts[
                        "accepted_empty_normalization_completed_edge"
                    ],
                },
                "accepted_empty_normalization_source_anchor": terminal[
                    "accepted_empty_normalization_source_anchor"
                ],
                "accepted_empty_normalization_source_anchor_profile": terminal[
                    "accepted_empty_normalization_source_anchor_profile"
                ],
                "accepted_empty_normalization_source_anchor_seal": terminal[
                    "accepted_empty_normalization_source_anchor_seal"
                ],
                "accepted_empty_normalization_receipt_seal": terminal[
                    "accepted_empty_normalization_receipt_seal"
                ],
                "accepted_empty_normalization_receipt_extension": terminal[
                    "accepted_empty_normalization_receipt_extension"
                ],
                "accepted_empty_normalization_candidate_identity": terminal[
                    "accepted_empty_normalization_candidate_identity"
                ],
                "accepted_empty_normalization_completed_edge_profile": terminal[
                    "accepted_empty_normalization_completed_edge_profile"
                ],
                "accepted_empty_normalization_completed_edge_seal": terminal[
                    "accepted_empty_normalization_completed_edge_seal"
                ],
                "accepted_empty_normalization_operation_identity": terminal[
                    "accepted_empty_normalization_operation_identity"
                ],
                "accepted_empty_normalization_success": terminal[
                    "accepted_empty_normalization_success"
                ],
                "accepted_empty_normalization_public_success": terminal[
                    "accepted_empty_normalization_public_success"
                ],
                "accepted_empty_normalization_receiptless_crash_profile_draft": (
                    terminal[
                        "accepted_empty_normalization_receiptless_crash_profile_draft"
                    ]
                ),
            },
        }
    except (KeyError, TypeError) as error:
        fail("sqlite.snapshot-binding-drift", f"binding field is missing: {error}")


def validate_snapshot_binding(
    contract: dict[str, Any], snapshot: dict[str, Any]
) -> None:
    try:
        semantic_contract = contract["authority"]["semantic_contract"]
        decision_adr = contract["authority"]["decision_adr"]
        decision_issue = contract["authority"]["decision_issue"]
        physical_format = contract["physical_format"]
        predecessor = physical_format["predecessor"]
        compatibility = contract["compatibility"]
        source_shm_capability = compatibility["predecessor_v2"][
            "read_path_strategy"
        ]["active_wal"]["source_shm_readonly_capability"]
        chunk_profile = contract["payload"]["chunk_profile"]
        migration = contract["migration"]
        terminal_reclassification = contract["transaction"]["recovery_model"][
            "terminal_reclassification"
        ]
    except (KeyError, TypeError) as error:
        fail(
            "sqlite.snapshot-binding-drift",
            f"SQLite authority field is missing: {error}",
        )

    if semantic_contract != SNAPSHOT_CONTRACT.as_posix():
        fail(
            "sqlite.snapshot-binding-drift",
            "SQLite semantic contract does not name the Snapshot authority",
        )

    actual = snapshot_binding_projection(snapshot)
    actual_binding_digest = document_digest(actual)
    if actual_binding_digest != EXPECTED_SNAPSHOT_BINDING:
        fail(
            "sqlite.snapshot-binding-drift",
            (
                "Snapshot Option A physical-format projection differs: "
                f"expected={EXPECTED_SNAPSHOT_BINDING}, "
                f"actual={actual_binding_digest}"
            ),
        )

    current_tag = f"{physical_format['id']}-{physical_format['current']}"
    predecessor_tag = (
        f"{predecessor['id']}-"
        f"{compatibility['predecessor_v2']['readable_format']}-read-only"
    )
    expected_ingress_tag = (
        f"{current_tag}-bounded-{chunk_profile['maximum_bytes']}-byte-chunks"
    )
    expected_terminal_normalization = {
        "sealed_receipt_profiles": {
            key: terminal_reclassification["sealed_receipt_profiles"][key]
            for key in (
                "accepted_empty_normalization_source_anchor",
                "accepted_empty_normalization",
                "accepted_empty_normalization_completed_edge",
            )
        },
        **{
            key: terminal_reclassification[key]
            for key in (
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
            )
        },
    }
    if (
        actual["authority"]
        != {
            "sqlite_decision_adr": decision_adr,
            "sqlite_decision_issue": decision_issue,
        }
        or actual["capacity_decision"]["decision_ref"] != decision_adr
        or actual["capacity_decision"]["decision_issue"] != decision_issue
        or actual["sqlite_backend"]["current_physical_format"]
        != expected_ingress_tag
        or actual["format_compatibility"]["current_sqlite"] != current_tag
        or actual["format_compatibility"]["readable_predecessor"]
        != predecessor_tag
        or not _matches_exact_requirement(
            source_shm_capability, SOURCE_SHM_READONLY_CAPABILITY
        )
        or actual["format_compatibility"][
            "sqlite_source_shm_readonly_capability"
        ]
        != source_shm_capability
        or actual["sqlite_v2_to_v3_migration"]["authority"] != decision_adr
        or actual["sqlite_v2_to_v3_migration"]["trigger"]
        != migration["public_trigger"]
        or actual["sqlite_terminal_recovery_normalization"]
        != expected_terminal_normalization
    ):
        fail(
            "sqlite.snapshot-binding-drift",
            "Snapshot projection is not derived from the exact SQLite authority",
        )


def validate_authorities(
    contract: dict[str, Any],
    schema: dict[str, Any],
    snapshot: dict[str, Any],
) -> None:
    validate_schema_document(schema)
    validate_option_a_contract(contract)
    validate_exact_schema(schema)
    schema_validate(contract, schema)
    validate_exact_contract(contract)
    validate_snapshot_binding(contract, snapshot)


def validate_all(root: pathlib.Path) -> dict[str, Any]:
    contract = load_yaml(root / CONTRACT)
    schema = load_yaml(root / CONTRACT_SCHEMA)
    snapshot = load_yaml(root / SNAPSHOT_CONTRACT)
    validate_authorities(contract, schema, snapshot)
    return contract


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    arguments = parser.parse_args()
    try:
        contract = validate_all(arguments.root.resolve())
    except SQLiteStoreContractError as error:
        print(f"NG SQLite store contract check failed: {error}", file=sys.stderr)
        return 1
    print(
        "verified NG SQLite store contract: "
        f"{document_digest(contract)}; Snapshot Option A binding exact"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
