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
    "sha256:4c5d624133aedc693d19a5846efc77805017b5fd8d7f684ebd0582d867419292"
)
EXPECTED_SCHEMA_DIGEST = (
    "sha256:5361a6ccfd87fe1dc1591f75d4ec8c75b5d2b8eab1a70a38db8890215c886192"
)

EXPECTED_SNAPSHOT_BINDING = (
    "sha256:4aa9eb36968e5d53f443a761660d1343d3c2388553d8e147b729c85630f6db2b"
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
                "private-copies-writers-fresh-recovery-scratch-and-ephemeral-memory"
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
                "readable-wal-without-shm-and-with-main-is-the-only-preauthority-"
                "crash-candidate"
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


def validate_option_a_contract(contract: dict[str, Any]) -> None:
    """Validate recovery/open semantics independently of the static digest."""

    actual = option_a_projection(contract)
    for path, expected in OPTION_A_REQUIREMENTS:
        label = ".".join(path)
        if actual[label] != expected:
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
        or source_shm_capability != SOURCE_SHM_READONLY_CAPABILITY
        or actual["format_compatibility"][
            "sqlite_source_shm_readonly_capability"
        ]
        != source_shm_capability
        or actual["sqlite_v2_to_v3_migration"]["authority"] != decision_adr
        or actual["sqlite_v2_to_v3_migration"]["trigger"]
        != migration["public_trigger"]
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
