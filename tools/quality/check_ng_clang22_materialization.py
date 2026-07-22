#!/usr/bin/env python3
"""Validate the installed Clang 22 materialization machine contract."""

from __future__ import annotations

import argparse
import base64
import binascii
import copy
import datetime
import decimal
import functools
import hashlib
import json
import pathlib
import re
import sys
import unicodedata
from typing import Any, Iterable, NoReturn

import jsonschema
import yaml

import check_ng_provider_runtime as provider_runtime


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONTRACT = pathlib.Path("schemas/cxxlens_ng_clang22_materialization_contract.yaml")
CONTRACT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml"
)
REQUEST_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_clang22_materialization_request.schema.yaml"
)
REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_clang22_materialization_report.schema.yaml"
)
OCCURRENCE_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_clang22_materializer_occurrence_manifest.schema.yaml"
)
REGISTRY = pathlib.Path("schemas/cxxlens_ng_relation_registry.yaml")
PROJECT_CATALOG = pathlib.Path("schemas/cxxlens_ng_project_catalog_contract.yaml")
PORTABLE_PROVIDER_TASK = pathlib.Path(
    "schemas/cxxlens_ng_portable_provider_task_contract.yaml"
)
PROVIDER_PROTOCOL = pathlib.Path("schemas/cxxlens_ng_provider_protocol.yaml")
PROVIDER_RUNTIME = pathlib.Path("schemas/cxxlens_ng_provider_runtime_contract.yaml")
SNAPSHOT_STORE = pathlib.Path("schemas/cxxlens_ng_snapshot_store_contract.yaml")
SQLITE_STORE = pathlib.Path("schemas/cxxlens_ng_sqlite_store_contract.yaml")
DF_0200_CORPUS = pathlib.Path(
    "schemas/cxxlens_ng_df_0200_claim_batch_differential_corpus.tsv"
)
DF_0200_CORPUS_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_df_0200_claim_batch_differential_corpus.schema.yaml"
)
DF_0200_CORPUS_DRIVER = pathlib.Path(
    "tests/adapter/clang22/df_0200_claim_batch_corpus_test.cpp"
)
TESTS_CMAKE = pathlib.Path("tests/CMakeLists.txt")
DECISION_ADR = pathlib.Path(
    "docs/design/adr/0096-clang22-installed-materialization-boundary.md"
)
INTEGRATED_DESIGN = pathlib.Path(
    "docs/design/cxxlens_next_generation_integrated_design_ja.md"
)
ROOT_CMAKE = pathlib.Path("CMakeLists.txt")
OCCURRENCE_GENERATOR_CMAKE = pathlib.Path(
    "cmake/GenerateClang22OccurrenceManifest.cmake.in"
)
SOURCE_PROVENANCE_CMAKE = pathlib.Path(
    "cmake/VerifyClang22SourceProvenance.cmake"
)
ROOTED_VFS_SOURCE = pathlib.Path(
    "src/llvm/clang22/materialization_rooted_vfs.cpp"
)
MATERIALIZATION_IO_SOURCE = pathlib.Path(
    "src/llvm/clang22/materialization_io.cpp"
)
MATERIALIZATION_IO_HEADER = pathlib.Path(
    "src/llvm/clang22/materialization_io.hpp"
)
MATERIALIZATION_REQUEST_STREAM_HEADER = pathlib.Path(
    "src/llvm/clang22/materialization_request_stream.hpp"
)
MATERIALIZATION_REQUEST_STREAM_SOURCE = pathlib.Path(
    "src/llvm/clang22/materialization_request_stream.cpp"
)
MATERIALIZATION_REQUEST_V2_1_SOURCE = pathlib.Path(
    "src/llvm/clang22/materialization_request_v2_1.cpp"
)
MATERIALIZATION_REQUEST_IDENTITY_SOURCE = pathlib.Path(
    "src/llvm/clang22/materialization_request_identity.cpp"
)
MATERIALIZATION_TASK_SPOOL_SOURCE = pathlib.Path(
    "src/llvm/clang22/materialization_task_spool.cpp"
)
MATERIALIZATION_ADMISSION_ERROR_HEADER = pathlib.Path(
    "src/llvm/clang22/materialization_admission_error.hpp"
)
MATERIALIZATION_REQUEST_DRIVER = pathlib.Path(
    "tests/adapter/clang22/materialization_request_driver.cpp"
)
STORE_SOURCE = pathlib.Path("src/sdk/store.cpp")
STORE_HEADER = pathlib.Path("include/cxxlens/sdk/store.hpp")
STORE_BACKEND_LIFETIME_INTERNAL = pathlib.Path(
    "src/sdk/store_backend_lifetime_internal.hpp"
)
SQLITE_CONNECTION_LIFECYCLE_INTERNAL = pathlib.Path(
    "src/sdk/sqlite_connection_lifecycle_internal.hpp"
)
SQLITE_CONNECTION_LIFECYCLE_SOURCE = pathlib.Path(
    "src/sdk/sqlite_connection_lifecycle_internal.cpp"
)
GENERIC_DEPENDENCIES = [
    REGISTRY,
    PROJECT_CATALOG,
    PORTABLE_PROVIDER_TASK,
    PROVIDER_PROTOCOL,
    PROVIDER_RUNTIME,
    SNAPSHOT_STORE,
    SQLITE_STORE,
]
AUTHORITY_PATHS = [
    CONTRACT,
    CONTRACT_SCHEMA,
    REQUEST_SCHEMA,
    REPORT_SCHEMA,
    REGISTRY,
]
OCCURRENCE_AUTHORITY_FILES = [
    (
        "relation-registry",
        "share/cxxlens/schemas/cxxlens_ng_relation_registry.yaml",
        REGISTRY,
    ),
    (
        "project-catalog-contract",
        "share/cxxlens/schemas/cxxlens_ng_project_catalog_contract.yaml",
        PROJECT_CATALOG,
    ),
    (
        "portable-provider-task-contract",
        "share/cxxlens/schemas/cxxlens_ng_portable_provider_task_contract.yaml",
        PORTABLE_PROVIDER_TASK,
    ),
    (
        "provider-protocol",
        "share/cxxlens/schemas/cxxlens_ng_provider_protocol.yaml",
        PROVIDER_PROTOCOL,
    ),
    (
        "provider-runtime-contract",
        "share/cxxlens/schemas/cxxlens_ng_provider_runtime_contract.yaml",
        PROVIDER_RUNTIME,
    ),
    (
        "snapshot-store-contract",
        "share/cxxlens/schemas/cxxlens_ng_snapshot_store_contract.yaml",
        SNAPSHOT_STORE,
    ),
    (
        "sqlite-store-contract",
        "share/cxxlens/schemas/cxxlens_ng_sqlite_store_contract.yaml",
        SQLITE_STORE,
    ),
    (
        "materialization-contract",
        "share/cxxlens/schemas/cxxlens_ng_clang22_materialization_contract.yaml",
        CONTRACT,
    ),
    (
        "materialization-contract-schema",
        "share/cxxlens/schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml",
        CONTRACT_SCHEMA,
    ),
    (
        "materialization-request-schema",
        "share/cxxlens/schemas/cxxlens_ng_clang22_materialization_request.schema.yaml",
        REQUEST_SCHEMA,
    ),
    (
        "materialization-report-schema",
        "share/cxxlens/schemas/cxxlens_ng_clang22_materialization_report.schema.yaml",
        REPORT_SCHEMA,
    ),
]
SHARED_OCCURRENCE_RUNTIME_FILES = [
    ("base", "lib/libcxxlens_base.so"),
    ("kernel", "lib/libcxxlens_kernel.so"),
    ("query", "lib/libcxxlens_query.so"),
    ("recipes", "lib/libcxxlens_recipes.so"),
    ("provider-sdk", "lib/libcxxlens_provider_sdk.so"),
    ("clang22-provider-sdk", "lib/libcxxlens_clang22_provider_sdk.so"),
]
FORBIDDEN_REPORT_LIFECYCLE_TEXT = (
    "bounded-spool-before-publication",
    "publication 前に bounded private spool 上で完全な schema-valid bytes まで構築する",
    "complete-report-before-publication",
    "schema-valid-report-before-publication",
)

DESCRIPTOR_IDS = [
    "cc.call_direct_target.v1",
    "cc.call_site.v1",
    "cc.entity.v1",
    "frontend.clang22.call_observation.v2",
    "frontend.clang22.entity_observation.v2",
    "frontend.clang22.type_observation.v2",
]
BASE_DESCRIPTOR_IDS = [
    "build.project.v1",
    "build.toolchain_context.v1",
    "build.variant.v1",
    "source.file.v1",
    "build.compile_unit.v1",
    "source.span.v1",
]
ADMITTED_DESCRIPTOR_IDS = sorted(BASE_DESCRIPTOR_IDS + DESCRIPTOR_IDS)
ENGINE_GENERATION_CONTRACT = "cxxlens.clang22-materialization-engine.v2"
INTERPRETATION_POLICY_ID = "cxxlens.clang22-interpretation-policy.v1"
INTERPRETATION_DOMAIN = "cc.clang22-canonical-1"
TRUST_POLICY_ID = "cxxlens.clang22-installed-native-worker-trust.v1"
RAW_INPUT_BYTE_LIMIT = 1_073_741_824
MATERIALIZATION_VERSION = "2.1.0"
PROVIDER_PROTOCOL_MINOR = 1
TASK_INPUT_FEATURE = "task-input-chunks-v1"
TASK_INPUT_CHUNK_BYTES = 1_048_576
MAXIMUM_TASK_INPUT_BYTES = 67_108_864
MAXIMUM_TASK_INPUT_CHUNKS = 64
MAXIMUM_STRONG_ID_UTF8_BYTES = 2_048
MAXIMUM_LOGICAL_PATH_UTF8_BYTES = 4_096
MAXIMUM_SQLITE_RELATIVE_PATH_UTF8_BYTES = 4_095
MAXIMUM_ARGV_ITEMS = 4_096
MAXIMUM_ARGV_ITEM_UTF8_BYTES = 2_048
MAXIMUM_TASK_SANDBOX_REQUIREMENTS = 4_096
MAXIMUM_JSON_MEMBERS_PER_OBJECT = 4_096
MAXIMUM_REQUEST_SCHEMA_CAPTURE_BYTES = 43
MAXIMUM_REQUEST_VERSION_CAPTURE_BYTES = 6
MAXIMUM_SEMANTIC_REPLAY_WINDOW_BYTES = 64 * 1_024 * 1_024
EXPECTED_REQUEST_SCHEMA_CANONICAL_DIGEST = (
    "sha256:241fc96ae3a249e5a8851baa95e585460ad29378cb20d11cfcda33a69eaa9270"
)
EXPECTED_REPORT_SCHEMA_CANONICAL_DIGEST = (
    "sha256:f321e25f72bf8c6312dfe1e36fe6b6573239db697c2cfabd60e2c0546f9ee98b"
)
DF_0200_REPORT_SHAPE_ACTIVATION = (
    "request-2.1.0-unchanged-report-private-spool-failure-"
    "occurrence-inventory-sandbox-bounds-activated"
)
MAXIMUM_GLOBAL_SEMANTIC_JSON_BYTES = 10_420_985
MAXIMUM_TASK_METADATA_SEMANTIC_JSON_BYTES = 8_463_179
CANONICAL_BASE64_PATTERN = (
    r"^(?:[A-Za-z0-9+/]{4})*"
    r"(?:[A-Za-z0-9+/][AQgw]==|[A-Za-z0-9+/]{2}[AEIMQUYcgkosw048]=)?$"
    r"(?![\s\S])"
)
CANONICAL_BASE64_ANNOTATION = (
    "rfc4648-standard-required-padding-zero-discarded-bits"
)
OCCURRENCE_MANIFEST_PATH = (
    "share/cxxlens/materialization/clang22/occurrence-v1.json"
)
GUARANTEE_PROFILE_ID = "cxxlens.clang22-materialization-guarantee-profile.v1"
GUARANTEE_ASSUMPTIONS: list[str] = []
GUARANTEE_MODALITIES = [
    "clang22.materialization-sealed.v1",
    "provider.transcript-sealed.v1",
    "sdk.claim-envelope-validated.v1",
]
TRANSPORT_COVERAGE_KIND = "task"
SEMANTIC_COVERAGE_KINDS = [
    "cc.call-extraction",
    "cc.entity",
    "frontend.clang22.observation",
]
COVERAGE_STATES = [
    "covered",
    "excluded",
    "not_applicable",
    "failed",
    "unresolved",
    "unsupported",
    "stale",
    "truncated",
]
BASE_RESULT_FIELDS = {
    "build.project.v1": "project",
    "build.toolchain_context.v1": "toolchain",
    "build.variant.v1": "variant",
    "source.file.v1": "snapshot",
    "build.compile_unit.v1": "compile_unit",
    "source.span.v1": "span",
}
EXPECTED_JSON_LEXICAL_POLICY = {
    "encoding": "strict-utf8-no-bom",
    "document": "exactly-one-top-level-object",
    "duplicate_members": "reject-at-any-depth",
    "trailing_or_second_value": "reject",
    "non_finite_numbers": "reject",
    "yaml_authority_loading": "separate",
}
EXPECTED_SEMANTIC_REPLAY_PROJECTION = {
    "token_replay": (
        "decoded-utf8-minimal-json-escape-canonical-integer-"
        "no-insignificant-whitespace"
    ),
    "raw_spelling_bound": "excluded",
    "schema_walk": "closed-required-local-ref-allof-intersection-oneof-maximum",
    "global_substitution": "tasks-empty-array",
    "task_metadata_substitution": "source-content-base64-empty-string",
    "window_bytes": MAXIMUM_SEMANTIC_REPLAY_WINDOW_BYTES,
    "global_selected_schema_maximum_bytes": MAXIMUM_GLOBAL_SEMANTIC_JSON_BYTES,
    "global_margin_bytes": (
        MAXIMUM_SEMANTIC_REPLAY_WINDOW_BYTES - MAXIMUM_GLOBAL_SEMANTIC_JSON_BYTES
    ),
    "task_metadata_selected_schema_maximum_bytes": (
        MAXIMUM_TASK_METADATA_SEMANTIC_JSON_BYTES
    ),
    "task_metadata_margin_bytes": (
        MAXIMUM_SEMANTIC_REPLAY_WINDOW_BYTES
        - MAXIMUM_TASK_METADATA_SEMANTIC_JSON_BYTES
    ),
    "request_schema_canonical_digest": EXPECTED_REQUEST_SCHEMA_CANONICAL_DIGEST,
}
EXPECTED_SEMANTIC_REPLAY = {
    **EXPECTED_SEMANTIC_REPLAY_PROJECTION,
    "derivation_digest": (
        "sha256:ff9baf9982f909d8a4f51c46f53637af6980a7d06728dfa65794ffc1eebf816d"
    ),
}
EXPECTED_REPORT_CONSTRUCTION = {
    "lifecycle": "bounded-two-phase-report-lifecycle",
    "lifecycle_order": [
        "publication-independent-projection-and-validation",
        "final-response-capacity-reservation",
        "publication-attempt",
        "exact-outcome-capture",
        "outcome-specific-reopen-or-recovery",
        "complete-response-finalization",
        "selected-v2-full-schema-validation",
        "bottom-up-cross-binding-validation",
        "stdout-publication",
    ],
    "prepublication_projection": "publication-independent-only",
    "prepublication_forbidden_claims": [
        "complete-response-authority",
        "publication-outcome",
        "invocation-publication-record",
        "physical-generation",
        "reopen-path-status-or-projection",
    ],
    "capacity_reservation": {
        "bound": (
            "checked-maximum-over-every-applicable-detailed-outcome-within-response-"
            "limit"
        ),
        "includes": [
            "final-json-framing",
            "exact-publication-outcome",
            "exact-sdk-records-and-receipts",
            "maximum-bounded-diagnostics",
        ],
        "failure": "compact-only-if-schema-valid-zero-effect-otherwise-exit-two",
    },
    "publication_attempt_boundary": (
        "immediately-before-exactly-one-snapshot-writer-publish-call"
    ),
    "publication_dependent_source": "exact-sdk-return-values-and-typed-errors-only",
    "committed_verified_reopen_order": [
        "capture-publish-returned-record",
        "backend-appropriate-reopen",
        "current-selector",
        "open-publication",
        "open-snapshot",
    ],
    "stdout_authority": {
        "before-full-validation": "forbidden",
        "authoritative-unit": "exactly-one-complete-json-response",
        "partial-or-short-write": "non-authoritative",
        "operating-system-atomicity": "not-claimed",
    },
}
EXPECTED_ADMISSION_FAILURE = {
    "phase_authentic_spool_failure": {
        "lexical_raw_and_task_index": "json-decode",
        "selected_schema_global_task_source_and_uniqueness": "request-schema",
        "derived_identity_source_task_input_and_execution_uniqueness": (
            "request-binding"
        ),
    },
    "internal_no_response": {
        "code_visibility": "source-private-not-a-stable-report-error",
        "causes": [
            "phase-opaque-allocation-failure",
            "proved-bound-invariant-breach",
            "compact-response-unsafe",
        ],
        "process_outcome": "exit-two-zero-stdout",
    },
    "forbidden_admission_codes": [
        "materialization.io-failure",
        "materialization.resource-limit",
        "materialization.internal-failure",
    ],
}
DF_0200_PARTITION_EVENT_CODEC = {
    "id": "cxxlens.df-0200.partition-event-stream.v1",
    "scope": "full-byte-private-partition-event-stream",
    "byte_order": "big-endian",
    "canonical_scalar_codec": "cxxlens-canonical-tuple-v1",
    "canonical_tuple_profile": {
        "kind_tags": {
            "null": 0,
            "boolean": 1,
            "signed-integer": 2,
            "bytes": 3,
            "utf8-string": 4,
            "ordered-tuple": 5,
        },
        "length_encoding": "u64be",
        "signed_integer_encoding": (
            "sign-u8-then-minimal-magnitude-length-u64be-and-big-endian-magnitude"
        ),
        "utf8": "strict-no-repair",
        "decode_rule": (
            "consume-one-complete-value-reencode-byte-identical-no-trailing-bytes"
        ),
    },
    "field_catalog": {
        "utf8-string-exactly-one": [
            "task-id",
            "partition-id",
            "relation-descriptor-id",
            "scope",
            "condition",
            "interpretation",
            "producer-semantics",
            "input-basis-digest",
            "precision-profile",
            "assumption-set-id",
            "canonical-row-identity",
            "claim-content-digest",
            "coverage-identity",
            "unresolved-identity",
            "event-digest",
            "claim-digest",
            "row-digest",
            "coverage-digest",
            "unresolved-digest",
            "partition-content-digest",
        ],
        "canonical-bytes-exactly-one": [
            "claim-canonical-order-key",
            "occurrence-canonical-order-key",
            "claim-content-canonical-bytes",
            "occurrence-metadata-canonical-bytes",
            "canonical-row-bytes",
            "annotation-order-key",
            "canonical-annotation-bytes",
            "canonical-coverage-bytes",
            "canonical-typed-unresolved-bytes",
        ],
        "u64be-as-canonical-bytes-exactly-one": [
            "event-count",
            "claim-count",
            "row-count",
            "coverage-count",
            "unresolved-count",
        ],
        "ordered-unique-canonical-bytes-tuple-exactly-one": [
            "hard-reference-targets",
            "soft-reference-targets",
            "functional-conflict-keys",
            "differential-keys",
        ],
        "collection_item_order": "ascending-full-canonical-item-bytes",
        "collection_duplicate_policy": "reject",
        "scalar_cardinality": "exactly-one-non-null",
        "unknown_or_multiply_classified_field": "reject",
    },
    "stream_header": {
        "magic_ascii_8": "CXLPEV01",
        "codec_version_u16be": 1,
        "header_length_u32be": 86,
        "stream_sequence_id": "sha256-32-raw",
        "spool_index": "u64be",
        "first_event_ordinal": "u128be",
        "declared_frame_count": "u64be",
        "declared_body_bytes": "u64be",
        "authority": "self-reported-cross-check-only",
    },
    "frame": {
        "layout": [
            "event-kind-u8",
            "key-length-u64be",
            "payload-length-u64be",
            "key-bytes",
            "payload-bytes",
            "checksum-sha256-32-raw",
        ],
        "framed_length": (
            "one-plus-eight-plus-eight-plus-key-length-plus-payload-length-plus-"
            "thirty-two"
        ),
        "key_and_payload_encoding": "cxxlens-canonical-tuple-v1",
        "checksum_domain": "cxxlens/df-0200-partition-event-frame/v1",
        "checksum_input": (
            "domain-length-u64be-domain-utf8-projection-length-u64be-then-kind-"
            "lengths-key-and-payload-before-checksum"
        ),
    },
    "event_projections": {
        "partition-begin": {
            "kind_code": 1,
            "key_fields": ["task-id", "partition-id"],
            "payload_fields": [
                "relation-descriptor-id",
                "scope",
                "condition",
                "interpretation",
                "producer-semantics",
                "input-basis-digest",
                "precision-profile",
                "assumption-set-id",
            ],
        },
        "claim-occurrence": {
            "kind_code": 2,
            "key_fields": [
                "task-id",
                "partition-id",
                "claim-canonical-order-key",
                "occurrence-canonical-order-key",
            ],
            "payload_fields": [
                "claim-content-canonical-bytes",
                "occurrence-metadata-canonical-bytes",
                "hard-reference-targets",
                "soft-reference-targets",
                "functional-conflict-keys",
                "differential-keys",
            ],
        },
        "detached-row": {
            "kind_code": 3,
            "key_fields": [
                "task-id",
                "partition-id",
                "relation-descriptor-id",
                "canonical-row-identity",
            ],
            "payload_fields": ["canonical-row-bytes"],
        },
        "claim-annotation": {
            "kind_code": 4,
            "key_fields": [
                "task-id",
                "partition-id",
                "claim-content-digest",
                "annotation-order-key",
            ],
            "payload_fields": ["canonical-annotation-bytes"],
        },
        "coverage": {
            "kind_code": 5,
            "key_fields": ["task-id", "partition-id", "coverage-identity"],
            "payload_fields": ["canonical-coverage-bytes"],
        },
        "unresolved": {
            "kind_code": 6,
            "key_fields": ["task-id", "partition-id", "unresolved-identity"],
            "payload_fields": ["canonical-typed-unresolved-bytes"],
        },
        "partition-end": {
            "kind_code": 7,
            "key_fields": ["task-id", "partition-id"],
            "payload_fields": [
                "event-count",
                "claim-count",
                "row-count",
                "coverage-count",
                "unresolved-count",
                "event-digest",
                "claim-digest",
                "row-digest",
                "coverage-digest",
                "unresolved-digest",
                "partition-content-digest",
            ],
        },
    },
    "event_container": {
        "key": "ordered-tuple-of-key-fields-in-listed-order",
        "payload": "ordered-tuple-of-payload-fields-in-listed-order",
        "field_cardinality": "every-listed-field-exactly-once-no-other-fields",
        "full_event_projection": [
            "event-kind-u8-as-one-canonical-byte-string",
            "exact-key-canonical-bytes-as-canonical-byte-string",
            "exact-payload-canonical-bytes-as-canonical-byte-string",
        ],
        "list_framing": (
            "ordered-tuple-of-full-event-projection-canonical-byte-strings"
        ),
        "list_deduplication": "reject-identical-full-event-projection",
    },
    "digest_framing": {
        "raw_sha256": (
            "domain-length-u64be-domain-utf8-projection-length-u64be-projection-bytes"
        ),
        "semantic_v2": (
            "canonical-tuple-cxxlens-semantic-digest-v2-domain-utf8-projection-bytes"
        ),
    },
    "digest_domains": {
        "stream-sequence-id": {
            "domain": "cxxlens/df-0200-partition-event-stream-sequence/v1",
            "output": "sha256-32-raw",
            "projection": [
                "materialization-request-id-utf8",
                "spool-index-u64be",
                "first-event-ordinal-u128be",
            ],
        },
        "frame-checksum": {
            "domain": "cxxlens/df-0200-partition-event-frame/v1",
            "output": "sha256-32-raw",
            "projection": [
                "event-kind-u8",
                "key-length-u64be",
                "payload-length-u64be",
                "exact-key-bytes",
                "exact-payload-bytes",
            ],
        },
        "trailer-frames-digest": {
            "domain": "cxxlens/df-0200-partition-event-frames/v1",
            "output": "sha256-32-raw",
            "projection": [
                "spool-index-u64be",
                "actual-frame-count-u64be",
                "ordered-u64be-length-prefixed-full-frame-bytes",
            ],
        },
        "trailer-stream-prefix-digest": {
            "domain": "cxxlens/df-0200-partition-event-stream-prefix/v1",
            "output": "sha256-32-raw",
            "projection": [
                "exact-header-bytes",
                "actual-body-bytes-u64be",
                "ordered-u64be-length-prefixed-full-frame-bytes",
            ],
        },
        "task-event-digest": {
            "domain": "cxxlens.df-0200.task-event-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "task-id",
                "event-count-u64be-as-canonical-bytes",
                "ordered-full-event-projection-bytes",
            ],
        },
        "partition-event-digest": {
            "domain": "cxxlens.df-0200.partition-event-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "partition-id",
                "event-count-u64be-as-canonical-bytes",
                "ordered-full-event-projection-bytes",
            ],
        },
        "task-partition-set-digest": {
            "domain": "cxxlens.df-0200.task-partition-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "task-id",
                "partition-count-u64be-as-canonical-bytes",
                "ordered-partition-id-and-full-projection-digest-tuples",
            ],
        },
        "global-event-digest": {
            "domain": "cxxlens.df-0200.global-event-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "materialization-request-id",
                "event-count-u64be-as-canonical-bytes",
                "ordered-full-event-projection-bytes",
            ],
        },
        "claim-digest": {
            "domain": "cxxlens.df-0200.claim-occurrence-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "partition-id",
                "claim-count-u64be-as-canonical-bytes",
                "ordered-full-claim-occurrence-projection-bytes",
            ],
        },
        "task-claim-digest": {
            "domain": "cxxlens.df-0200.task-claim-occurrence-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "task-id",
                "claim-count-u64be-as-canonical-bytes",
                "ordered-full-claim-occurrence-projection-bytes",
            ],
        },
        "global-claim-digest": {
            "domain": "cxxlens.df-0200.global-claim-occurrence-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "materialization-request-id",
                "claim-count-u64be-as-canonical-bytes",
                "ordered-full-claim-occurrence-projection-bytes",
            ],
        },
        "row-digest": {
            "domain": "cxxlens.df-0200.detached-row-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "partition-id",
                "row-count-u64be-as-canonical-bytes",
                "ordered-full-detached-row-projection-bytes",
            ],
        },
        "task-row-digest": {
            "domain": "cxxlens.df-0200.task-detached-row-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "task-id",
                "row-count-u64be-as-canonical-bytes",
                "ordered-full-detached-row-projection-bytes",
            ],
        },
        "global-row-digest": {
            "domain": "cxxlens.df-0200.global-detached-row-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "materialization-request-id",
                "row-count-u64be-as-canonical-bytes",
                "ordered-full-detached-row-projection-bytes",
            ],
        },
        "coverage-digest": {
            "domain": "cxxlens.df-0200.coverage-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "partition-id",
                "coverage-count-u64be-as-canonical-bytes",
                "ordered-full-coverage-projection-bytes",
            ],
        },
        "task-coverage-digest": {
            "domain": "cxxlens.df-0200.task-coverage-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "task-id",
                "coverage-count-u64be-as-canonical-bytes",
                "ordered-full-coverage-projection-bytes",
            ],
        },
        "global-coverage-digest": {
            "domain": "cxxlens.df-0200.global-coverage-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "materialization-request-id",
                "coverage-count-u64be-as-canonical-bytes",
                "ordered-full-coverage-projection-bytes",
            ],
        },
        "unresolved-digest": {
            "domain": "cxxlens.df-0200.unresolved-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "partition-id",
                "unresolved-count-u64be-as-canonical-bytes",
                "ordered-full-unresolved-projection-bytes",
            ],
        },
        "task-unresolved-digest": {
            "domain": "cxxlens.df-0200.task-unresolved-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "task-id",
                "unresolved-count-u64be-as-canonical-bytes",
                "ordered-full-unresolved-projection-bytes",
            ],
        },
        "global-unresolved-digest": {
            "domain": "cxxlens.df-0200.global-unresolved-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "materialization-request-id",
                "unresolved-count-u64be-as-canonical-bytes",
                "ordered-full-unresolved-projection-bytes",
            ],
        },
        "global-task-set-digest": {
            "domain": "cxxlens.df-0200.global-task-receipt-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "materialization-request-id",
                "task-count-u64be-as-canonical-bytes",
                "canonical-task-order-and-full-receipt-seal-digest-tuples",
            ],
        },
        "global-partition-set-digest": {
            "domain": "cxxlens.df-0200.global-partition-full-projection.v1",
            "output": "semantic-v2-sha256-string",
            "projection": [
                "materialization-request-id",
                "partition-count-u64be-as-canonical-bytes",
                "ordered-task-id-partition-id-and-full-projection-digest-tuples",
            ],
        },
        "partition-content-digest": {
            "domain": "partition-content",
            "output": "store-canonical-identity-v1-string",
            "projection": [
                "partition-id",
                "claim-set-digest",
                "coverage-digest",
            ],
        },
    },
    "canonical_order": [
        "validated-request-canonical-task-order",
        "full-partition-id-bytes",
        "partition-begin",
        "claim-occurrence-full-key-bytes",
        "detached-row-full-key-bytes",
        "claim-annotation-full-key-bytes",
        "coverage-full-key-bytes",
        "unresolved-full-key-bytes",
        "partition-end",
    ],
    "stream_trailer": {
        "magic_ascii_8": "CXLPEEND",
        "trailer_length_bytes": 112,
        "layout": [
            "magic-ascii-8",
            "spool-index-u64be",
            "next-event-ordinal-u128be",
            "actual-frame-count-u64be",
            "actual-body-bytes-u64be",
            "frames-sha256-32-raw",
            "stream-prefix-sha256-32-raw",
        ],
        "authority": "self-reported-cross-check-only",
    },
    "rejection": [
        "unknown-event-kind",
        "missing-or-duplicate-header-or-trailer",
        "noncanonical-key-or-payload",
        "missing-duplicate-or-interleaved-partition-begin-or-end",
        "task-partition-or-event-reordered",
        "event-ordinal-gap-overlap-or-spool-index-drift",
        "declared-versus-actual-count-or-byte-mismatch",
        "frame-checksum-or-stream-digest-mismatch",
        "truncated-length-key-payload-checksum-or-trailer",
        "trailing-bytes-after-trailer",
        "whole-partition-drop-against-external-authority",
    ],
}
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
EXPECTED_DF_0200_RESOLUTION = {
    "status": "accepted-authority-implementation-pending",
    "resolution_id": "cxxlens.df-0200.incremental-claim-store.v1",
    "implementation_disposition": "pending-implementation-and-qualification",
    "d1_claim_batch_oracle": {
        "public_reference": "sdk-claim-batch-commit",
        "public_reference_control_flow": (
            "independent-bounded-resident-reference-during-qualification"
        ),
        "production_path": "source-private-incremental-external-validator",
        "shared_implementation_allowlist": [
            "canonical-codecs",
            "identity-functions",
            "field-validators",
        ],
        "shared_commit_control_flow_or_verdict_logic": "forbidden",
        "literal_all_claims_vector_call": "required-for-bounded-qualification-only",
        "exact_equivalence": [
            "accepted-final-occurrence-multiset",
            "canonical-order",
            "exact-duplicate-deduplication",
            "metadata-distinct-same-content-preservation",
            "hard-and-soft-reference-verdicts",
            "functional-conflicts",
            "differential-disagreements",
            "unresolved-conflict-and-differential-censuses",
            "cxxlens-claim-batch-v2-canonical-encoding-and-content-digest",
        ],
        "digest_only_equality_or_order": "forbidden",
        "qualification_corpus": {
            "id": "cxxlens.df-0200.claim-batch-differential-corpus.v1",
            "freeze_point": "pre-refactor-canonical-bytes-and-verdicts",
            "artifact": {
                "path": str(DF_0200_CORPUS),
                "schema": str(DF_0200_CORPUS_SCHEMA),
                "schema_id": (
                    "https://cxxlens.dev/schema/ng/"
                    "df-0200-claim-batch-differential-corpus.schema.yaml"
                ),
                "format": (
                    "cxxlens.df-0200.claim-batch-differential-corpus.tsv.v1"
                ),
                "artifact_version": "1.0.0",
                "raw_sha256": (
                    "sha256:f05513d05b0b57788b6f94d9c1a477c88d589b64dd8232d88a5c6c6022a84836"
                ),
            },
            "current_public_reference": {
                "driver": str(DF_0200_CORPUS_DRIVER),
                "cmake_target": "cxxlens-df0200-claim-batch-corpus",
                "ctest": "qualification.df0200-claim-batch-corpus",
                "operation": "cxxlens::sdk::claim_batch::commit",
                "mode": "execute-all-frozen-cases-and-compare-exact-artifact-bytes",
                "generator": {
                    "visibility": "source-private",
                    "option": "--emit",
                    "artifact_update_gate": (
                        "independent-review-and-new-frozen-digest"
                    ),
                },
            },
            "census": {
                "case_count": 10,
                "success_count": 9,
                "error_count": 1,
                "added_claim_count": 17,
                "existing_claim_count": 16,
                "projection_byte_count": 88_607,
                "projection_digest": (
                    "semantic-v2:sha256:741e9c5d7682f11a574b5218de1b48b009907782441bc3c9ac1488f264d42ba3"
                ),
                "projection_fields": [
                    "case-id",
                    "full-added-and-existing-input-encoding",
                    "exact-success-or-error-tuple-encoding",
                    "full-semantic-verdict-encoding",
                ],
            },
            "artifact_case_ids": [
                "hard-missing",
                "soft-unresolved-exact-duplicate",
                "metadata-distinct-occurrences",
                "one-shot-conflict-forward",
                "one-shot-conflict-reverse",
                "split-new-existing-conflict",
                "existing-existing-non-reclassification",
                "new-existing-same-payload",
                "new-existing-disjoint",
                "new-existing-cross-domain-differential",
            ],
            "compare_against": [
                "frozen-pre-refactor-corpus",
                "current-public-sdk-claim-batch-commit",
            ],
            "cases": [
                "added-versus-existing-claim-classification",
                "new-to-existing-hard-and-soft-references",
                "new-to-existing-functional-conflicts-and-differentials",
                "existing-to-existing-non-reclassification",
                "one-shot-versus-split-batch-parity",
                "canonical-input-permutations",
                "exact-duplicate-deduplication",
                "metadata-distinct-same-content-preservation",
                "hard-and-soft-reference-verdicts",
                "functional-conflict-and-differential-verdicts",
            ],
            "expected_projection": [
                "full-added-and-existing-canonical-input-bytes",
                "full-canonical-claim-batch-v2-bytes",
                "exact-success-or-structured-error-tuple",
                "accepted-added-and-existing-occurrence-sets",
                "reference-conflict-and-differential-verdicts",
                "unresolved-conflict-and-differential-censuses",
            ],
            "production_path_comparison": (
                "required-as-accepted-activation-step-before-production"
            ),
            "corpus_regeneration": (
                "forbidden-without-independent-review-and-new-frozen-digest"
            ),
        },
        "bounded_differential_oracle": (
            "frozen-corpus-plus-independent-current-public-api-required-before-"
            "production"
        ),
        "qualified_zero_side_channel_result": (
            "replay-receipt-instead-of-resident-full-claim-batch-result"
        ),
    },
    "d2_move_only_lifecycle": {
        "operations": ["begin", "consume-task-rvalue", "finalize-rvalue"],
        "task_order": "exact-canonical-next-task-only",
        "live_sealed_results": "at-most-one",
        "consume_effect": "validate-spool-and-destroy-before-return",
        "aggregate_authority": "sealed-source-private-replayable-spools",
        "finalize_precondition": "exact-task-census",
        "final_source": "move-only-replayable-bounded-typed-partition-events",
        "event_codec": DF_0200_PARTITION_EVENT_CODEC["id"],
        "all_task_result_span_or_full_graph": "forbidden",
    },
    "partition_event_codec": DF_0200_PARTITION_EVENT_CODEC,
    "d3_store_ingestion": {
        "bridge": "source-private-non-installed-header-without-catalog-entry",
        "public_state_machine": [
            "created",
            "staged",
            "validating",
            "committed",
            "rejected",
            "rolled_back",
        ],
        "source_events": [
            "partition-begin",
            "claim-occurrence",
            "detached-row",
            "claim-annotation",
            "coverage",
            "unresolved",
            "partition-end",
        ],
        "event_codec": DF_0200_PARTITION_EVENT_CODEC["id"],
        "external_completeness_authority": (
            DF_0200_EXTERNAL_COMPLETENESS_AUTHORITY
        ),
        "independent_validation": {
            "materializer_receipt_is_authority": False,
            "stream_header_or_trailer_is_completeness_authority": False,
            "required_recomputation": [
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
        "private_ingress_collection_overflow": {
            "trigger": "any-canonical-v5-collection-count-greater-than-u64-max",
            "operation": "partition_stage",
            "code": "store.counter-overflow",
            "field": "materialization-v5-collection-count",
            "detail": "",
            "mapping": (
                "store-stage-materialization.store-failure-draft-discarded"
            ),
        },
        "transaction": (
            "one-unpublished-candidate-one-publish-attempt-no-partial-publication"
        ),
        "memory_backend": (
            "exactly-one-canonical-backend-owned-payload-transferred-without-full-copy"
        ),
        "sqlite_prepublication": (
            "sealed-canonical-v5-payload-spool-and-independent-validation"
        ),
        "sqlite_predecessor_v2_capacity": (
            "single-payload-blob-runtime-max-length-1000000000-insufficient-for-"
            "required-limit-adjacent-parity"
        ),
        "sqlite_current_physical_format": (
            "cxxlens.sqlite-semantic-store.v3-3.0.0-bounded-8388608-byte-chunks"
        ),
        "sqlite_publish_and_reopen_qualification": (
            "required-current-v3-plus-v2-read-migration-and-limit-exceeding-parity"
        ),
        "sqlite_capacity_decision_status": "accepted",
        "payload_and_format": (
            "logical-canonical-v5-unchanged-except-authorized-physical-generation-"
            "current-sqlite-v3"
        ),
    },
    "d4_memory_accounting": {
        "metrics": [
            "peak-transient-owned-bytes",
            "memory-backend-final-payload-bytes",
            "sealed-spool-logical-bytes",
            "sqlite-persisted-payload-bytes",
            "operating-system-peak-rss",
        ],
        "transient_formula": (
            "shared-catalog-plus-fixed-buffers-plus-compact-task-index-window-plus-"
            "maximum-of-task-source-output-claim-consume-claim-finalize-store-reopen-"
            "or-report-window"
        ),
        "final_payload_exclusion": (
            "exactly-one-immutable-memory-payload-transferred-unchanged-on-success-only"
        ),
        "spool_and_persisted_storage": (
            "measured-separately-never-hidden-as-zero-residency"
        ),
        "sort_arena_bytes": 8_388_608,
        "record_compare_bytes": 65_536,
        "record_compare_cursors": "exactly-two-cursors-thirty-two-kib-each",
        "merge_fan_in": 16,
        "maximum_merge_file_descriptors": 18,
        "merge_file_descriptor_census": (
            "sixteen-inputs-plus-one-output-plus-one-metadata"
        ),
        "arithmetic": (
            "checked-unsigned-128-before-u64-or-signed-offset-narrowing"
        ),
        "framed_record": {
            "layout": [
                "event-kind-u8",
                "key-length-u64be",
                "payload-length-u64be",
                "key-bytes",
                "payload-bytes",
                "checksum-sha256-32-raw",
            ],
            "logical_bytes_include": "kind-lengths-key-payload-and-checksum",
            "cross_segment_or_spool": "forbidden",
            "preappend_check": (
                "u128-framed-length-segment-spool-and-aggregate-before-any-io"
            ),
        },
        "maximum_spool_logical_bytes": 18_446_744_073_709_551_615,
        "maximum_spool_segment_bytes": 9_223_372_036_854_775_807,
        "maximum_spool_segments_per_spool": 3,
        "maximum_record_bytes": 9_223_372_036_854_775_807,
        "maximum_record_count": 18_446_744_073_709_551_615,
        "segment_offsets": {
            "interval": "half-open-zero-to-segment-length",
            "data_position": "lowest-exact-segment-index-and-offset-within-interval",
            "nonfinal_end": "next-segment-index-zero",
            "final_eof": "segment-index-equals-segment-count-and-offset-zero",
            "empty_nonfinal_segment": "forbidden",
        },
        "rollover": (
            "segment-first-then-next-spool-at-record-boundary-with-no-record-split"
        ),
        "aggregate_census": "checked-u128-across-all-spools-runs-and-merges",
        "oversize_sort_record": (
            "framed-record-over-eight-mib-is-one-streamed-singleton-run"
        ),
        "full_byte_comparison": (
            "two-cursors-total-sixty-four-kib-never-digest-order"
        ),
        "u128_overflow_tests": (
            "checked-operand-overflow-without-constructing-u128-max-plus-one"
        ),
        "semantic_version_component_maximum": 4_294_967_295,
        "canonical_v5_collection_count": {
            "encoding": "u64be",
            "maximum": 18_446_744_073_709_551_615,
            "aggregate_before_narrowing": "checked-u128",
        },
        "legacy_decoder_collection_caps": {
            "one_million": [
                "descriptor-and-string-vectors",
                "manifest-partitions-and-closures",
                "row-and-annotation-relation-groups",
                "unresolved-source-columns",
            ],
            "ten_million": [
                "rows-claims-unresolved-annotations-and-coverage",
                "partition-envelope-claims-coverage-and-unresolved",
            ],
            "authority": (
                "legacy-implementation-guards-not-normative-ceilings-remove-via-"
                "bounded-streaming"
            ),
        },
        "maximum_report_bytes": 1_073_741_824,
        "limit_priority": (
            "proved-record-spool-collection-or-report-limit-before-io-actual-"
            "private-spool-enospc-only-after-proved-in-range"
        ),
        "canonical_order": "exact-full-canonical-bytes-never-digest-order",
    },
    "d5_failure_taxonomy": {
        "stable_spool_failure_cause": (
            "actual-private-prepublication-spool-port-io-or-hash-failure-only"
        ),
        "proposed_prepublication_phase_codes": {
            "materialization-validation": "materialization.spool-failure",
            "store-stage": "materialization.spool-failure",
            "report-construction": "materialization.spool-failure",
        },
        "pre_review_phase_matrix_and_report_schema": "unchanged",
        "accepted_activation_step": (
            "atomically-add-three-private-spool-phase-codes-request-bound-reverse-"
            "closures-for-spool-and-partition-stage-counter-overflow-and-update-"
            "full-report-schema-canonical-json-digest"
        ),
        "relation_or_claim_schema_ceiling_exceeded": (
            "materialization-validation-materialization-claim-invalid-before-store-open"
        ),
        "v5_collection_count_overflow": {
            "operation": "partition_stage",
            "code": "store.counter-overflow",
            "field": "materialization-v5-collection-count",
            "detail": "",
            "phase_and_code": "store-stage-materialization.store-failure",
            "effect": "draft-discarded-publication-not-attempted",
        },
        "sqlite_writer_publish_enospc_or_sqlite_toobig": {
            "operation": "writer_publish",
            "code": "store.sqlite-failure",
            "field": "database",
            "detail": "opaque",
            "outcome": "publication_outcome_unknown",
        },
        "successful_receipt_contradiction": "source-private-exit-two-zero-stdout",
        "semantic_claim_failure": (
            "existing-materialization-claim-invalid-or-coverage-code"
        ),
        "report_over_limit": (
            "report-construction-materialization-report-invalid-if-compact-completable"
        ),
        "actual_private_spool_enospc": (
            "phase-authentic-materialization.spool-failure-only-before-publication"
        ),
        "publish_returned_handle_then_verification_failure": (
            "committed_unverified-detailed-response-when-safely-constructible"
        ),
        "response_unsafe_exit_two": [
            "spool-or-allocation-failure-preventing-safe-response",
            "report-construction-or-transport-failure-preventing-safe-response",
            "successful-receipt-contradiction",
            "checked-arithmetic-contradiction",
        ],
        "post_publish_typed_outcome_reclassification": "forbidden",
        "new_materialization_resource_or_internal_stable_code": "forbidden",
        "sqlite_v2_migration_required": {
            "store_tuple": {
                "operation": "writer_begin",
                "code": "store.migration-required",
                "field": "sqlite-physical-format",
                "detail": "cxxlens.sqlite-semantic-store.v2-to-v3",
            },
            "phase_and_code": "store-stage-materialization.store-failure",
            "effect": (
                "logical-draft-discarded-publication-not-attempted-"
                "committed-transaction-count-zero"
            ),
            "materializer_implicit_migration": "forbidden",
        },
    },
    "sqlite_capacity_decision": DF_0200_SQLITE_CAPACITY_DECISION,
    "d6_compatibility": {
        "contract_version": "2.1.0-accepted-df-0200-option-a-authority",
        "report_schema_canonical_json_digest": (
            EXPECTED_REPORT_SCHEMA_CANONICAL_DIGEST
        ),
        "public_headers_signatures_and_inventory": "unchanged",
        "public_catalog": (
            "additive-sqlite-v3-behavior-entry-with-store.migration-required-"
            "no-new-callable"
        ),
        "request_and_report_shape": DF_0200_REPORT_SHAPE_ACTIVATION,
        "claim_store_identities_and_logical_canonical_schema_projection": (
            "unchanged-with-authorized-physical-generation-transition"
        ),
        "sqlite_contract": "schemas/cxxlens_ng_sqlite_store_contract.yaml",
        "sqlite_physical_format": "cxxlens.sqlite-semantic-store.v3-3.0.0",
        "sqlite_chunk_profile": (
            "cxxlens.sqlite-payload-chunks.v1-8388608-byte-maximum-"
            "16777216-runtime-floor"
        ),
        "sqlite_predecessor": "exact-v2.6.0-read-only-direct-open",
        "sqlite_migration": "compact-v2.6.0-to-v3.0.0-single-transaction-cow",
        "sqlite_capacity_decision_status": "accepted",
        "source_private_receipts_and_counters": "excluded-from-public-report-and-api",
        "external_qualified_v2_1_found": (
            "require-successor-version-and-migration-boundary"
        ),
        "fresh_public_semantics_gate": [
            "request-accepted-set-or-budget",
            "retained-memory-guarantee-weakening",
            "public-claim-or-store-signature-success-semantics-or-cursor-lifetime",
            "public-header-callable-or-catalog-entry",
            "report-shape-or-version",
            "canonical-v5-or-snapshot-identity",
            "incompatible-sqlite-format-or-migration-beyond-adr-0097",
        ],
    },
    "falsification": [
        "frozen-pre-refactor-and-current-public-claim-batch-byte-verdict-corpus",
        "added-existing-and-new-existing-reference-conflict-nonreclassification",
        "one-shot-split-and-input-permutation-parity",
        "exact-duplicate-versus-metadata-distinct-occurrence-preservation",
        "cross-task-run-and-partition-hard-soft-reference-and-conflict-matrix",
        "event-codec-unknown-missing-reordered-truncated-and-checksum-matrix",
        "external-census-digest-and-whole-partition-drop-rejection",
        "segment-run-merge-manifest-and-byte-record-seal-receipt-matrix",
        "exact-eight-field-and-zero-row-partition-matrix",
        "four-thousand-ninety-six-task-one-live-seal-and-permutation-evidence",
        "every-private-spool-operation-phase-code-effect-fault-injection",
        "partition-stage-u64-collection-overflow-exact-sdk-tuple",
        "checked-u64-u128-framing-segment-offset-fan-in-and-report-boundaries",
        "sqlite-writer-publish-unknown-and-committed-unverified-preservation",
        "single-owner-memory-payload-and-forbidden-all-task-graph-measurement",
        "sqlite-option-a-v3-v2-read-migration-and-limit-exceeding-parity",
        "memory-sqlite-static-shared-semantic-and-public-api-parity",
    ],
}
EXPECTED_BASE_CLAIM_CONTRACT = {
    "owner": "installed-tool",
    "descriptor_order": BASE_DESCRIPTOR_IDS,
    "topology": "exact-hard-reference-topological-order",
    "worker_output_overlap": "forbidden",
    "staging": (
        "same-unpublished-single-transaction-before-worker-hard-reference-validation"
    ),
    "row_authority": {
        "build.project.v1": "exact-project-request-payload",
        "build.toolchain_context.v1": "exact-task-toolchain-payload",
        "build.variant.v1": "exact-task-variant-payload",
        "source.file.v1": "exact-task-source-payload-and-decoded-bytes",
        "build.compile_unit.v1": (
            "exact-task-build-payload-normalized-invocation-maps-to-effective-invocation"
        ),
        "source.span.v1": (
            "independently-validated-worker-full-span-bundle-v2-with-origin-explicitly-absent"
        ),
    },
    "envelope_construction": {
        "condition_fragment": "exact-originating-task-condition-universe-and-condition",
        "interpretation_domain": "exact-originating-task-interpretation-domain",
        "row_envelope_binding": (
            "exact-row-identity-and-row-digest-to-sorted-origin-associations"
        ),
        "semantic_task_context": [
            "provider-task-id",
            "task-input-digest",
            "selected-catalog-compile-unit-id",
            "final-relation-compile-unit-id",
            "condition-universe-id",
            "condition-id",
            "interpretation-domain",
        ],
        "physical_provider_execution": "excluded",
        "source_span_bundle_edge": (
            "exact-validated-bundle-observation-row-and-originating-semantic-task-"
            "association"
        ),
        "producer_identity": (
            "exact-tool-semantic-identity-excluding-package-configuration-binary-and-prefix"
        ),
        "physical_producer_occurrence": (
            "exact-report-installation-provider-and-authority-binding-outside-semantic-envelope"
        ),
        "provenance": "exact-request-row-or-validated-span-bundle-to-claim-edge",
        "evidence": "exact-catalog-source-toolchain-or-provider-observation-edge",
        "guarantee": "exact-report-guarantee-digest",
        "caller_authored_envelope": "forbidden",
    },
    "validation": [
        "recompute-exact-registry-domain-identities",
        "reject-conflicting-duplicate-base-rows",
        "validate-hard-references-in-descriptor-order",
        "recompute-row-envelope-and-source-bundle-task-binding-sets",
        "bind-row-envelope-and-edge-set-digests",
        "seal-before-worker-output-adoption",
    ],
}
EXPECTED_SOURCE_IDENTITY_CONTRACT = {
    "logical_path": {
        "contract_version": "cxxlens.logical-path.v1",
        "accepted_domain": "project",
        "request_form": "project-uri-with-normalized-nonempty-posix-relative-path",
        "normalization": (
            "reject-empty-dot-dotdot-empty-segment-backslash-query-fragment-and-non-nfc"
        ),
        "file_id": (
            "canonical-file-domain-identity-over-domain-relative-path-and-contract-version"
        ),
    },
    "line_index": {
        "contract_version": "cxxlens.byte-line-index.v1",
        "authority": "decoded-source-bytes",
        "offsets": "zero-and-byte-after-each-lf-including-eof",
        "identity": (
            "canonical-line-index-domain-identity-over-contract-content-size-and-offsets"
        ),
    },
    "base64": {
        "encoding": "rfc4648-standard-alphabet-with-required-padding",
        "canonicality": "zero-discarded-padding-bits",
        "raw_json_token_escape_spelling": (
            "non-authoritative-after-strict-string-decode"
        ),
        "decoded_string": (
            "exactly-one-canonical-spelling-per-decoded-byte-sequence"
        ),
        "request_schema_rejection": "before-derived-identity-and-binding",
        "source_authority": (
            "decoded-bytes-count-content-digest-and-line-index"
        ),
        "task_v3_projection": (
            "unique-canonical-base64-derived-from-sealed-source-bytes"
        ),
        "validation": "decode-reencode-and-exact-compare",
    },
    "validation": "recompute-before-source-file-and-task-adoption",
}
EXPECTED_INSTALLED_OCCURRENCE = {
    "schema": "cxxlens.clang22-materializer-occurrence-manifest.v1",
    "fixed_path": OCCURRENCE_MANIFEST_PATH,
    "request_authority": "expected-exact-manifest-file-digest",
    "manifest_payload": {
        "fields": [
            "schema",
            "manifest_version",
            "source_revision",
            "source_tree",
            "package_configuration",
            "files",
            "occurrence_payload_digest",
        ],
        "files": (
            "configuration-closed-exact-ordered-role-prefix-relative-path-and-"
            "sha256-digest-records"
        ),
        "configuration_inventories": {
            "static": {
                "count": 13,
                "ordered_roles": [
                    "materializer-executable",
                    "worker-executable",
                    *[role for role, _, _ in OCCURRENCE_AUTHORITY_FILES],
                ],
            },
            "shared": {
                "count": 19,
                "ordered_roles": [
                    "materializer-executable",
                    "worker-executable",
                    *[role for role, _, _ in OCCURRENCE_AUTHORITY_FILES],
                    *[role for role, _ in SHARED_OCCURRENCE_RUNTIME_FILES],
                ],
                "package_owned_runtime_dso_paths": (
                    "safe-prefix-relative-lib-or-lib64-soname"
                ),
            },
        },
        "external_system_libraries": (
            "excluded-toolchain-and-runtime-evidence-only"
        ),
        "self_entry": "forbidden",
        "digest": (
            "sha256-of-utf8-canonical-json-sorted-keys-no-whitespace-exact-"
            "manifest-fields-excluding-occurrence_payload_digest"
        ),
        "inventory_digest": (
            "sha256-of-utf8-canonical-json-sorted-keys-no-whitespace-ordered-"
            "role-path-digest-records"
        ),
    },
    "runtime_measurement": {
        "executable_object": "proc-self-exe-opened-regular-not-deleted",
        "prefix_derivation": (
            "exact-kernel-executable-object-at-bin-cxxlens-clang22-materialize"
        ),
        "lookup": (
            "prefix-dirfd-openat2-resolve-beneath-no-symlinks-no-magiclinks"
        ),
        "file_digest": "stable-before-after-stat-over-opened-fd",
        "role_snapshot": {
            "cardinality": "exactly-one-private-snapshot-per-closed-manifest-role",
            "construction": (
                "copy-and-sha256-same-verified-stream-during-measurement"
            ),
            "closure": (
                "source-before-after-stable-identity-plus-authority-digest-plus-"
                "snapshot-size-and-digest"
            ),
            "sealing": (
                "memfd-write-grow-shrink-and-further-seal-additions-"
                "irreversibly-forbidden"
            ),
            "retained_authority": (
                "sealed-snapshot-not-installed-path-or-mutable-inode"
            ),
            "consumption": (
                "independent-read-only-handle-to-same-sealed-inode-without-byte-recopy"
            ),
            "postmeasurement-path-or-inode-mutation": "cannot-change-consumed-bytes",
            "unsupported-or-copy-write-seal-failure": (
                "reject-before-worker-or-store-effect"
            ),
        },
        "before_worker_or_store_effect": "required",
        "argv0_or_path_authority": "forbidden",
    },
    "trust_claim": "measured-self-consistency-and-invocation-attribution-only",
    "external_trust_witness": (
        "full-prefix-install-artifact-manifest-including-occurrence-manifest-bytes"
    ),
    "external-full-prefix-digest-in-request-or-semantic-identity": "forbidden",
}
EXPECTED_COVERAGE_CONTRACT = {
    "record_type": "typed-coverage-unit",
    "complete_worker_record_set_per_task": {
        "transport": [
            {
                "kind": TRANSPORT_COVERAGE_KIND,
                "id": "exact-provider-task-id",
                "state": "covered",
                "reason": "empty",
            }
        ],
        "semantic_kinds": SEMANTIC_COVERAGE_KINDS,
        "semantic_id": "exact-provider-task-id",
        "qualified_semantic_state_and_reason": "covered-and-empty",
        "canonical_order": "kind-id-state-reason",
        "missing-duplicate-extra-renamed-wrong-task-or-state": "reject",
    },
    "generic_validator": "transport-required-specialization-blind-and-lossless",
    "specialization_seal": (
        "exact-three-semantic-records-plus-retained-transport-record"
    ),
    "report_planes": ["transport", "semantic"],
    "global_transport_count": "exact-task-count",
    "global_semantic_count": "exact-three-times-task-count",
    "filtering-or-plane-substitution": "forbidden",
    "balance": "requested-semantic-fragments-exact-partition",
    "empty-success": "forbidden",
}
EXPECTED_SQLITE_EFFECT_ROOT = {
    "capture": (
        "startup-current-working-directory-opened-once-before-request-parsing"
    ),
    "request_path": (
        "canonical-relative-utf8-no-empty-dot-dotdot-root-drive-nul-backslash-or-"
        "normalization-change"
    ),
    "sqlite_name_binding": (
        "synthetic-rooted-prefix-before-sqlite-uri-parsing-with-colon-and-file-uri-"
        "rejected-before-effect"
    ),
    "implementation": "source-private-rooted-sqlite-vfs",
    "registration": (
        "named-non-default-process-local-exclusive-through-store-connection-lifetime"
    ),
    "store_backend_lifetime": (
        "type-erased-token-retained-through-complete-sqlite-connection-destruction"
    ),
    "store_backend_lifetime_bridge": (
        "source-private-dso-linkage-access-class-with-no-installed-callable-signature-or-"
        "supported-external-abi"
    ),
    "sqlite_open_handle_lifetime": (
        "stack-raii-from-open-return-through-diagnostic-and-heap-owner-transfer"
    ),
    "destruction_order": (
        "sqlite-connection-close-before-vfs-unregister-root-fd-close-and-library-release"
    ),
    "active_database_authority": (
        "exact-main-database-path-refcount-new-authority-revoked-on-last-close-entry-"
        "erased-after-pending-and-leases-drain"
    ),
    "active_authority_lease": (
        "check-and-filesystem-effect-serialized-through-active-authority"
    ),
    "opened_sidecar_io_authority": {
        "reacquire-active-base-before": [
            "read",
            "write",
            "truncate",
            "sync",
            "file-size",
            "lock",
            "unlock",
            "check-reserved-lock",
            "file-control",
            "shm-lock",
        ],
        "main-database-only": ["shm-map", "shm-unmap"],
        "no-effect-advisory": [
            "sector-size",
            "device-characteristics",
            "shm-barrier",
            "fetch",
            "unfetch",
        ],
        "close": "always-release-owned-resources",
    },
    "opener_preallocation": (
        "synthetic-name-exact-path-state-and-store-receipt-before-sdk-or-sqlite-effect"
    ),
    "main_database_open_order": (
        "pending-authority-reservation-before-rooted-create-then-noexcept-active-commit"
    ),
    "named_file_namespace": (
        "exact-main-database-plus-known-derived-wal-shm-and-journal-only"
    ),
    "named_xopen_roles": (
        "exact-main-database-main-journal-and-write-ahead-log-only"
    ),
    "named_delete_on_close": (
        "rejected-before-file-effect-anonymous-transients-are-private-memfd-only"
    ),
    "anonymous_xopen_roles": (
        "exact-temporary-database-transient-database-temporary-journal-and-subjournal-only"
    ),
    "xopen_failure_state": (
        "methods-null-and-output-flags-zero-until-success"
    ),
    "c_abi_exception_boundary": {
        "applies_to": "every-vfs-and-io-method-table-callback",
        "exception_escape": "forbidden",
        "construction-and-owned-resource-failure": (
            "callback-specific-typed-sqlite-result-with-no-ownership-leak"
        ),
        "partial-handle-state": "may-remain-owned-for-later-close-or-unmap",
        "initialized_failure_outputs": {
            "xopen": ["p-methods-null", "output-flags-zero"],
            "xaccess": "exists-zero",
            "xfull-pathname": "first-byte-nul",
            "xshm-map": "pointer-null",
            "xfetch": "pointer-null",
            "has-moved": "moved-one",
            "all-other-error-outputs": "unspecified",
        },
        "already-authenticated-filesystem-effects": (
            "nontransactional-callback-specific"
        ),
        "has-moved-allocation-or-reauthentication-failure": (
            "sqlite-ok-with-moved-one"
        ),
    },
    "shared_memory_callbacks": "active-authenticated-main-database-handle-only",
    "access_and_delete": (
        "authenticated-active-main-database-or-known-sidecar-of-active-main-"
        "database-only"
    ),
    "callback_capability_surface": (
        "dynamic-library-loading-and-system-call-replacement-or-discovery-disabled"
    ),
    "moved_detection": {
        "identity": (
            "opened-main-database-device-inode-compared-with-current-rooted-path"
        ),
        "moved_cases": ["missing", "rename", "replacement", "reauthentication-failure"],
    },
    "delete_authority": {
        "namespace": "authenticated-known-path-beneath-captured-root",
        "regular_observation": "required-immediately-before-path-unlink",
        "concurrent-rebind-exact-object-guarantee": "not-claimed",
    },
    "regular_file_effects": (
        "named-open-and-access-require-nonblocking-regular-file-verdict"
    ),
    "resolution": (
        "openat2-beneath-no-symlinks-no-magiclinks-for-database-wal-shm-and-journal"
    ),
    "resolution_authority": {
        "parent": (
            "captured-root-duplicate-or-descendant-authenticated-beneath-at-acquisition"
        ),
        "leaf": "relative-to-retained-authenticated-parent-directory-capability",
        "concurrent_ancestor_rename_or_mount_relocation": {
            "capability_revocation": "not-implied",
            "effect_time_current_namespace_beneath_root": "not-claimed",
        },
    },
    "unsupported-kernel-or-vfs": "fail-before-store-effect",
    "operational_receipt": [
        "rooted-vfs-v1",
        "mount-device-inode-observation-digest",
        "exact-relative-path",
        "parent-and-leaf-resolution-verdict",
    ],
    "semantic-snapshot-claim-backend-parity-identity": "excluded",
}
EXPECTED_SDK_PUBLISH_MAPPING = {
    "classification_key": [
        "authenticated-operation",
        "backend",
        "exact-sdk-code",
        "exact-sdk-field",
        "authority-declared-stable-detail",
    ],
    "diagnostic-prose-parsing": "forbidden",
    "prepublication_operations": [
        "configuration",
        "store_open",
        "head_current",
        "writer_begin",
        "partition_stage",
        "closure_stage",
        "writer_validate",
    ],
    "prepublication_failure": (
        "compact-phase-authentic-publication-not-attempted-zero-commit"
    ),
    "writer_publish_sqlite": {
        "store.publication-conflict": {
            "field": "exact-series-id",
            "detail": "empty",
            "outcome": "rejected_stale",
        },
        "store.counter-overflow": {
            "fields": ["publication_sequence", "physical_generation"],
            "detail": "empty",
            "outcome": "rejected_store_failure-counter_overflow",
        },
        "store.hash-collision": {
            "field": "exact-candidate-snapshot-id",
            "detail": "empty",
            "outcome": "rejected_store_failure-hash_collision",
        },
        "store.snapshot-ambiguous": {
            "field": "exact-snapshot-id",
            "detail": "empty",
            "outcome": "rejected_store_failure-persistence-corrupt",
        },
        "store.sqlite-failure": {
            "field": "database",
            "detail": "opaque",
            "outcome": "publication_outcome_unknown-persistence_io",
        },
        "store.corrupt": {
            "exact_fields_and_details": {
                "sqlite": [
                    "backend",
                    "column-count",
                    "publication-row",
                    "series-head-count",
                    "series-head",
                    "series-head-sequence",
                ],
                "exact-publication-id": [
                    "authority-record",
                    "duplicate-publication-id",
                    "parent",
                    "parent-sequence",
                ],
                "exact-series-id": [
                    "duplicate-sequence",
                    "series-roots",
                    "series-head-cas",
                ],
            },
            "outcome": "rejected_store_failure-persistence-corrupt",
        },
    },
    "publish_returned_handle": "exact-returned-record-is-commit-proof",
    "postpublish_operations": [
        "store_reopen",
        "verify_current",
        "verify_open_publication",
        "verify_open_snapshot",
        "verify_projection",
    ],
    "postpublish-error-or-mismatch": (
        "committed_unverified-preserve-first-cause"
    ),
    "invariant_breach_exit_two": [
        "store.transaction-state/publish/empty",
        "store.corrupt/publication/identity",
        "store.publish-stale-parent",
        "any-memory-backend-publish-error",
        "every-unlisted-writer-publish-tuple",
    ],
    "recovery-observation-reclassification": "forbidden",
}
RECOMPUTED_IDS_AND_DIGESTS = [
    "materialization_request_id",
    "authority_registry_digest",
    "engine_registry_digest",
    "engine_generation_id",
    "interpretation_policy_digest",
    "trust_policy_digest",
    "snapshot_series_id",
    "project_id",
    "catalog_id",
    "catalog_digest",
    "catalog_compile_unit_census_digest",
    "compile_unit_id",
    "source_snapshot_id",
    "file_id",
    "line_index_id",
    "source_content_digest",
    "build_variant_id",
    "toolchain_context_id",
    "normalized_invocation_digest",
    "task_input_digest",
    "provider_task_id",
    "provider_condition_ref_id",
    "provider_execution_id",
    "batch_id",
    "source_span_id",
    "observation_payload_digest",
    "clang22_observation_id",
    "semantic_key_id",
    "assertion_id",
    "claim_content_digest",
    "producer_input_basis_digest",
    "partition_id",
    "partition_claim_set_digest",
    "partition_coverage_digest",
    "partition_content_digest",
    "snapshot_id",
    "publication_id",
]
CROSS_BOUND_CALLER_AUTHORITY = [
    "selected_catalog_compile_unit_id",
    "environment_digest",
]
CATALOG_COMPILE_UNIT_FIELDS = (
    "catalog_compile_unit_id",
    "effective_invocation_digest",
    "source_digest",
    "environment_digest",
)
TASK_EXECUTION_KEY_FIELDS = (
    "provider_task_id",
    "task_input_digest",
    "provider_execution_id",
)
EXPECTED_PORTABLE_SPECIALIZATION = {
    "reference_kind": "reverse-specialization-index",
    "dependency": False,
    "authority": CONTRACT.as_posix(),
    "task_owner": "installed-provider-owned-tool",
    "public_cpp_task_builder": "forbidden",
    "requested_outputs": "exact-contract-six",
    "dependency_groups": "exact-canonical-and-observation-both-mandatory",
    "validated_result": "tool-private-immutable-sealed-value",
    "public_process_frames": "diagnostic-only",
    "partial_adoption": "forbidden",
    "condition_binding": "exact-materializer-derived-universe-and-condition-ref",
}
EXPECTED_PROTOCOL_ADOPTION_BOUNDARY = {
    "transcript_validation": "necessary-not-sufficient-for-adoption",
    "public_execution_report_frames": "diagnostic-only",
    "adoption_input": "immutable-sealed-decoded-groups-from-the-shared-validator",
    "group_inventory": "exact-task-declared-and-complete",
    "base_hard_references": "independently-validated-before-group-adoption",
    "partial_policy": "explicit-at-task-authority-never-inferred",
    "clang22_installed_reference_kind": "reverse-specialization-index",
    "clang22_installed_dependency": False,
    "clang22_installed_authority": CONTRACT.as_posix(),
    "raw_frame_redecode_or_private_codec_duplication": "forbidden",
}
EXPECTED_RUNTIME_ADOPTION_PROJECTION = {
    "validated_frames": "diagnostic-only-non-authoritative",
    "sealed_value": "tool-private-immutable-complete-task-result",
    "reconstruction_from_public_frames": "forbidden",
    "specialization_reference_kind": "reverse-specialization-index",
    "specialization_dependency": False,
    "publication": "installed-materializer-contract-only",
}
EXPECTED_RUNTIME_INSTALLED_MATERIALIZATION = {
    "tool": "cxxlens-clang22-materialize",
    "reference_kind": "reverse-specialization-index",
    "dependency": False,
    "authority": CONTRACT.as_posix(),
    "public_cpp_bridge": "forbidden",
    "exact_outputs": list(DESCRIPTOR_IDS),
    "dependency_groups": ["canonical", "observation"],
    "partial_adoption": "forbidden",
    "source_less_observation": (
        "retain-with-typed-unresolved-and-non-exact-guarantee"
    ),
    "canonical_call_site_without_complete_primary_span": "reject-transaction",
}
GROUP_DESCRIPTORS = {
    "canonical": DESCRIPTOR_IDS[:3],
    "observation": DESCRIPTOR_IDS[3:],
}
DESCRIPTOR_GROUP = {
    descriptor: group
    for group, descriptors in GROUP_DESCRIPTORS.items()
    for descriptor in descriptors
}
DESCRIPTOR_STAGE = {
    descriptor: "canonical_claim" if descriptor.startswith("cc.") else "assertion"
    for descriptor in DESCRIPTOR_IDS
}
PRIMARY_SPAN_BUNDLE_FIELDS = [
    "span_id",
    "snapshot",
    "file",
    "begin",
    "end",
    "role",
    "read_only",
]
PRIMARY_SPAN_ABSENCE_CATEGORY = "missing_primary_span_bundle"
SPAN_REGISTRY_COLUMN_SHAPES = {
    "span_id": ("source", "optional<typed_id<source_span_id>>"),
    "snapshot": ("source_snapshot", "optional<typed_id<source_snapshot_id>>"),
    "file": ("source_file", "optional<typed_id<file_id>>"),
    "begin": ("source_begin", "optional<uint64>"),
    "end": ("source_end", "optional<uint64>"),
    "role": ("source_role", "optional<open_symbol<source.range-role/1>>"),
    "read_only": ("source_read_only", "optional<bool>"),
}
SPAN_OBSERVATION_DESCRIPTORS = [
    "frontend.clang22.entity_observation.v2",
    "frontend.clang22.call_observation.v2",
]
SPAN_REGISTRY_COLUMN_MAPPING = {
    descriptor: {
        abstract: f"{descriptor}.{column_name}"
        for abstract, (column_name, _) in SPAN_REGISTRY_COLUMN_SHAPES.items()
    }
    for descriptor in SPAN_OBSERVATION_DESCRIPTORS
}
SPAN_ORIGIN_COLUMN_MAPPING = {
    descriptor: f"{descriptor}.source_origin_chain"
    for descriptor in SPAN_OBSERVATION_DESCRIPTORS
}
OBSERVATION_KIND_TO_DESCRIPTOR = {
    "call": "frontend.clang22.call_observation.v2",
    "entity": "frontend.clang22.entity_observation.v2",
    "type": "frontend.clang22.type_observation.v2",
}
EXPECTED_OBSERVATION_V2_NATIVE_CODEC = {
    "codec": "cxxlens.clang22.observation-native.v2",
    "canonical_binary": "cxxlens-canonical-tuple-v1",
    "canonical_integer_domain": "signed-int64",
    "kind_to_descriptor": OBSERVATION_KIND_TO_DESCRIPTOR,
    "native_record": {
        "exact_fields": [
            "kind",
            "final_relation_compile_unit_id",
            "semantic_key",
            "payload",
            "primary_span",
            "origin_chain",
            "exact_equivalence",
            "limitation",
        ],
        "authority": "validated-provider-native-detached-value-before-row-construction",
        "unknown_or_missing_field": "reject",
    },
    "semantic_key": {
        "input": "nonempty-strict-utf8-native-string",
        "row_encoding": "exact-utf8-bytes",
        "normalization": "none",
        "hash_or_v1_alias": "forbidden",
    },
    "payload": {
        "input": "unique-map-of-nonempty-strict-utf8-key-to-strict-utf8-value",
        "key_order": "strict-utf8-byte-lexicographic",
        "entry_projection": ["key-as-utf8-string", "value-as-utf8-string"],
        "projection": [
            "cxxlens.clang22.observation-payload.v2",
            "relation-descriptor-id",
            "canonical-ordered-entry-tuples",
        ],
        "digest": "semantic-digest-v2",
        "digest_domain": "cxxlens.clang22.observation-payload.v2",
        "row_encoding": "canonical-semantic-v2-digest-string",
    },
    "primary_span": {
        "entity_and_call": "exact-seven-field-bundle-or-absent",
        "type": "absent-only",
        "row_mapping": [
            "source",
            "source_snapshot",
            "source_file",
            "source_begin",
            "source_end",
            "source_role",
            "source_read_only",
        ],
    },
    "origin_chain": {
        "input_order": "immediate-expansion-to-outermost",
        "entry_projection": [
            "kind-as-utf8-string",
            "logical-path-as-utf8-string",
            "begin-as-signed-int64",
            "end-as-signed-int64",
            "read-only-as-boolean",
        ],
        "projection": [
            "cxxlens.clang22.source-origin-chain.v2",
            "ordered-entry-tuples",
        ],
        "empty": "optional-column-absent",
        "nonempty_row_encoding": "exact-canonical-binary-bytes",
        "duplicate_entries": "preserve",
        "path_normalization": "none",
        "entry_validation": (
            "nonempty-kind-and-path-valid-utf8-zero-to-int64max-half-open-and-read-only-true"
        ),
        "type": "empty-only",
    },
    "row": {
        "compile_unit": "exact-current-task-final-relation-compile-unit-id",
        "exact_equivalence": "exact-native-boolean",
        "limitation": "absent-or-nonempty-strict-utf8",
        "exact_limitation_coupling": (
            "true-requires-absent-and-false-requires-nonempty-limitation"
        ),
        "result_identity": (
            "registry-declared-domain-projection-via-sdk-derive-domain-identity"
        ),
        "validation": (
            "shared-row-builder-plus-sdk-validate-domain-identity-before-seal"
        ),
    },
    "v1_bytes_or_digest": "reject-without-reconstruction-from-native-v2-fields",
}
EXPECTED_EFFECTIVE_INVOCATION_CODEC = {
    "codec": "cxxlens.clang22.effective-invocation.v1",
    "canonical_binary": "cxxlens-canonical-tuple-v1",
    "projection": [
        "contract-tag",
        "working-directory",
        "ordered-effective-argv",
    ],
    "argv_semantics": {
        "argv0": "preserve",
        "order": "preserve",
        "duplicates": "preserve",
        "shell_parsing": "forbidden",
        "sorting_or_deduplication": "forbidden",
    },
    "digest": "semantic-digest-v2-in-exact-domain",
    "cross_binding": [
        "task-normalized-invocation-digest",
        "selected-catalog-effective-invocation-digest",
        "final-build-compile-unit-effective-invocation-digest",
    ],
    "validation_order": (
        "recompute-before-project-catalog-final-compile-unit-task-and-request-identities"
    ),
}
EXPECTED_REPORT_DIGEST_CHAIN = {
    "leaf_authority": (
        "shared-validator-sealed-decoded-rows-chunks-provenance-and-typed-side-channels"
    ),
    "report_self_consistency_is_adoption_authority": False,
    "batch": {
        "domain": "cxxlens.clang22-batch-result.v1",
        "leaves": [
            "ordered-chunk-set",
            "row-binding-set",
            "exact-worker-assertion-content-set",
            "exact-worker-assertion-full-occurrence-set",
            "provenance-edge-set",
            "observation-equivalence-census-when-observation",
        ],
        "task_binding": (
            "exact-composite-execution-key-plus-semantic-row-origin-and-final-relation-"
            "compile-unit"
        ),
        "row_binding_fields": [
            "row-digest",
            "semantic-originating-task",
            "final-relation-compile-unit-id",
            "primary-span-bundle-digest-or-absent",
            "exact-equivalence-or-absent",
            "limitation-digest-or-absent",
        ],
        "observation_census_binding": (
            "exact-row-digest-origin-final-exactness-and-limitation-digest"
        ),
        "empty_batch_normalization": (
            "zero-row-zero-ordered-chunks-zero-row-bindings-zero-provenance-edges"
        ),
    },
    "group": {
        "domain": "cxxlens.clang22-group-batch-set.v1",
        "order": "descriptor-contract-order",
    },
    "task": {
        "side_channel_domain": "cxxlens.clang22-task-side-channels.v1",
        "result_domain": "cxxlens.clang22-task-result.v1",
        "result_set_domain": "cxxlens.clang22-task-result-set.v1",
        "raw_frame_set_domain": "cxxlens.clang22-raw-frame-set.v1",
        "transcript_receipt": {
            "raw_frame_stream": (
                "exact-provider-stdout-byte-count-and-sha256-before-decode-or-move"
            ),
            "expected_provider_identity": {
                "exact_fields": [
                    "provider_id",
                    "provider_version",
                    "provider_binary_digest",
                    "provider_semantic_contract_digest",
                    "protocol_major",
                    "protocol_minor",
                    "required_features",
                    "sandbox_policy_digest",
                    "offered_relations",
                ],
                "source": (
                    "exact-request-worker-identity-negotiated-session-measured-worker-"
                    "binary-and-authorized-descriptor-order"
                ),
                "report_cross_binding": (
                    "provider-report-plus-measured-worker-occurrence-plus-registry-"
                    "descriptor-set"
                ),
                "provider-output-self-consistency": "forbidden",
            },
            "frame_transcript_domain": "cxxlens.provider-frame-transcript.v2",
            "sealed_transcript_domain": "cxxlens.provider-sealed-transcript.v1",
            "sealed_transcript_exact_fields": [
                "task_id",
                "terminal",
                "batches",
                "coverage_records",
                "unresolved_records",
                "evidence_records",
            ],
            "sealed_transcript_exact_batch_fields": [
                "task_id",
                "descriptor_id",
                "descriptor_digest",
                "dependency_group_id",
                "atomic_output_group_id",
                "batch_id",
                "batch_digest",
                "ordered_chunk_digests",
                "row_canonical_forms",
            ],
            "sealed_transcript_exact_coverage_fields": [
                "kind",
                "id",
                "state",
                "reason",
            ],
            "sealed_transcript_exact_unresolved_fields": [
                "code",
                "subject",
                "detail",
            ],
            "sealed_transcript_exact_evidence_fields": [
                "kind",
                "subject",
                "producer",
                "summary",
            ],
            "sealed_transcript_digest_projection": [
                "task-id",
                "terminal",
                "batches",
                "full-flat-coverage-records",
                "full-unresolved-records",
                "full-evidence-records",
            ],
            "materialization-specialization-projection": "none",
            "construction": (
                "same-shared-validation-pass-that-constructs-immutable-seal"
            ),
            "public-process-semantic-digest-alias": "forbidden",
        },
        "input_transfer_receipt": {
            "protocol": "1.1.0-task-input-chunks-v1",
            "fields": [
                "task-input-codec",
                "logical-byte-count",
                "logical-task-input-digest",
                "canonical-chunk-size",
                "chunk-count",
                "ordered-chunk-payload-digest-set-digest",
            ],
            "raw-host-frames-authorize-task-or-adoption": False,
        },
    },
    "global_side_channels": {
        "domains": [
            "cxxlens.clang22-global-transport-coverage.v1",
            "cxxlens.clang22-global-coverage.v1",
            "cxxlens.clang22-global-unresolved.v1",
            "cxxlens.clang22-global-evidence.v1",
        ],
        "transport-versus-semantic-coverage": (
            "separate-recomputable-record-planes"
        ),
        "task_order": "semantic-task-key-byte-order",
        "physical_provider_execution": "excluded",
    },
    "guarantee": {
        "domain": "cxxlens.clang22-materialization-guarantee.v1",
        "profile_domain": GUARANTEE_PROFILE_ID,
        "inputs": [
            "exact-profile-id-and-digest",
            "global-side-channel-digests",
            "task-guarantee-fragments",
            "canonical-three-observation-descriptor-censuses",
        ],
        "base-or-claim-stage-back-edge": "forbidden",
    },
    "claim_stage": {
        "content_domain": "cxxlens.clang22-claim-stage-content-set.v1",
        "sdk_occurrence_domain": (
            "cxxlens.clang22-claim-stage-sdk-occurrence-set.v1"
        ),
        "origin_association_domain": (
            "cxxlens.clang22-claim-stage-origin-association-set.v1"
        ),
        "provenance_domain": "cxxlens.clang22-claim-stage-provenance-set.v1",
        "stage_domain": "cxxlens.clang22-claim-stage.v1",
    },
    "global_provenance": {
        "domain": "cxxlens.clang22-global-provenance.v1",
        "inputs": "canonical-three-claim-stage-summaries",
    },
    "store_identity": {
        "inputs": [
            "claim-rows",
            "full-sdk-claim-envelopes",
            "canonicalization-edges",
            "origin-associations",
            "complete-final-claim-batch",
            "exact-eight-field-partitions",
            "coverage-and-unresolved",
            "snapshot-manifest",
            "publication-record",
            "reopened-store-state",
        ],
        "opaque-fixture-ids-or-recomputed-booleans": "forbidden",
    },
    "reopened_store": {
        "descriptor_inventory": (
            "exact-twelve-id-runtime-digest-pairs-from-handle"
        ),
        "required_open_paths": [
            "current-selector",
            "open-publication",
            "open-snapshot",
        ],
        "fixed_lookup_inputs": {
            "current-selector": "exact-full-selector-and-series-id",
            "open-publication": "exact-candidate-publication-id",
            "open-snapshot": "exact-candidate-snapshot-id",
        },
        "path_receipt": {
            "status": [
                "present",
                "not_found",
                "corrupt",
                "ambiguous",
                "unavailable",
                "error",
                "not_attempted",
            ],
            "present_projection": [
                "backend",
                "full-publication-record",
                "full-snapshot-manifest",
                "snapshot-manifest-digest",
                "exact-descriptor-inventory",
                "partition-binding-multiset-digest",
                "row-multiset-digest",
                "claim-annotation-multiset-digest",
                "coverage-multiset-digest",
                "unresolved-digest",
                "closure-digest",
                "cursor-projection-digest",
                "canonical-export-digest",
                "semantic-projection-digest",
                "handle-projection-digest",
            ],
            "non-present": "typed-sdk-code-and-field-with-null-projection",
        },
        "passed_path_rules": {
            "all-three": "present",
            "current-selector-and-open-publication": (
                "same-semantic-publication-as-invocation-with-validated-physical-"
                "generation-transition"
            ),
            "open-snapshot": (
                "exact-sdk-authoritative-returned-record-with-requested-snapshot-id"
            ),
            "all-three-semantic-snapshot-projections": "exact-equal",
        },
        "committed_unverified_path_rules": {
            "closed-mixture": (
                "present-or-typed-non-present-for-all-three-paths"
            ),
            "retain-every-successful-projection": True,
            "first-failure": (
                "exact-stage-path-and-sdk-error-or-projection-mismatch"
            ),
            "mismatch_digest_binding": {
                "expected": "recomputed-exact-store-handle-projection",
                "actual": (
                    "retained-successful-handle-projection-at-exact-access-path"
                ),
                "retained_digest_fields": [
                    "snapshot-manifest",
                    "partition-bindings",
                    "rows",
                    "claim-annotations",
                    "coverage",
                    "unresolved",
                    "closure",
                    "cursor-projection",
                    "canonical-export",
                ],
                "computed_domains": {
                    "publication-semantic-fields": (
                        "cxxlens.clang22-reopened-publication-semantic-fields.v1"
                    ),
                    "physical-generation-transition": (
                        "cxxlens.clang22-reopened-physical-generation.v1"
                    ),
                    "descriptor-inventory": (
                        "cxxlens.clang22-reopened-descriptor-inventory.v1"
                    ),
                    "open-snapshot-return-binding": (
                        "cxxlens.clang22-reopened-open-snapshot-return.v1"
                    ),
                },
                "cross-path-equality": (
                    "first-fixed-path-semantic-projection-digest-that-differs-"
                    "from-expected"
                ),
                "cause-expected-actual-exact-equality": "required",
            },
        },
        "exact_compare": [
            "selector",
            "snapshot-manifest",
            "eight-field-partition-bindings",
            "rows",
            "claim-annotation-multiset",
            "coverage-multiset",
            "unresolved",
            "closure",
            "descriptor-inventory",
        ],
        "canonical_export_digest": (
            "sha256-of-exact-utf8-bytes-returned-by-sdk-canonical-export"
        ),
        "snapshot_manifest_digest": (
            "sha256-of-exact-canonical-json-utf8-bytes"
        ),
        "receipt_leaf_digests": (
            "domain-separated-canonical-tuples-in-named-field-order"
        ),
        "semantic_projection_digest": (
            "excludes-access-path-lookup-and-publication-record"
        ),
        "handle_projection_digest": (
            "includes-semantic-projection-and-full-returned-publication-record"
        ),
        "cursor_projection_domain": (
            "cxxlens.clang22-materialization-reopen-cursor.v1"
        ),
        "cursor_projection": (
            "descriptor-id-order-with-row-canonical-forms-claim-annotation-"
            "multiset-and-coverage-multiset"
        ),
        "partition-envelope-proof": (
            "checker-reconstructs-exact-sdk-v5-canonical-export-from-full-report-"
            "claim-envelopes"
        ),
        "physical-generation-equality-across-compaction": "not-required",
        "publication-semantic-fields-and-allowed-generation-transition": "required",
        "snapshot-path-publication-equality-with-series-paths": "not-required",
        "boolean-only-proof": "forbidden",
    },
    "validation_order": [
        "batch",
        "group",
        "task-side-channel",
        "task-result",
        "task-result-and-raw-frame-sets",
        "global-side-channels",
        "guarantee",
        "claim-stages",
        "global-provenance",
        "base-claim-guarantee-cross-binding",
        "sdk-claim-envelopes-and-canonicalization",
        "complete-final-claim-batch",
        "claim-basis-and-partitions",
        "snapshot-manifest",
        "publication-record",
        "reopened-store-state",
    ],
}
STABLE_ERRORS = {
    "materialization.request-invalid",
    "materialization.spool-failure",
    "materialization.version-unsupported",
    "materialization.identity-mismatch",
    "materialization.catalog-census-mismatch",
    "materialization.task-binding-mismatch",
    "materialization.descriptor-binding-mismatch",
    "materialization.transcript-invalid",
    "materialization.group-incomplete",
    "materialization.span-invalid",
    "materialization.claim-invalid",
    "materialization.coverage-incomplete",
    "materialization.stale-parent",
    "materialization.store-failure",
    "materialization.worker-failure",
    "materialization.report-invalid",
}


class MaterializationError(ValueError):
    """One stable installed-materialization contract invariant failed."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code


def fail(code: str, message: str) -> None:
    raise MaterializationError(code, message)


@functools.lru_cache(maxsize=None)
def load(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as error:
        fail("materialization.request-invalid", f"cannot load {path}: {error}")
    if not isinstance(value, dict):
        fail("materialization.request-invalid", f"expected mapping: {path}")
    return value


class _DuplicateJsonMember(ValueError):
    pass


def _reject_duplicate_json_members(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise _DuplicateJsonMember(key)
        value[key] = item
    return value


def load_strict_json_bytes(
    raw: bytes,
    label: str,
    *,
    error_code: str | None = None,
) -> dict[str, Any]:
    """Load one strict UTF-8 JSON object without weakening YAML authority loading."""

    lexical_error = error_code or (
        "materialization.report-invalid"
        if "report" in label.casefold()
        else "materialization.request-invalid"
    )
    if lexical_error not in {
        "materialization.request-invalid",
        "materialization.report-invalid",
    }:
        raise ValueError("strict JSON error_code is not a request/report stable error")
    if not isinstance(raw, bytes):
        fail(
            lexical_error,
            f"{label}: invalid JSON lexical form: input is not bytes",
        )
    if raw.startswith(b"\xef\xbb\xbf"):
        fail(
            lexical_error,
            f"{label}: invalid JSON lexical form: UTF-8 BOM is forbidden",
        )
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        fail(
            lexical_error,
            f"{label}: invalid JSON lexical form: invalid UTF-8 at byte {error.start}",
        )

    def object_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise _DuplicateJsonMember(key)
            result[key] = value
        return result

    def reject_constant(value: str) -> Any:
        raise ValueError(f"non-finite number {value}")

    def parse_exact_integer(value: str) -> int:
        """Honor JSON Schema's mathematical integer domain without binary floats."""

        try:
            parsed = decimal.Decimal(value)
        except decimal.InvalidOperation as error:
            raise ValueError(f"invalid number {value}") from error
        if not parsed.is_finite() or parsed != parsed.to_integral_value():
            raise ValueError(f"non-integral number {value}")
        minimum = decimal.Decimal(-(1 << 63))
        maximum = decimal.Decimal((1 << 64) - 1)
        if parsed < minimum or parsed > maximum:
            raise ValueError(f"integer outside JSON machine domain {value}")
        return int(parsed)

    try:
        value = json.loads(
            text,
            object_pairs_hook=object_pairs,
            parse_float=parse_exact_integer,
            parse_int=parse_exact_integer,
            parse_constant=reject_constant,
        )
    except _DuplicateJsonMember as error:
        fail(
            lexical_error,
            f"{label}: invalid JSON lexical form: duplicate member {error}",
        )
    except (json.JSONDecodeError, ValueError) as error:
        fail(
            lexical_error,
            f"{label}: invalid JSON lexical form: {error}",
        )

    def validate_unicode(item: Any) -> None:
        if isinstance(item, str):
            try:
                item.encode("utf-8", errors="strict")
            except UnicodeEncodeError as error:
                fail(
                    lexical_error,
                    f"{label}: invalid JSON lexical form: invalid Unicode scalar at {error.start}",
                )
        elif isinstance(item, list):
            for child in item:
                validate_unicode(child)
        elif isinstance(item, dict):
            for key, child in item.items():
                validate_unicode(key)
                validate_unicode(child)

    validate_unicode(value)
    if not isinstance(value, dict):
        fail(
            lexical_error,
            f"{label}: expected one top-level JSON object",
        )
    return value


def validate_schema(
    value: Any,
    schema: dict[str, Any],
    label: str,
    *,
    error_code: str = "materialization.request-invalid",
) -> None:
    if error_code not in {
        "materialization.request-invalid",
        "materialization.report-invalid",
    }:
        raise ValueError("schema error_code is not a request/report stable error")
    try:
        jsonschema.Draft202012Validator(
            schema,
            format_checker=jsonschema.Draft202012Validator.FORMAT_CHECKER,
        ).validate(value)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(error_code, f"{label}: {error.message}")


def _utf8_size(value: str, label: str) -> int:
    try:
        return len(value.encode("utf-8", errors="strict"))
    except UnicodeEncodeError as error:
        fail(
            "materialization.request-invalid",
            f"{label} is not strict UTF-8: {error}",
        )


def _enforce_request_schema_utf8_limits(
    value: Any,
    schema: dict[str, Any],
    root_schema: dict[str, Any],
    path: str = "$",
) -> None:
    """Enforce contract byte caps that JSON Schema maxLength cannot express."""

    reference = schema.get("$ref")
    if isinstance(reference, str) and reference.startswith("#/$defs/"):
        definition = reference.removeprefix("#/$defs/")
        if isinstance(value, str):
            limit = {
                "strong_id": MAXIMUM_STRONG_ID_UTF8_BYTES,
                "logical_path": MAXIMUM_LOGICAL_PATH_UTF8_BYTES,
            }.get(definition)
            if limit is not None and _utf8_size(value, path) > limit:
                fail(
                    "materialization.request-invalid",
                    f"{path} exceeds the {definition} UTF-8 byte limit",
                )
        target = root_schema.get("$defs", {}).get(definition)
        if isinstance(target, dict):
            _enforce_request_schema_utf8_limits(value, target, root_schema, path)

    if isinstance(value, dict):
        properties = schema.get("properties", {})
        if isinstance(properties, dict):
            for key, child in value.items():
                child_schema = properties.get(key)
                if isinstance(child_schema, dict):
                    _enforce_request_schema_utf8_limits(
                        child,
                        child_schema,
                        root_schema,
                        f"{path}.{key}",
                    )
    elif isinstance(value, list):
        prefix_items = schema.get("prefixItems")
        if isinstance(prefix_items, list):
            for index, (child, child_schema) in enumerate(zip(value, prefix_items)):
                if isinstance(child_schema, dict):
                    _enforce_request_schema_utf8_limits(
                        child,
                        child_schema,
                        root_schema,
                        f"{path}[{index}]",
                    )
        item_schema = schema.get("items")
        if isinstance(item_schema, dict):
            for index, child in enumerate(value):
                _enforce_request_schema_utf8_limits(
                    child,
                    item_schema,
                    root_schema,
                    f"{path}[{index}]",
                )

    for keyword in ("allOf", "anyOf", "oneOf"):
        branches = schema.get(keyword)
        if isinstance(branches, list):
            for branch in branches:
                if isinstance(branch, dict):
                    _enforce_request_schema_utf8_limits(
                        value,
                        branch,
                        root_schema,
                        path,
                    )


def validate_request_utf8_byte_limits(
    request: dict[str, Any],
    request_schema: dict[str, Any],
) -> None:
    logical_paths: list[tuple[str, Any]] = [
        ("$.project.logical_root", request["project"]["logical_root"]),
    ]
    for index, task in enumerate(request["tasks"]):
        logical_paths.extend(
            [
                (f"$.tasks[{index}].working_directory", task["working_directory"]),
                (f"$.tasks[{index}].source.logical_path", task["source"]["logical_path"]),
            ]
        )
        sysroot = task["toolchain"]["sysroot"]
        if sysroot is not None:
            logical_paths.append((f"$.tasks[{index}].toolchain.sysroot", sysroot))
        if len(task["effective_argv"]) > MAXIMUM_ARGV_ITEMS:
            fail(
                "materialization.request-invalid",
                f"$.tasks[{index}].effective_argv exceeds the item limit",
            )
        for argument_index, argument in enumerate(task["effective_argv"]):
            if _utf8_size(
                argument,
                f"$.tasks[{index}].effective_argv[{argument_index}]",
            ) > MAXIMUM_ARGV_ITEM_UTF8_BYTES:
                fail(
                    "materialization.request-invalid",
                    f"$.tasks[{index}].effective_argv[{argument_index}] exceeds the UTF-8 byte limit",
                )
    for label, logical_path in logical_paths:
        if _utf8_size(logical_path, label) > MAXIMUM_LOGICAL_PATH_UTF8_BYTES:
            fail(
                "materialization.request-invalid",
                f"{label} exceeds the logical-path UTF-8 byte limit",
            )

    sqlite_path = request["publication"]["sqlite_path"]
    if (
        sqlite_path is not None
        and _utf8_size(sqlite_path, "$.publication.sqlite_path")
        > MAXIMUM_SQLITE_RELATIVE_PATH_UTF8_BYTES
    ):
        fail(
            "materialization.request-invalid",
            "$.publication.sqlite_path exceeds the UTF-8 byte limit",
        )
    _enforce_request_schema_utf8_limits(request, request_schema, request_schema)


def canonical_sqlite_relative_path(value: Any) -> str:
    """Validate the exact effect-root-relative SQLite path before any effect."""

    if (
        not isinstance(value, str)
        or not value
        or value != unicodedata.normalize("NFC", value)
        or "\x00" in value
        or "\\" in value
        or value.startswith("/")
        or (
            len(value) >= 2
            and value[0].isascii()
            and value[0].isalpha()
            and value[1] == ":"
        )
        or _utf8_size(value, "SQLite relative path")
        > MAXIMUM_SQLITE_RELATIVE_PATH_UTF8_BYTES
    ):
        fail(
            "materialization.request-invalid",
            "SQLite path is not canonical effect-root-relative UTF-8",
        )
    segments = value.split("/")
    if any(segment in {"", ".", ".."} for segment in segments):
        fail(
            "materialization.request-invalid",
            "SQLite path is not canonical effect-root-relative UTF-8",
        )
    return value


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def content_digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def decode_canonical_base64(value: Any) -> bytes:
    """Decode the unique RFC 4648 spelling accepted by request/task.v3 authority."""

    if not isinstance(value, str):
        fail(
            "materialization.request-invalid",
            "source.content_base64 is not a canonical Base64 string",
        )
    try:
        decoded = base64.b64decode(value, validate=True)
    except (binascii.Error, ValueError) as error:
        fail(
            "materialization.request-invalid",
            f"source.content_base64 is not valid RFC 4648 Base64: {error}",
        )
    canonical = base64.b64encode(decoded).decode("ascii")
    if canonical != value:
        fail(
            "materialization.request-invalid",
            "source.content_base64 is not canonical RFC 4648 Base64",
        )
    return decoded


def _length(value: int) -> bytes:
    return value.to_bytes(8, byteorder="big", signed=False)


def _canonical_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return b"\x04" + _length(len(encoded)) + encoded


def _canonical_bytes(value: bytes) -> bytes:
    return b"\x03" + _length(len(value)) + value


def _canonical_boolean(value: bool) -> bytes:
    return b"\x01" + (b"\x01" if value else b"\x00")


def _canonical_integer(value: int) -> bytes:
    if value < -(1 << 63) or value > (1 << 63) - 1:
        fail(
            "materialization.request-invalid",
            "canonical tuple integer is outside the shared signed-int64 domain",
        )
    negative = value < 0
    magnitude = -value if negative else value
    width = max(1, (magnitude.bit_length() + 7) // 8)
    return (
        b"\x02"
        + (b"\x01" if negative else b"\x00")
        + _length(width)
        + magnitude.to_bytes(width, byteorder="big", signed=False)
    )


def _canonical_tuple(values: Iterable[bytes]) -> bytes:
    items = list(values)
    output = bytearray(b"\x05" + _length(len(items)))
    for item in items:
        output.extend(_length(len(item)))
        output.extend(item)
    return bytes(output)


def source_span_identity(
    snapshot: str,
    file: str,
    begin: int,
    end: int,
    role: str,
) -> str:
    """Mirror sdk::source_span_identity's canonical v1 identity projection."""

    return canonical_identity_digest(
        "source-span",
        [snapshot, file, begin, end, role],
    )


def canonical_identity_digest(identity_kind: str, values: list[Any]) -> str:
    projection = _canonical_tuple(_canonical_projection_value(value) for value in values)
    return identity_kind + ":" + content_digest(
        b"cxxlens\0" + identity_kind.encode("utf-8") + b"\0v1\0" + projection
    )


def canonical_identity_digest_fields(
    identity_kind: str,
    fields: Iterable[bytes],
) -> str:
    projection = _canonical_tuple(fields)
    return identity_kind + ":" + content_digest(
        b"cxxlens\0" + identity_kind.encode("utf-8") + b"\0v1\0" + projection
    )


def normalized_project_logical_path(value: Any) -> tuple[str, str]:
    """Validate and split the installed source-ingestion logical-path authority."""

    prefix = "project://"
    if (
        not isinstance(value, str)
        or not value.startswith(prefix)
        or value != unicodedata.normalize("NFC", value)
        or _utf8_size(value, "source logical path")
        > MAXIMUM_LOGICAL_PATH_UTF8_BYTES
    ):
        fail("materialization.identity-mismatch", "source logical path is not canonical")
    relative = value[len(prefix) :]
    segments = relative.split("/")
    if (
        not relative
        or "\\" in relative
        or "?" in relative
        or "#" in relative
        or any(segment in ("", ".", "..") for segment in segments)
    ):
        fail("materialization.identity-mismatch", "source logical path is not canonical")
    return "project", relative


def file_identity(logical_path: Any) -> str:
    """Derive file_id from path domain, normalized path, and path contract version."""

    domain, relative = normalized_project_logical_path(logical_path)
    return canonical_identity_digest(
        "file",
        [domain, relative, "cxxlens.logical-path.v1"],
    )


def line_index_identity(source: bytes) -> str:
    """Derive the exact byte-line-index ID directly from decoded source bytes."""

    if not isinstance(source, bytes):
        fail("materialization.identity-mismatch", "line-index source is not bytes")
    offsets = [0]
    offsets.extend(index + 1 for index, byte in enumerate(source) if byte == 0x0A)
    return canonical_identity_digest(
        "line-index",
        [
            "cxxlens.byte-line-index.v1",
            content_digest(source),
            len(source),
            offsets,
        ],
    )


def _valid_strong_id(value: Any) -> bool:
    if not isinstance(value, str) or not 0 < len(value) <= 512:
        return False
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError:
        return False
    return len(encoded) <= MAXIMUM_STRONG_ID_UTF8_BYTES and all(
        ord(character) >= 0x20 and ord(character) != 0x7F
        for character in value
    )


def validate_primary_span_bundle(
    bundle: Any,
    task_source: dict[str, Any],
) -> str:
    """Validate the tool-private seven-field bundle independently of relation rows."""

    if bundle is None:
        return "absent"
    if (
        not isinstance(bundle, dict)
        or len(bundle) != len(PRIMARY_SPAN_BUNDLE_FIELDS)
        or set(bundle) != set(PRIMARY_SPAN_BUNDLE_FIELDS)
    ):
        fail(
            "materialization.span-invalid",
            "primary span bundle is neither absent nor the exact seven-field bundle",
        )
    if not all(
        _valid_strong_id(bundle[field])
        for field in ("span_id", "snapshot", "file", "role")
    ):
        fail("materialization.span-invalid", "primary span bundle text is invalid")
    begin = bundle["begin"]
    end = bundle["end"]
    source_size = task_source.get("size_bytes")
    if (
        isinstance(begin, bool)
        or not isinstance(begin, int)
        or isinstance(end, bool)
        or not isinstance(end, int)
        or isinstance(source_size, bool)
        or not isinstance(source_size, int)
        or source_size < 0
        or begin < 0
        or end < begin
        or end > source_size
    ):
        fail("materialization.span-invalid", "primary span range is outside task source")
    if not isinstance(bundle["read_only"], bool):
        fail("materialization.span-invalid", "primary span read_only is not boolean")
    if (
        bundle["snapshot"] != task_source.get("source_snapshot_id")
        or bundle["file"] != task_source.get("file_id")
    ):
        fail("materialization.span-invalid", "primary span task source binding differs")
    expected = source_span_identity(
        bundle["snapshot"],
        bundle["file"],
        begin,
        end,
        bundle["role"],
    )
    if bundle["span_id"] != expected:
        fail("materialization.span-invalid", "primary span identity differs")
    return "present"


def source_span_base_row(
    bundle: dict[str, Any],
    task_source: dict[str, Any],
) -> dict[str, Any]:
    """Construct the exact source.span base row after independent bundle validation."""

    if validate_primary_span_bundle(bundle, task_source) != "present":
        fail("materialization.span-invalid", "absent bundle cannot construct source.span")
    return {
        "span": bundle["span_id"],
        "snapshot": bundle["snapshot"],
        "file": bundle["file"],
        "begin": bundle["begin"],
        "end": bundle["end"],
        "role": bundle["role"],
        "origin": None,
        "read_only": bundle["read_only"],
    }


def _canonical_projection_value(value: Any) -> bytes:
    """Encode the contract's sorted-key tuple projection without JSON authority."""

    if value is None:
        return b"\x00"
    if isinstance(value, bool):
        return _canonical_boolean(value)
    if isinstance(value, int):
        return _canonical_integer(value)
    if isinstance(value, bytes):
        return _canonical_bytes(value)
    if isinstance(value, str):
        return _canonical_string(value)
    if isinstance(value, list):
        return _canonical_tuple(_canonical_projection_value(item) for item in value)
    if isinstance(value, dict):
        return _canonical_tuple(
            _canonical_tuple(
                (_canonical_string(str(key)), _canonical_projection_value(value[key]))
            )
            for key in sorted(value)
        )
    fail("materialization.request-invalid", f"unsupported request projection value: {type(value)}")


def _strict_utf8(value: Any, *, nonempty: bool = False) -> bool:
    if not isinstance(value, str) or (nonempty and not value):
        return False
    try:
        value.encode("utf-8", errors="strict")
    except UnicodeEncodeError:
        return False
    return True


def observation_v2_payload_projection(
    descriptor_id: str,
    payload: Any,
) -> bytes:
    """Encode the exact provider-native v2 payload map without v1 canonical-form reuse."""

    if descriptor_id not in OBSERVATION_KIND_TO_DESCRIPTOR.values():
        fail("materialization.claim-invalid", "observation v2 descriptor is unsupported")
    if not isinstance(payload, dict):
        fail("materialization.claim-invalid", "observation v2 payload is not a map")
    entries: list[tuple[str, str]] = []
    for key, value in payload.items():
        if not _strict_utf8(key, nonempty=True) or not _strict_utf8(value):
            fail(
                "materialization.claim-invalid",
                "observation v2 payload key/value is not strict UTF-8",
            )
        entries.append((key, value))
    entries.sort(key=lambda item: item[0].encode("utf-8"))
    return _canonical_tuple(
        (
            _canonical_string("cxxlens.clang22.observation-payload.v2"),
            _canonical_string(descriptor_id),
            _canonical_tuple(
                _canonical_tuple(
                    (_canonical_string(key), _canonical_string(value))
                )
                for key, value in entries
            ),
        )
    )


def observation_v2_payload_digest(descriptor_id: str, payload: Any) -> str:
    return semantic_digest(
        "cxxlens.clang22.observation-payload.v2",
        observation_v2_payload_projection(descriptor_id, payload),
    )


def observation_v2_origin_chain_bytes(origin_chain: Any) -> bytes | None:
    """Encode immediate-to-outermost native origins; absence is distinct from an empty tuple."""

    if not isinstance(origin_chain, list):
        fail("materialization.claim-invalid", "observation v2 origin chain is not a list")
    if not origin_chain:
        return None
    encoded_entries: list[bytes] = []
    exact_fields = {"kind", "logical_path", "begin", "end", "read_only"}
    for origin in origin_chain:
        if not isinstance(origin, dict) or set(origin) != exact_fields:
            fail(
                "materialization.claim-invalid",
                "observation v2 origin entry does not have the exact field set",
            )
        begin = origin["begin"]
        end = origin["end"]
        if (
            not _strict_utf8(origin["kind"], nonempty=True)
            or not _strict_utf8(origin["logical_path"], nonempty=True)
            or isinstance(begin, bool)
            or not isinstance(begin, int)
            or isinstance(end, bool)
            or not isinstance(end, int)
            or begin < 0
            or end < begin
            or end > (1 << 63) - 1
            or origin["read_only"] is not True
        ):
            fail(
                "materialization.claim-invalid",
                "observation v2 origin entry violates the native v2 domain",
            )
        encoded_entries.append(
            _canonical_tuple(
                (
                    _canonical_string(origin["kind"]),
                    _canonical_string(origin["logical_path"]),
                    _canonical_integer(begin),
                    _canonical_integer(end),
                    _canonical_boolean(True),
                )
            )
        )
    return _canonical_tuple(
        (
            _canonical_string("cxxlens.clang22.source-origin-chain.v2"),
            _canonical_tuple(encoded_entries),
        )
    )


def semantic_digest(domain: str, payload: bytes | str) -> str:
    raw = payload.encode("utf-8") if isinstance(payload, str) else payload
    framed = _canonical_tuple(
        (
            _canonical_string("cxxlens-semantic-digest-v2"),
            _canonical_string(domain),
            _canonical_bytes(raw),
        )
    )
    return "semantic-v2:sha256:" + hashlib.sha256(framed).hexdigest()


def validate_df_0200_claim_batch_corpus(
    root: pathlib.Path,
    binding: dict[str, Any],
    *,
    artifact_bytes: bytes | None = None,
    corpus_schema: dict[str, Any] | None = None,
    driver_text: str | None = None,
    cmake_text: str | None = None,
) -> dict[str, Any]:
    """Validate the immutable D1 corpus, census, and executable public-API binding."""

    try:
        artifact_binding = binding["artifact"]
        census_binding = binding["census"]
        public_reference = binding["current_public_reference"]
        case_ids = binding["artifact_case_ids"]
    except (KeyError, TypeError):
        fail("materialization.claim-invalid", "DF-0200 corpus binding is incomplete")
    if artifact_bytes is None:
        artifact_bytes = (root / artifact_binding["path"]).read_bytes()
    if corpus_schema is None:
        corpus_schema = load(root / artifact_binding["schema"])
    if driver_text is None:
        driver_text = (root / public_reference["driver"]).read_text(encoding="utf-8")
    if cmake_text is None:
        cmake_text = (root / TESTS_CMAKE).read_text(encoding="utf-8")

    if content_digest(artifact_bytes) != artifact_binding["raw_sha256"]:
        fail("materialization.claim-invalid", "DF-0200 corpus raw SHA-256 differs")
    expected_schema_binding = {
        "path": artifact_binding["path"],
        "raw_sha256": artifact_binding["raw_sha256"],
    }
    if (
        corpus_schema.get("$id") != artifact_binding["schema_id"]
        or corpus_schema.get("x-cxxlens-artifact-binding")
        != expected_schema_binding
    ):
        fail("materialization.claim-invalid", "DF-0200 corpus schema binding differs")
    try:
        jsonschema.Draft202012Validator.check_schema(corpus_schema)
    except jsonschema.SchemaError as error:
        fail(
            "materialization.claim-invalid",
            f"DF-0200 corpus schema is invalid: {error.message}",
        )

    try:
        text = artifact_bytes.decode("ascii")
    except UnicodeDecodeError:
        fail("materialization.claim-invalid", "DF-0200 corpus is not strict ASCII")
    if not text.endswith("\n") or "\r" in text or "\x00" in text:
        fail("materialization.claim-invalid", "DF-0200 corpus framing differs")
    lines = text.splitlines()
    expected_header_keys = [
        "format",
        "artifact_version",
        "schema_path",
        "source_api",
        "census",
        "columns",
    ]
    if len(lines) != 6 + census_binding["case_count"]:
        fail("materialization.claim-invalid", "DF-0200 corpus line census differs")
    headers: dict[str, str] = {}
    for index, expected_key in enumerate(expected_header_keys):
        parts = lines[index].split("\t", maxsplit=1)
        if len(parts) != 2 or parts[0] != f"#{expected_key}":
            fail("materialization.claim-invalid", "DF-0200 corpus header order differs")
        headers[expected_key] = parts[1]

    expected_columns = [
        "id",
        "equivalence_group",
        "verdict_group",
        "added_templates",
        "existing_templates",
        "input_encoding_hex",
        "outcome",
        "error_code",
        "error_field",
        "error_detail",
        "claim_count",
        "unresolved_count",
        "conflict_count",
        "differential_count",
        "output_encoding_hex",
        "expected_tuple_encoding_hex",
        "verdict_encoding_hex",
    ]
    if (
        headers["format"] != artifact_binding["format"]
        or headers["artifact_version"] != artifact_binding["artifact_version"]
        or headers["schema_path"] != artifact_binding["schema"]
        or headers["source_api"] != public_reference["operation"]
        or headers["columns"].split("\t") != expected_columns
    ):
        fail("materialization.claim-invalid", "DF-0200 corpus header binding differs")

    census_parts = headers["census"].split(";")
    if any(part.count("=") != 1 for part in census_parts):
        fail("materialization.claim-invalid", "DF-0200 corpus census grammar differs")
    raw_census = dict(part.split("=", maxsplit=1) for part in census_parts)
    expected_census_keys = [
        "case_count",
        "success_count",
        "error_count",
        "added_claim_count",
        "existing_claim_count",
        "projection_byte_count",
        "projection_digest",
    ]
    if list(raw_census) != expected_census_keys:
        fail("materialization.claim-invalid", "DF-0200 corpus census fields differ")
    try:
        census = {
            key: int(raw_census[key])
            for key in expected_census_keys
            if key != "projection_digest"
        }
    except ValueError:
        fail("materialization.claim-invalid", "DF-0200 corpus census is not numeric")
    census["projection_digest"] = raw_census["projection_digest"]
    if any(census[key] != census_binding[key] for key in expected_census_keys):
        fail("materialization.claim-invalid", "DF-0200 corpus frozen census differs")

    def parse_templates(value: str) -> list[str]:
        if value == "-":
            return []
        parsed = value.split(",")
        if any(not item for item in parsed):
            fail("materialization.claim-invalid", "DF-0200 template list differs")
        return parsed

    def parse_optional(value: str) -> str:
        return "" if value == "-" else value

    def parse_hex(value: str, *, optional: bool = False) -> tuple[str, bytes]:
        normalized = "" if optional and value == "-" else value
        try:
            decoded = bytes.fromhex(normalized)
        except ValueError:
            fail("materialization.claim-invalid", "DF-0200 corpus hex differs")
        if decoded.hex() != normalized:
            fail("materialization.claim-invalid", "DF-0200 corpus hex is noncanonical")
        return normalized, decoded

    cases: list[dict[str, Any]] = []
    projection_rows: list[bytes] = []
    actual_added_count = 0
    actual_existing_count = 0
    actual_projection_bytes = 0
    for line in lines[6:]:
        fields = line.split("\t")
        if len(fields) != len(expected_columns):
            fail("materialization.claim-invalid", "DF-0200 corpus row width differs")
        row = dict(zip(expected_columns, fields, strict=True))
        added = parse_templates(row["added_templates"])
        existing = parse_templates(row["existing_templates"])
        input_hex, input_bytes = parse_hex(row["input_encoding_hex"])
        output_hex, output_bytes = parse_hex(
            row["output_encoding_hex"], optional=True
        )
        expected_hex, expected_bytes = parse_hex(row["expected_tuple_encoding_hex"])
        verdict_hex, verdict_bytes = parse_hex(row["verdict_encoding_hex"])
        try:
            counts = {
                key: int(row[key])
                for key in (
                    "claim_count",
                    "unresolved_count",
                    "conflict_count",
                    "differential_count",
                )
            }
        except ValueError:
            fail("materialization.claim-invalid", "DF-0200 result census differs")
        if any(value < 0 for value in counts.values()):
            fail("materialization.claim-invalid", "DF-0200 result census is negative")
        error_fields = [
            parse_optional(row["error_code"]),
            parse_optional(row["error_field"]),
            parse_optional(row["error_detail"]),
        ]
        if row["outcome"] == "success":
            if any(error_fields) or not output_bytes:
                fail("materialization.claim-invalid", "DF-0200 success tuple differs")
        elif row["outcome"] == "error":
            if not error_fields[0] or output_bytes or any(counts.values()):
                fail("materialization.claim-invalid", "DF-0200 error tuple differs")
        else:
            fail("materialization.claim-invalid", "DF-0200 outcome differs")
        if not input_bytes or not expected_bytes or not verdict_bytes:
            fail("materialization.claim-invalid", "DF-0200 projection is empty")
        normalized = {
            "id": row["id"],
            "equivalence_group": parse_optional(row["equivalence_group"]),
            "verdict_group": parse_optional(row["verdict_group"]),
            "added_templates": added,
            "existing_templates": existing,
            "input_encoding_hex": input_hex,
            "outcome": row["outcome"],
            "error_code": error_fields[0],
            "error_field": error_fields[1],
            "error_detail": error_fields[2],
            **counts,
            "output_encoding_hex": output_hex,
            "expected_tuple_encoding_hex": expected_hex,
            "verdict_encoding_hex": verdict_hex,
        }
        cases.append(normalized)
        actual_added_count += len(added)
        actual_existing_count += len(existing)
        actual_projection_bytes += len(input_bytes) + len(expected_bytes) + len(verdict_bytes)
        projection_rows.append(
            _canonical_tuple(
                (
                    _canonical_string(row["id"]),
                    _canonical_bytes(input_bytes),
                    _canonical_bytes(expected_bytes),
                    _canonical_bytes(verdict_bytes),
                )
            )
        )

    actual_success = sum(row["outcome"] == "success" for row in cases)
    actual_error = sum(row["outcome"] == "error" for row in cases)
    if (
        [row["id"] for row in cases] != case_ids
        or len(set(case_ids)) != len(case_ids)
        or actual_success != census["success_count"]
        or actual_error != census["error_count"]
        or actual_added_count != census["added_claim_count"]
        or actual_existing_count != census["existing_claim_count"]
        or actual_projection_bytes != census["projection_byte_count"]
    ):
        fail("materialization.claim-invalid", "DF-0200 case/projection census differs")
    census_projection = _canonical_tuple(
        (
            _canonical_string("cxxlens.df-0200.claim-batch-corpus-census.v1"),
            _canonical_integer(len(cases)),
            _canonical_integer(actual_success),
            _canonical_integer(actual_error),
            _canonical_integer(actual_added_count),
            _canonical_integer(actual_existing_count),
            _canonical_integer(actual_projection_bytes),
            _canonical_tuple(projection_rows),
        )
    )
    if semantic_digest(
        "cxxlens.df-0200.claim-batch-corpus-census.v1", census_projection
    ) != census["projection_digest"]:
        fail("materialization.claim-invalid", "DF-0200 projection digest differs")

    normalized_corpus = {
        "format": headers["format"],
        "artifact_version": headers["artifact_version"],
        "schema_path": headers["schema_path"],
        "source_api": headers["source_api"],
        "census": census,
        "cases": cases,
    }
    try:
        jsonschema.validate(normalized_corpus, corpus_schema)
    except jsonschema.ValidationError as error:
        fail(
            "materialization.claim-invalid",
            f"DF-0200 normalized corpus violates schema: {error.message}",
        )

    required_driver_markers = [
        artifact_binding["raw_sha256"],
        public_reference["operation"],
        "commit(*engine, existing)",
        "artifact == generated",
        'std::string_view{argv[1]} == "--emit"',
        *case_ids,
    ]
    if any(marker not in driver_text for marker in required_driver_markers):
        fail("materialization.claim-invalid", "DF-0200 C++ driver binding differs")
    required_cmake_markers = [
        public_reference["cmake_target"],
        public_reference["ctest"],
        artifact_binding["path"],
        "cxxlens::clang22_provider_sdk",
    ]
    if any(marker not in cmake_text for marker in required_cmake_markers):
        fail("materialization.claim-invalid", "DF-0200 CMake/CTest binding differs")
    return normalized_corpus


def validate_df_0200_codec_receipt_closure(
    codec: dict[str, Any], external: dict[str, Any]
) -> None:
    """Reject field, domain, projection, independence, and receipt-cycle drift."""

    projections = codec.get("event_projections", {})
    projected_fields = {
        field
        for event in projections.values()
        for collection in (event.get("key_fields", []), event.get("payload_fields", []))
        for field in collection
    }
    field_catalog = codec.get("field_catalog", {})
    field_classes = [
        "utf8-string-exactly-one",
        "canonical-bytes-exactly-one",
        "u64be-as-canonical-bytes-exactly-one",
        "ordered-unique-canonical-bytes-tuple-exactly-one",
    ]
    classified = [
        field
        for name in field_classes
        for field in field_catalog.get(name, [])
    ]
    if (
        set(classified) != projected_fields
        or len(classified) != len(set(classified))
        or field_catalog != DF_0200_PARTITION_EVENT_CODEC["field_catalog"]
    ):
        fail(
            "materialization.claim-invalid",
            "DF-0200 event field type/cardinality catalog is incomplete or ambiguous",
        )

    digest_domains = codec.get("digest_domains", {})
    required_digest_ids = {
        "stream-sequence-id",
        "frame-checksum",
        "trailer-frames-digest",
        "trailer-stream-prefix-digest",
        "task-event-digest",
        "partition-event-digest",
        "global-event-digest",
        "task-partition-set-digest",
        "claim-digest",
        "task-claim-digest",
        "global-claim-digest",
        "row-digest",
        "task-row-digest",
        "global-row-digest",
        "coverage-digest",
        "task-coverage-digest",
        "global-coverage-digest",
        "unresolved-digest",
        "task-unresolved-digest",
        "global-unresolved-digest",
        "global-task-set-digest",
        "global-partition-set-digest",
        "partition-content-digest",
    }
    domains = [value.get("domain") for value in digest_domains.values()]
    if (
        set(digest_domains) != required_digest_ids
        or any(not value.get("projection") for value in digest_domains.values())
        or len(domains) != len(set(domains))
    ):
        fail(
            "materialization.claim-invalid",
            "DF-0200 codec digest domain/projection catalog differs",
        )

    try:
        receipts = external["sealed_execution_journal_and_task_receipts"]
        oracle = external["pre_encoder_receipt_oracle"]
        receipt_seal = oracle["receipt_seal"]
        journal = oracle["execution_journal_receipt_set"]
        receipt_catalog = oracle["receipt_field_catalog"]
    except (KeyError, TypeError):
        fail("materialization.claim-invalid", "DF-0200 receipt authority is incomplete")
    task_fields = set(receipts.get("exact_task_projection", []))
    journal_fields = set(receipts.get("exact_journal_projection", []))
    receipt_seal_fields = set(receipt_seal.get("projection", []))
    journal_input_fields = set(journal.get("projection", []))
    if (
        receipt_seal_fields
        != task_fields - {"pre-encoder-task-receipt-seal-digest"}
        or journal_input_fields
        != journal_fields - {"execution-journal-receipt-set-digest"}
        or set(receipt_catalog) != task_fields | journal_fields
        or "successful-seal" not in receipt_seal_fields
        or "selected-request-entry-binding-digest" not in receipt_seal_fields
        or "execution-journal-receipt-set-digest" in receipt_seal_fields
        or "pre-encoder-task-receipt-seal-digest" in journal_input_fields
    ):
        fail(
            "materialization.claim-invalid",
            "DF-0200 receipt seal/journal field closure or acyclicity differs",
        )

    expected_domain_refs = {
        "task-partition-count-and-full-projection-digest": "task-partition-set-digest",
        "task-event-count-and-digest": "task-event-digest",
        "task-claim-count-and-digest": "task-claim-digest",
        "task-row-count-and-digest": "task-row-digest",
        "task-coverage-count-and-digest": "task-coverage-digest",
        "task-unresolved-count-and-digest": "task-unresolved-digest",
    }
    if any(
        receipt_catalog[field].get("domain_id")
        != digest_domains[domain_ref].get("domain")
        for field, domain_ref in expected_domain_refs.items()
    ):
        fail(
            "materialization.claim-invalid",
            "DF-0200 receipt field-to-codec-domain mapping differs",
        )
    expected_provider_refs = {
        "provider-stdout-sha256": (
            "schemas/cxxlens_ng_provider_runtime_contract.yaml#"
            "runtime_private_receipt.raw_stdout"
        ),
        "provider-frame-transcript-digest": (
            "schemas/cxxlens_ng_provider_runtime_contract.yaml#"
            "runtime_private_receipt.frame_transcript"
        ),
        "provider-sealed-transcript-digest": (
            "schemas/cxxlens_ng_provider_runtime_contract.yaml#"
            "runtime_private_receipt.sealed_transcript"
        ),
    }
    if any(
        receipt_catalog[field].get("authority_ref") != authority_ref
        for field, authority_ref in expected_provider_refs.items()
    ):
        fail(
            "materialization.claim-invalid",
            "DF-0200 receipt Provider authority reference differs",
        )
    if (
        receipt_catalog["selected-request-entry-binding-digest"].get("domain_id")
        != external["validated_request"]["selected_request_entry_binding"]["domain"]
        or receipt_catalog["pre-encoder-task-receipt-seal-digest"].get("domain_id")
        != receipt_seal.get("domain")
        or receipt_catalog["execution-journal-receipt-set-digest"].get("domain_id")
        != journal.get("domain")
    ):
        fail(
            "materialization.claim-invalid",
            "DF-0200 receipt seal domain binding differs",
        )
    selected_binding = external["validated_request"][
        "selected_request_entry_binding"
    ]
    if (
        selected_binding.get("cardinality") != "exactly-one-per-selected-task"
        or selected_binding.get("global_set", {}).get("projection")
        != [
            "materialization-request-id",
            "exact-task-count",
            "canonical-task-id-order",
            "ordered-selected-request-entry-binding-digests",
        ]
        or oracle.get("selected_request_entry_cross_check")
        != (
            "exactly-one-entry-whose-task-id-and-canonical-ordinal-equal-this-"
            "task-receipt"
        )
    ):
        fail(
            "materialization.claim-invalid",
            "DF-0200 per-task selected request entry binding differs",
        )


def _df_0200_u64_canonical_bytes(value: int) -> bytes:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < 1 << 64:
        fail("materialization.claim-invalid", "DF-0200 receipt u64 field differs")
    return _canonical_bytes(value.to_bytes(8, byteorder="big", signed=False))


def df_0200_selected_request_entry_digest(
    request_id: str,
    task_id: str,
    task_ordinal: int,
    source_digest: str,
    output_budget: int,
    row_budget: int,
) -> str:
    projection = _canonical_tuple(
        (
            _canonical_string(request_id),
            _canonical_string(task_id),
            _df_0200_u64_canonical_bytes(task_ordinal),
            _canonical_string(source_digest),
            _df_0200_u64_canonical_bytes(output_budget),
            _df_0200_u64_canonical_bytes(row_budget),
        )
    )
    return semantic_digest(
        "cxxlens.df-0200.selected-request-entry-binding.v1", projection
    )


def _df_0200_event_receipt_digests(
    task_id: str, events: list[tuple[str, bytes]]
) -> dict[str, Any]:
    ordered = sorted(events, key=lambda value: (value[0].encode("utf-8"), value[1]))
    if len(ordered) != len(set(ordered)):
        fail("materialization.claim-invalid", "DF-0200 duplicate final event")
    partitions: dict[str, list[bytes]] = {}
    for partition_id, projection in ordered:
        partitions.setdefault(partition_id, []).append(projection)
    partition_rows: list[bytes] = []
    for partition_id, projections in partitions.items():
        partition_projection = _canonical_tuple(
            (
                _canonical_string(partition_id),
                _df_0200_u64_canonical_bytes(len(projections)),
                _canonical_tuple(_canonical_bytes(value) for value in projections),
            )
        )
        partition_digest = semantic_digest(
            "cxxlens.df-0200.partition-event-full-projection.v1",
            partition_projection,
        )
        partition_rows.append(
            _canonical_tuple(
                (_canonical_string(partition_id), _canonical_string(partition_digest))
            )
        )
    partition_set_projection = _canonical_tuple(
        (
            _canonical_string(task_id),
            _df_0200_u64_canonical_bytes(len(partitions)),
            _canonical_tuple(partition_rows),
        )
    )
    task_event_projection = _canonical_tuple(
        (
            _canonical_string(task_id),
            _df_0200_u64_canonical_bytes(len(ordered)),
            _canonical_tuple(_canonical_bytes(value) for _, value in ordered),
        )
    )
    return {
        "partition": (
            len(partitions),
            semantic_digest(
                "cxxlens.df-0200.task-partition-full-projection.v1",
                partition_set_projection,
            ),
        ),
        "event": (
            len(ordered),
            semantic_digest(
                "cxxlens.df-0200.task-event-full-projection.v1",
                task_event_projection,
            ),
        ),
    }


def _df_0200_empty_task_component(
    task_id: str, component: str
) -> tuple[int, str]:
    domain = {
        "claim": "cxxlens.df-0200.task-claim-occurrence-full-projection.v1",
        "row": "cxxlens.df-0200.task-detached-row-full-projection.v1",
        "coverage": "cxxlens.df-0200.task-coverage-full-projection.v1",
        "unresolved": "cxxlens.df-0200.task-unresolved-full-projection.v1",
    }[component]
    projection = _canonical_tuple(
        (
            _canonical_string(task_id),
            _df_0200_u64_canonical_bytes(0),
            _canonical_tuple(()),
        )
    )
    return 0, semantic_digest(domain, projection)


def _df_0200_receipt_field(value: Any) -> bytes:
    if isinstance(value, bool):
        return _canonical_boolean(value)
    if isinstance(value, int):
        return _df_0200_u64_canonical_bytes(value)
    if isinstance(value, str):
        return _canonical_string(value)
    if (
        isinstance(value, tuple)
        and len(value) == 2
        and isinstance(value[0], int)
        and isinstance(value[1], str)
    ):
        return _canonical_tuple(
            (_df_0200_u64_canonical_bytes(value[0]), _canonical_string(value[1]))
        )
    fail("materialization.claim-invalid", "DF-0200 receipt field type differs")


def _df_0200_task_receipt_seal(receipt: dict[str, Any]) -> str:
    projection_order = DF_0200_EXTERNAL_COMPLETENESS_AUTHORITY[
        "pre_encoder_receipt_oracle"
    ]["receipt_seal"]["projection"]
    projection = _canonical_tuple(
        _df_0200_receipt_field(receipt[field]) for field in projection_order
    )
    return semantic_digest(
        "cxxlens.df-0200.pre-encoder-task-receipt.v1", projection
    )


def _df_0200_execution_journal_digest(
    request_id: str, task_ids: list[str], task_seals: list[str]
) -> str:
    projection = _canonical_tuple(
        (
            _canonical_string(request_id),
            _df_0200_u64_canonical_bytes(len(task_ids)),
            _canonical_tuple(_canonical_string(value) for value in task_ids),
            _canonical_tuple(_canonical_string(value) for value in task_seals),
        )
    )
    return semantic_digest(
        "cxxlens.df-0200.execution-journal-receipt-set.v1", projection
    )


def df_0200_build_receipt_fixture(
    events: list[tuple[str, bytes]],
    *,
    request_id: str = "request:df0200",
    task_id: str = "task:0",
    task_ordinal: int = 0,
    selected_request_entry_binding_digest: str | None = None,
) -> tuple[dict[str, Any], str]:
    if selected_request_entry_binding_digest is None:
        selected_request_entry_binding_digest = df_0200_selected_request_entry_digest(
            request_id,
            task_id,
            task_ordinal,
            "sha256:" + "1" * 64,
            1024,
            128,
        )
    event_digests = _df_0200_event_receipt_digests(task_id, events)
    receipt: dict[str, Any] = {
        "materialization-request-id": request_id,
        "selected-request-entry-binding-digest": selected_request_entry_binding_digest,
        "task-id": task_id,
        "canonical-task-ordinal": task_ordinal,
        "successful-seal": True,
        "provider-stdout-byte-count": 16,
        "provider-stdout-sha256": "sha256:" + "2" * 64,
        "decoded-provider-frame-count": 1,
        "provider-frame-transcript-digest": "semantic-v2:sha256:" + "3" * 64,
        "provider-sealed-transcript-digest": "semantic-v2:sha256:" + "4" * 64,
        "task-partition-count-and-full-projection-digest": event_digests["partition"],
        "task-event-count-and-digest": event_digests["event"],
        "task-claim-count-and-digest": _df_0200_empty_task_component(task_id, "claim"),
        "task-row-count-and-digest": _df_0200_empty_task_component(task_id, "row"),
        "task-coverage-count-and-digest": _df_0200_empty_task_component(
            task_id, "coverage"
        ),
        "task-unresolved-count-and-digest": _df_0200_empty_task_component(
            task_id, "unresolved"
        ),
    }
    receipt["pre-encoder-task-receipt-seal-digest"] = _df_0200_task_receipt_seal(
        receipt
    )
    journal = _df_0200_execution_journal_digest(
        request_id,
        [task_id],
        [receipt["pre-encoder-task-receipt-seal-digest"]],
    )
    return receipt, journal


def validate_df_0200_stream_receipt_fixture(
    stream_events: list[tuple[str, bytes]],
    receipt: dict[str, Any],
    claimed_execution_journal_digest: str,
    immutable_execution_journal_digest: str,
    expected_selected_request_entry_binding_digest: str,
) -> None:
    if (
        receipt.get("selected-request-entry-binding-digest")
        != expected_selected_request_entry_binding_digest
    ):
        fail(
            "materialization.claim-invalid",
            "DF-0200 selected request entry/task receipt cross-check differs",
        )
    if receipt.get("successful-seal") is not True:
        fail("materialization.claim-invalid", "DF-0200 successful task seal is absent")
    task_id = receipt.get("task-id")
    request_id = receipt.get("materialization-request-id")
    if not isinstance(task_id, str) or not isinstance(request_id, str):
        fail("materialization.claim-invalid", "DF-0200 receipt identity differs")
    stream_digests = _df_0200_event_receipt_digests(task_id, stream_events)
    if (
        receipt.get("task-partition-count-and-full-projection-digest")
        != stream_digests["partition"]
        or receipt.get("task-event-count-and-digest") != stream_digests["event"]
    ):
        fail(
            "materialization.claim-invalid",
            "DF-0200 stream differs from fixed pre-encoder receipt",
        )
    actual_task_seal = _df_0200_task_receipt_seal(receipt)
    if receipt.get("pre-encoder-task-receipt-seal-digest") != actual_task_seal:
        fail("materialization.claim-invalid", "DF-0200 task receipt seal differs")
    actual_journal = _df_0200_execution_journal_digest(
        request_id, [task_id], [actual_task_seal]
    )
    if actual_journal != claimed_execution_journal_digest:
        fail("materialization.claim-invalid", "DF-0200 claimed final receipt set differs")
    if claimed_execution_journal_digest != immutable_execution_journal_digest:
        fail(
            "materialization.claim-invalid",
            "DF-0200 immutable execution journal receipt-set digest differs",
        )


def admitted_descriptor_inventory(registry: dict[str, Any]) -> list[dict[str, str]]:
    """Project exact Registry bindings into the closed relation-engine inventory."""

    bindings = registry["base_descriptors"] + registry["descriptors"]
    return sorted(
        (
            {
                "descriptor_id": binding["descriptor_id"],
                "runtime_descriptor_digest": binding["runtime_descriptor_digest"],
            }
            for binding in bindings
        ),
        key=lambda binding: binding["descriptor_id"].encode("utf-8"),
    )


def expected_engine_registry_digest(
    admitted_descriptors: Iterable[dict[str, str]],
) -> str:
    inventory = list(admitted_descriptors)
    payload = "".join(
        binding["descriptor_id"]
        + "="
        + binding["runtime_descriptor_digest"]
        + "\n"
        for binding in inventory
    )
    return semantic_digest("cxxlens.relation-registry.v1", payload)


def expected_engine_generation_id(request: dict[str, Any]) -> str:
    return canonical_identity_digest(
        "engine-generation",
        [
            request["engine"]["generation_contract"],
            request["worker"]["provider_id"],
            request["worker"]["provider_version"],
            request["worker"]["semantic_contract_digest"],
            request["engine"]["engine_registry_digest"],
        ],
    )


def expected_interpretation_policy_digest(policy: dict[str, Any]) -> str:
    projection = _canonical_tuple(
        (
            _canonical_string(policy["policy_id"]),
            _canonical_string(policy["selected_domain"]),
        )
    )
    return semantic_digest(INTERPRETATION_POLICY_ID, projection)


def task_sandbox_requirements(tasks: Iterable[dict[str, Any]]) -> list[dict[str, str]]:
    return [
        {"minimum": minimum, "policy_digest": policy_digest}
        for minimum, policy_digest in sorted(
            {
                (task["sandbox"]["minimum"], task["sandbox"]["policy_digest"])
                for task in tasks
            }
        )
    ]


def expected_trust_policy_digest(policy: dict[str, Any]) -> str:
    requirements = policy["task_sandbox_requirements"]
    projection = _canonical_tuple(
        (
            _canonical_string(policy["policy_id"]),
            _canonical_string(policy["execution_profile"]),
            _canonical_string(policy["provider_id"]),
            _canonical_string(policy["provider_version"]),
            _canonical_string(policy["semantic_contract_digest"]),
            _canonical_integer(policy["protocol_major"]),
            _canonical_integer(policy["protocol_minor"]),
            _canonical_tuple(
                _canonical_string(feature)
                for feature in policy["required_features"]
            ),
            _canonical_string(policy["required_qualification"]),
            _canonical_string(policy["worker_sandbox_policy_digest"]),
            _canonical_tuple(
                _canonical_tuple(
                    (
                        _canonical_string(requirement["minimum"]),
                        _canonical_string(requirement["policy_digest"]),
                    )
                )
                for requirement in requirements
            ),
        )
    )
    return semantic_digest(TRUST_POLICY_ID, projection)


def expected_series_id(selector: dict[str, Any]) -> str:
    return canonical_identity_digest(
        "snapshot-series",
        [
            selector[field]
            for field in (
                "catalog_id",
                "channel_id",
                "engine_generation_id",
                "condition_universe_id",
                "relation_registry_digest",
                "interpretation_policy_digest",
                "trust_policy_digest",
            )
        ],
    )


def bind_engine_policy_and_selector_identities(request: dict[str, Any]) -> None:
    inventory = admitted_descriptor_inventory(request["registry"])
    request["engine"] = {
        "generation_contract": ENGINE_GENERATION_CONTRACT,
        "admitted_descriptors": inventory,
        "engine_registry_digest": expected_engine_registry_digest(inventory),
        "engine_generation_id": "pending",
    }
    request["engine"]["engine_generation_id"] = expected_engine_generation_id(request)
    request["interpretation_policy"] = {
        "policy_id": INTERPRETATION_POLICY_ID,
        "selected_domain": INTERPRETATION_DOMAIN,
        "interpretation_policy_digest": "pending",
    }
    request["interpretation_policy"]["interpretation_policy_digest"] = (
        expected_interpretation_policy_digest(request["interpretation_policy"])
    )
    request["trust_policy"] = {
        "policy_id": TRUST_POLICY_ID,
        "execution_profile": "trust.native-worker",
        "provider_id": request["worker"]["provider_id"],
        "provider_version": request["worker"]["provider_version"],
        "semantic_contract_digest": request["worker"]["semantic_contract_digest"],
        "protocol_major": request["worker"]["protocol_major"],
        "protocol_minor": request["worker"]["protocol_minor"],
        "required_features": list(request["worker"]["required_features"]),
        "required_qualification": "canonical-semantic-qualified",
        "worker_sandbox_policy_digest": request["worker"]["sandbox_policy_digest"],
        "task_sandbox_requirements": task_sandbox_requirements(request["tasks"]),
        "trust_policy_digest": "pending",
    }
    request["trust_policy"]["trust_policy_digest"] = expected_trust_policy_digest(
        request["trust_policy"]
    )
    universes = {task["condition_universe_id"] for task in request["tasks"]}
    if len(universes) != 1:
        fail(
            "materialization.task-binding-mismatch",
            "request tasks do not share one Store condition universe",
        )
    selector = {
        "catalog_id": request["project"]["catalog_id"],
        "channel_id": "channel:clang22-production",
        "engine_generation_id": request["engine"]["engine_generation_id"],
        "condition_universe_id": next(iter(universes)),
        "relation_registry_digest": request["engine"]["engine_registry_digest"],
        "interpretation_policy_digest": request["interpretation_policy"][
            "interpretation_policy_digest"
        ],
        "trust_policy_digest": request["trust_policy"]["trust_policy_digest"],
    }
    request["publication"]["selector"] = selector
    request["publication"]["series_id"] = expected_series_id(selector)


def raw_input_observation(request_bytes: bytes) -> dict[str, Any]:
    observed = request_bytes[: RAW_INPUT_BYTE_LIMIT + 1]
    return {
        "byte_limit": RAW_INPUT_BYTE_LIMIT,
        "observed_size_bytes": len(observed),
        "observed_prefix_digest": content_digest(observed),
        "complete": len(request_bytes) <= RAW_INPUT_BYTE_LIMIT,
    }


def project_catalog_projection(project: dict[str, Any]) -> bytes:
    """Independently mirror project_catalog::canonical_projection()."""

    entries = project["catalog_compile_units"]
    return _canonical_tuple(
        (
            _canonical_string("cxxlens.project-catalog.v1"),
            _canonical_string(project["logical_root"]),
            _canonical_string(project["catalog_environment_digest"]),
            _canonical_tuple(
                _canonical_tuple(
                    _canonical_string(entry[field])
                    for field in CATALOG_COMPILE_UNIT_FIELDS
                )
                for entry in entries
            ),
        )
    )


def expected_project_catalog_digest(project: dict[str, Any]) -> str:
    return semantic_digest(
        "cxxlens.project-catalog.v1",
        project_catalog_projection(project),
    )


def expected_catalog_compile_unit_census_digest(project: dict[str, Any]) -> str:
    return semantic_digest(
        "cxxlens.clang22-catalog-compile-unit-census.v1",
        _canonical_tuple(
            _canonical_string(entry["catalog_compile_unit_id"])
            for entry in project["catalog_compile_units"]
        ),
    )


def effective_invocation_projection(
    working_directory: str,
    effective_argv: list[str],
) -> bytes:
    """Encode the installed specialization's exact executed invocation."""

    if (
        not isinstance(working_directory, str)
        or not working_directory
        or _utf8_size(working_directory, "working directory")
        > MAXIMUM_LOGICAL_PATH_UTF8_BYTES
    ):
        fail("materialization.request-invalid", "working directory is invalid")
    if (
        not isinstance(effective_argv, list)
        or not effective_argv
        or len(effective_argv) > MAXIMUM_ARGV_ITEMS
        or any(not isinstance(argument, str) or not argument for argument in effective_argv)
        or any(
            _utf8_size(argument, "effective argv item")
            > MAXIMUM_ARGV_ITEM_UTF8_BYTES
            for argument in effective_argv
        )
    ):
        fail("materialization.request-invalid", "effective argv is invalid")
    return _canonical_tuple(
        (
            _canonical_string("cxxlens.clang22.effective-invocation.v1"),
            _canonical_string(working_directory),
            _canonical_tuple(_canonical_string(argument) for argument in effective_argv),
        )
    )


def expected_normalized_invocation_digest(task: dict[str, Any]) -> str:
    return semantic_digest(
        "cxxlens.clang22.effective-invocation.v1",
        effective_invocation_projection(
            task["working_directory"],
            task["effective_argv"],
        ),
    )


def bind_project_catalog_identity(project: dict[str, Any]) -> None:
    project["catalog_digest"] = expected_project_catalog_digest(project)
    project["catalog_id"] = "catalog:" + project["catalog_digest"]
    project["catalog_compile_unit_census_digest"] = (
        expected_catalog_compile_unit_census_digest(project)
    )


def task_execution_key(row: dict[str, Any]) -> tuple[str, str, str]:
    return tuple(row[field] for field in TASK_EXECUTION_KEY_FIELDS)  # type: ignore[return-value]


def provider_condition_ref_projection(task: dict[str, Any]) -> bytes:
    return _canonical_tuple(
        (
            _canonical_string("cxxlens.clang22.condition-ref.v1"),
            _canonical_string(task["condition_universe_id"]),
            _canonical_string(task["condition_id"]),
        )
    )


def expected_provider_condition_ref_id(task: dict[str, Any]) -> str:
    return "condition-ref:" + semantic_digest(
        "cxxlens.clang22.condition-ref.v1",
        provider_condition_ref_projection(task),
    )


def provider_task_projection(
    request: dict[str, Any],
    task: dict[str, Any],
) -> bytes:
    """Independently mirror sdk::provider::task::canonical_projection()."""

    bindings = {
        row["descriptor_id"]: row["runtime_descriptor_digest"]
        for row in request["registry"]["descriptors"]
    }

    def descriptor_values(descriptor_ids: list[str]) -> bytes:
        descriptors = sorted(
            (descriptor_id, bindings[descriptor_id])
            for descriptor_id in descriptor_ids
        )
        return _canonical_tuple(
            _canonical_tuple(
                (_canonical_string(descriptor_id), _canonical_string(digest))
            )
            for descriptor_id, digest in descriptors
        )

    outputs = descriptor_values(task["requested_descriptor_ids"])
    return _canonical_tuple(
        (
            _canonical_string("cxxlens.provider-task.v1"),
            _canonical_string(request["worker"]["provider_id"]),
            _canonical_string(request["worker"]["provider_version"]),
            _canonical_string(request["worker"]["semantic_contract_digest"]),
            _canonical_string(request["project"]["catalog_id"]),
            _canonical_string(request["project"]["catalog_digest"]),
            outputs,
            _canonical_string(expected_provider_condition_ref_id(task)),
            _canonical_string(task["interpretation_domain"]),
            outputs,
            _canonical_tuple(()),
            _canonical_tuple(
                (_canonical_string("cc.clang22-canonical-1"),)
            ),
            _canonical_string("observation"),
            _canonical_string("assertion"),
            _canonical_tuple(
                _canonical_string(group)
                for group in sorted(task["dependency_groups"])
            ),
        )
    )


def expected_provider_task_id(
    request: dict[str, Any],
    task: dict[str, Any],
) -> str:
    return "task:" + semantic_digest(
        "cxxlens.provider-task.v1",
        provider_task_projection(request, task),
    )


def bind_provider_task_identities(request: dict[str, Any]) -> None:
    for task in request["tasks"]:
        task["provider_task_id"] = expected_provider_task_id(request, task)


def worker_task_v3_projection(
    request: dict[str, Any],
    task: dict[str, Any],
) -> bytes:
    """Encode the installed worker's full-catalog cxxlens.clang22.task.v3 input."""

    project = request["project"]
    global_catalog = {
        key: copy.deepcopy(project[key])
        for key in (
            "catalog_id",
            "catalog_digest",
            "logical_root",
            "catalog_environment_digest",
            "catalog_compile_units",
        )
    }
    per_tu_payload = copy.deepcopy(task)
    for field in (
        "provider_task_id",
        "provider_execution_id",
        "task_input_digest",
        "selected_catalog_compile_unit_id",
        "compile_unit_id",
    ):
        per_tu_payload.pop(field, None)
    source_bytes = decode_canonical_base64(
        per_tu_payload["source"]["content_base64"]
    )
    per_tu_payload["source"]["content_base64"] = base64.b64encode(
        source_bytes
    ).decode("ascii")
    return _canonical_tuple(
        (
            _canonical_string("cxxlens.clang22.task.v3"),
            _canonical_projection_value(global_catalog),
            _canonical_string(task["selected_catalog_compile_unit_id"]),
            _canonical_string(task["compile_unit_id"]),
            _canonical_projection_value(per_tu_payload),
        )
    )


_PROJECTION_UTF8_BOUND = "x-cxxlens-max-utf8-bytes"
_PROJECTION_ARITHMETIC_MAX = (1 << 64) - 1
_TASK_V3_PAYLOAD_EXCLUDED_FIELDS = {
    "provider_task_id",
    "provider_execution_id",
    "task_input_digest",
    "selected_catalog_compile_unit_id",
    "compile_unit_id",
}


def request_source_base64_schema(request_schema: dict[str, Any]) -> dict[str, Any]:
    """Return the selected-v2 source Base64 schema or fail closed on shape drift."""

    try:
        value = request_schema["properties"]["tasks"]["items"]["properties"][
            "source"
        ]["properties"]["content_base64"]
    except (KeyError, TypeError):
        fail(
            "materialization.task-binding-mismatch",
            "request schema lacks source.content_base64 authority",
        )
    if not isinstance(value, dict):
        fail(
            "materialization.task-binding-mismatch",
            "request schema source.content_base64 authority is not an object",
        )
    return value


def _checked_projection_add(*values: int) -> int:
    total = 0
    for value in values:
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            fail(
                "materialization.task-binding-mismatch",
                "task.v3 projection proof has a non-canonical arithmetic operand",
            )
        if value > _PROJECTION_ARITHMETIC_MAX - total:
            fail(
                "materialization.task-binding-mismatch",
                "task.v3 projection proof checked uint64 addition overflowed",
            )
        total += value
    return total


def _checked_projection_multiply(left: int, right: int) -> int:
    if (
        isinstance(left, bool)
        or not isinstance(left, int)
        or left < 0
        or isinstance(right, bool)
        or not isinstance(right, int)
        or right < 0
    ):
        fail(
            "materialization.task-binding-mismatch",
            "task.v3 projection proof has a non-canonical arithmetic operand",
        )
    if left and right > _PROJECTION_ARITHMETIC_MAX // left:
        fail(
            "materialization.task-binding-mismatch",
            "task.v3 projection proof checked uint64 multiplication overflowed",
        )
    return left * right


def _task_v3_projection_component_schemas(
    request_schema: dict[str, Any],
) -> list[tuple[str, dict[str, Any]]]:
    properties = request_schema.get("properties")
    if not isinstance(properties, dict):
        fail(
            "materialization.task-binding-mismatch",
            "request schema lacks task.v3 projection properties",
        )
    project_schema = properties.get("project")
    tasks_schema = properties.get("tasks")
    if not isinstance(project_schema, dict) or not isinstance(tasks_schema, dict):
        fail(
            "materialization.task-binding-mismatch",
            "request schema lacks task.v3 project/task authority",
        )
    project_properties = project_schema.get("properties")
    project_required = project_schema.get("required")
    task_schema = tasks_schema.get("items")
    if (
        not isinstance(project_properties, dict)
        or not isinstance(project_required, list)
        or len(project_required) != len(set(project_required))
        or not isinstance(task_schema, dict)
    ):
        fail(
            "materialization.task-binding-mismatch",
            "request schema task.v3 project/task shape is not closed",
        )
    task_properties = task_schema.get("properties")
    task_required = task_schema.get("required")
    if not isinstance(task_properties, dict) or not isinstance(task_required, list):
        fail(
            "materialization.task-binding-mismatch",
            "request schema task.v3 per-TU payload shape is not closed",
        )

    catalog_fields = (
        "catalog_id",
        "catalog_digest",
        "logical_root",
        "catalog_environment_digest",
        "catalog_compile_units",
    )
    if any(
        field not in project_properties or field not in project_required
        for field in catalog_fields
    ):
        fail(
            "materialization.task-binding-mismatch",
            "request schema lacks an exact required task.v3 global catalog field census",
        )
    global_catalog_schema = {
        "type": "object",
        "additionalProperties": False,
        "required": list(catalog_fields),
        "properties": {
            field: copy.deepcopy(project_properties[field]) for field in catalog_fields
        },
    }

    payload_fields = [
        field
        for field in task_required
        if field not in _TASK_V3_PAYLOAD_EXCLUDED_FIELDS
    ]
    if set(task_properties) != set(task_required) or any(
        field not in task_properties for field in payload_fields
    ):
        fail(
            "materialization.task-binding-mismatch",
            "request schema task.v3 per-TU field census is not exact",
        )
    payload_schema = {
        "type": "object",
        "additionalProperties": False,
        "required": payload_fields,
        "properties": {
            field: copy.deepcopy(task_properties[field]) for field in payload_fields
        },
    }
    for field in ("selected_catalog_compile_unit_id", "compile_unit_id"):
        if field not in task_properties:
            fail(
                "materialization.task-binding-mismatch",
                f"request schema lacks task.v3 {field}",
            )
    return [
        ("contract_tag", {"const": "cxxlens.clang22.task.v3"}),
        ("full_global_project_catalog", global_catalog_schema),
        (
            "selected_catalog_compile_unit_id",
            copy.deepcopy(task_properties["selected_catalog_compile_unit_id"]),
        ),
        (
            "final_relation_compile_unit_id",
            copy.deepcopy(task_properties["compile_unit_id"]),
        ),
        ("exact_per_tu_task_payload", payload_schema),
    ]


def _projection_schema_bound(
    schema: dict[str, Any],
    root_schema: dict[str, Any],
    path: str,
) -> dict[str, Any]:
    reference = schema.get("$ref")
    if reference is not None:
        if not isinstance(reference, str) or not reference.startswith("#/$defs/"):
            fail(
                "materialization.task-binding-mismatch",
                f"task.v3 projection proof has a non-local schema reference at {path}",
            )
        definition = reference.removeprefix("#/$defs/")
        target = root_schema.get("$defs", {}).get(definition)
        if not isinstance(target, dict):
            fail(
                "materialization.task-binding-mismatch",
                f"task.v3 projection proof has an unresolved schema reference at {path}",
            )
        target_bound = _projection_schema_bound(target, root_schema, path)
        return {
            "kind": "ref",
            "path": path,
            "reference": reference,
            "maximum_canonical_bytes": target_bound["maximum_canonical_bytes"],
            "target": target_bound,
        }

    if "const" in schema:
        encoded = _canonical_projection_value(schema["const"])
        return {
            "kind": "const",
            "path": path,
            "value": schema["const"],
            "maximum_canonical_bytes": len(encoded),
        }
    enum = schema.get("enum")
    if isinstance(enum, list) and enum:
        encoded_values = [_canonical_projection_value(value) for value in enum]
        return {
            "kind": "enum",
            "path": path,
            "values": enum,
            "maximum_canonical_bytes": max(map(len, encoded_values)),
        }

    branches = schema.get("oneOf")
    if isinstance(branches, list) and branches:
        branch_bounds = [
            _projection_schema_bound(branch, root_schema, f"{path}.oneOf[{index}]")
            for index, branch in enumerate(branches)
            if isinstance(branch, dict)
        ]
        if len(branch_bounds) != len(branches):
            fail(
                "materialization.task-binding-mismatch",
                f"task.v3 projection proof has a non-schema oneOf branch at {path}",
            )
        selected = max(
            range(len(branch_bounds)),
            key=lambda index: branch_bounds[index]["maximum_canonical_bytes"],
        )
        return {
            "kind": "oneOf",
            "path": path,
            "selected_branch": selected,
            "maximum_canonical_bytes": branch_bounds[selected][
                "maximum_canonical_bytes"
            ],
            "branches": branch_bounds,
        }

    schema_type = schema.get("type")
    if schema_type == "string":
        maximum_utf8_bytes = schema.get(_PROJECTION_UTF8_BOUND)
        if (
            isinstance(maximum_utf8_bytes, bool)
            or not isinstance(maximum_utf8_bytes, int)
            or maximum_utf8_bytes < 0
        ):
            fail(
                "materialization.task-binding-mismatch",
                f"task.v3 projection proof is missing a finite UTF-8 byte bound at {path}",
            )
        maximum = _checked_projection_add(1, 8, maximum_utf8_bytes)
        return {
            "kind": "string",
            "path": path,
            "maximum_utf8_bytes": maximum_utf8_bytes,
            "maximum_unicode_scalars": schema.get("maxLength"),
            "pattern": schema.get("pattern"),
            "maximum_canonical_bytes": maximum,
        }
    if schema_type == "integer":
        minimum = schema.get("minimum")
        maximum_value = schema.get("maximum")
        if (
            isinstance(minimum, bool)
            or not isinstance(minimum, int)
            or isinstance(maximum_value, bool)
            or not isinstance(maximum_value, int)
            or minimum < -(1 << 63)
            or maximum_value > (1 << 63) - 1
            or minimum > maximum_value
        ):
            fail(
                "materialization.task-binding-mismatch",
                f"task.v3 projection proof lacks a finite signed-int64 bound at {path}",
            )
        maximum = max(len(_canonical_integer(minimum)), len(_canonical_integer(maximum_value)))
        return {
            "kind": "integer",
            "path": path,
            "minimum": minimum,
            "maximum": maximum_value,
            "maximum_canonical_bytes": maximum,
        }
    if schema_type == "boolean":
        return {"kind": "boolean", "path": path, "maximum_canonical_bytes": 2}
    if schema_type == "null":
        return {"kind": "null", "path": path, "maximum_canonical_bytes": 1}
    if schema_type == "array":
        maximum_items = schema.get("maxItems")
        item_schema = schema.get("items")
        if (
            isinstance(maximum_items, bool)
            or not isinstance(maximum_items, int)
            or maximum_items < 0
            or not isinstance(item_schema, dict)
            or "prefixItems" in schema
        ):
            fail(
                "materialization.task-binding-mismatch",
                f"task.v3 projection proof lacks one finite homogeneous array bound at {path}",
            )
        item_bound = _projection_schema_bound(item_schema, root_schema, f"{path}[]")
        maximum = _checked_projection_add(
            1,
            8,
            _checked_projection_multiply(
                maximum_items,
                _checked_projection_add(8, item_bound["maximum_canonical_bytes"]),
            ),
        )
        return {
            "kind": "array",
            "path": path,
            "maximum_items": maximum_items,
            "unique_items": schema.get("uniqueItems") is True,
            "maximum_canonical_bytes": maximum,
            "item": item_bound,
        }
    if schema_type == "object":
        properties = schema.get("properties")
        required = schema.get("required")
        if (
            schema.get("additionalProperties") is not False
            or not isinstance(properties, dict)
            or not isinstance(required, list)
            or len(required) != len(set(required))
            or set(required) != set(properties)
        ):
            fail(
                "materialization.task-binding-mismatch",
                f"task.v3 projection proof lacks an exact object field census at {path}",
            )
        fields: list[dict[str, Any]] = []
        maximum = 9
        for field in sorted(properties):
            child = _projection_schema_bound(
                properties[field], root_schema, f"{path}.{field}"
            )
            key_size = len(_canonical_string(field))
            pair_size = _checked_projection_add(
                9,
                8,
                key_size,
                8,
                child["maximum_canonical_bytes"],
            )
            maximum = _checked_projection_add(maximum, 8, pair_size)
            fields.append({"name": field, "key_bytes": key_size, "value": child})
        return {
            "kind": "object",
            "path": path,
            "field_count": len(fields),
            "maximum_canonical_bytes": maximum,
            "fields": fields,
        }
    fail(
        "materialization.task-binding-mismatch",
        f"task.v3 projection proof has an unsupported schema shape at {path}",
    )


def _projection_bound_counts(bound: dict[str, Any]) -> tuple[int, int, int, int]:
    kind = bound["kind"]
    if kind == "ref":
        return _projection_bound_counts(bound["target"])
    if kind == "oneOf":
        return _projection_bound_counts(bound["branches"][bound["selected_branch"]])
    if kind == "object":
        totals = [0, 0, 0, 0]
        for field in bound["fields"]:
            counts = _projection_bound_counts(field["value"])
            totals = [
                _checked_projection_add(total, count)
                for total, count in zip(totals, counts)
            ]
        return tuple(totals)  # type: ignore[return-value]
    if kind == "array":
        structural, expanded, utf8_structural, utf8_expanded = (
            _projection_bound_counts(bound["item"])
        )
        return (
            structural,
            _checked_projection_multiply(bound["maximum_items"], expanded),
            utf8_structural,
            _checked_projection_multiply(
                bound["maximum_items"], utf8_expanded
            ),
        )
    is_string = 1 if kind == "string" else 0
    return (1, 1, is_string, is_string)


def _maximum_string_witness(
    bound: dict[str, Any],
    *,
    salt: int,
) -> str:
    maximum = bound["maximum_utf8_bytes"]
    pattern = bound.get("pattern")
    if pattern == '^[^\\u0000-\\u001f\\u007f]+$':
        scalar_count = bound.get("maximum_unicode_scalars")
        if not isinstance(scalar_count, int) or maximum != scalar_count * 4:
            fail(
                "materialization.task-binding-mismatch",
                f"task.v3 saturated vector lacks a maximum strong-ID witness at {bound['path']}",
            )
        first = chr(0x10000 + (salt % 0xE0000))
        witness = first + chr(0x10000) * (scalar_count - 1)
    elif pattern == '^(?:sha256|semantic-v2:sha256):[0-9a-f]{64}$':
        witness = "semantic-v2:sha256:" + "f" * 64
    elif pattern == '^semantic-v2:sha256:[0-9a-f]{64}$':
        witness = "semantic-v2:sha256:" + "f" * 64
    elif pattern == '^file:sha256:[0-9a-f]{64}$':
        witness = "file:sha256:" + "f" * 64
    elif pattern == '^line-index:sha256:[0-9a-f]{64}$':
        witness = "line-index:sha256:" + "f" * 64
    elif pattern == CANONICAL_BASE64_PATTERN:
        witness = "A" * (maximum - 2) + "=="
    elif isinstance(pattern, str) and pattern.startswith("^(?:project|build|"):
        prefix = "project://"
        witness = prefix + "p" * (maximum - len(prefix))
    elif isinstance(pattern, str) and pattern.startswith("^project://"):
        prefix = "project://"
        witness = prefix + "p" * (maximum - len(prefix))
    else:
        fail(
            "materialization.task-binding-mismatch",
            f"task.v3 saturated vector lacks a finite string witness at {bound['path']}",
        )
    witness_schema = {
        key: value
        for key, value in {
            "type": "string",
            "maxLength": bound.get("maximum_unicode_scalars"),
            "pattern": pattern,
        }.items()
        if value is not None
    }
    try:
        if salt > 0:
            pass
        elif len(witness) <= 1_000_000:
            jsonschema.Draft202012Validator(witness_schema).validate(witness)
        elif (
            pattern != CANONICAL_BASE64_PATTERN
            or maximum % 4 != 0
            or not isinstance(bound.get("maximum_unicode_scalars"), int)
            or len(witness) > bound["maximum_unicode_scalars"]
        ):
            raise jsonschema.ValidationError("large saturated string is invalid")
    except jsonschema.ValidationError:
        fail(
            "materialization.task-binding-mismatch",
            f"task.v3 saturated vector string is not schema-valid at {bound['path']}",
        )
    return witness


def _emit_projection_saturated_witness(
    bound: dict[str, Any],
    emit: Any,
    *,
    salt: int = 0,
) -> None:
    kind = bound["kind"]
    if kind == "ref":
        _emit_projection_saturated_witness(bound["target"], emit, salt=salt)
        return
    if kind == "oneOf":
        _emit_projection_saturated_witness(
            bound["branches"][bound["selected_branch"]], emit, salt=salt
        )
        return
    if kind == "const":
        emit(_canonical_projection_value(bound["value"]))
        return
    if kind == "enum":
        encoded = max(
            (_canonical_projection_value(value) for value in bound["values"]),
            key=len,
        )
        emit(encoded)
        return
    if kind == "string":
        witness = _maximum_string_witness(bound, salt=salt)
        encoded = witness.encode("utf-8", errors="strict")
        if len(encoded) != bound["maximum_utf8_bytes"]:
            fail(
                "materialization.task-binding-mismatch",
                f"task.v3 saturated vector string size differs at {bound['path']}",
            )
        emit(b"\x04" + _length(len(encoded)))
        emit(encoded)
        return
    if kind == "integer":
        candidates = (bound["minimum"], bound["maximum"])
        encoded = max((_canonical_integer(value) for value in candidates), key=len)
        emit(encoded)
        return
    if kind == "boolean":
        emit(_canonical_boolean(False))
        return
    if kind == "null":
        emit(b"\x00")
        return
    if kind == "array":
        count = bound["maximum_items"]
        emit(b"\x05" + _length(count))
        item_size = bound["item"]["maximum_canonical_bytes"]
        for index in range(count):
            emit(_length(item_size))
            _emit_projection_saturated_witness(
                bound["item"], emit, salt=index
            )
        return
    if kind == "object":
        emit(b"\x05" + _length(bound["field_count"]))
        for field in bound["fields"]:
            key = _canonical_string(field["name"])
            child = field["value"]
            pair_size = _checked_projection_add(
                9, 8, len(key), 8, child["maximum_canonical_bytes"]
            )
            emit(_length(pair_size))
            emit(b"\x05" + _length(2))
            emit(_length(len(key)))
            emit(key)
            emit(_length(child["maximum_canonical_bytes"]))
            _emit_projection_saturated_witness(child, emit, salt=salt)
        return
    fail(
        "materialization.task-binding-mismatch",
        f"task.v3 saturated vector has an unsupported bound kind at {bound['path']}",
    )


def _task_v3_projection_component_bounds(
    request_schema: dict[str, Any],
) -> list[dict[str, Any]]:
    return [
        {
            "name": name,
            "bound": _projection_schema_bound(schema, request_schema, f"$.{name}"),
        }
        for name, schema in _task_v3_projection_component_schemas(request_schema)
    ]


def _stream_task_v3_saturated_bounds(
    component_bounds: list[dict[str, Any]],
    emit: Any,
) -> int:
    emitted = 0

    def counted_emit(value: bytes) -> None:
        nonlocal emitted
        emitted = _checked_projection_add(emitted, len(value))
        emit(value)

    counted_emit(b"\x05" + _length(len(component_bounds)))
    for component in component_bounds:
        bound = component["bound"]
        counted_emit(_length(bound["maximum_canonical_bytes"]))
        _emit_projection_saturated_witness(bound, counted_emit)
    return emitted


def stream_worker_task_v3_saturated_vector(
    request_schema: dict[str, Any],
    emit: Any,
) -> int:
    """Stream the maximum schema-valid task.v3 witness to an independent sink."""

    if not callable(emit):
        fail(
            "materialization.task-binding-mismatch",
            "task.v3 saturated vector sink is not callable",
        )
    return _stream_task_v3_saturated_bounds(
        _task_v3_projection_component_bounds(request_schema), emit
    )


def maximum_worker_task_v3_projection_proof(
    request_schema: dict[str, Any],
    *,
    transfer_limit: int = MAXIMUM_TASK_INPUT_BYTES,
) -> dict[str, Any]:
    """Derive and witness the finite task.v3 maximum from request-schema authority."""

    if (
        isinstance(transfer_limit, bool)
        or not isinstance(transfer_limit, int)
        or transfer_limit < 0
        or transfer_limit > _PROJECTION_ARITHMETIC_MAX
    ):
        fail(
            "materialization.task-binding-mismatch",
            "task.v3 projection proof transfer limit is outside checked uint64",
        )
    component_bounds = _task_v3_projection_component_bounds(request_schema)
    outer_framing = _checked_projection_add(
        1, 8, _checked_projection_multiply(len(component_bounds), 8)
    )
    maximum = outer_framing
    for component in component_bounds:
        maximum = _checked_projection_add(
            maximum, component["bound"]["maximum_canonical_bytes"]
        )
    if maximum > transfer_limit:
        fail(
            "materialization.task-binding-mismatch",
            f"maximum task.v3 projection {maximum} exceeds transfer limit {transfer_limit}",
        )

    counts = [0, 0, 0, 0]
    for component in component_bounds:
        component_counts = _projection_bound_counts(component["bound"])
        counts = [
            _checked_projection_add(total, count)
            for total, count in zip(counts, component_counts)
        ]

    hasher = hashlib.sha256()
    emitted = _stream_task_v3_saturated_bounds(component_bounds, hasher.update)
    if emitted != maximum:
        fail(
            "materialization.task-binding-mismatch",
            "task.v3 saturated vector does not attain the derived maximum",
        )

    derivation = {
        "codec": "cxxlens.clang22.task.v3",
        "canonical_codec": "cxxlens-canonical-tuple-v1",
        "components": component_bounds,
    }
    return {
        "authority": (
            "checker-derived-from-every-projected-request-field-count-and-utf8-byte-bound"
        ),
        "request_schema": REQUEST_SCHEMA.as_posix(),
        "derivation_digest": content_digest(canonical_json(derivation)),
        "arithmetic": "checked-uint64-add-and-multiply",
        "canonical_length_field_bytes": 8,
        "component_maximum_bytes": {
            component["name"]: component["bound"]["maximum_canonical_bytes"]
            for component in component_bounds
        },
        "outer_tuple_framing_bytes": outer_framing,
        "structural_leaf_schema_count": counts[0],
        "maximum_expanded_leaf_value_count": counts[1],
        "bounded_utf8_leaf_schema_count": counts[2],
        "maximum_expanded_bounded_utf8_value_count": counts[3],
        "maximum_projection_bytes": maximum,
        "transfer_limit_bytes": transfer_limit,
        "margin_bytes": transfer_limit - maximum,
        "saturated_vector": {
            "construction": "schema-maximal-canonical-task-v3-stream",
            "byte_count": emitted,
            "digest": "sha256:" + hasher.hexdigest(),
        },
        "runtime_magic_constant_only": "forbidden",
    }


def expected_task_input_digest(
    request: dict[str, Any],
    task: dict[str, Any],
) -> str:
    return content_digest(worker_task_v3_projection(request, task))


def expected_provider_execution_id(
    request: dict[str, Any],
    task: dict[str, Any],
) -> str:
    task_prefix = "task:"
    task_id = task["provider_task_id"]
    if not isinstance(task_id, str) or not task_id.startswith(task_prefix):
        fail("materialization.identity-mismatch", "provider task ID grammar differs")
    return canonical_identity_digest(
        "provider-execution",
        [
            request["worker"]["provider_id"],
            request["worker"]["installed_binary_digest"],
            task_id[len(task_prefix) :],
            task["task_input_digest"],
        ],
    )


def bind_task_execution_identities(request: dict[str, Any]) -> None:
    """Bind fixture task-input and physical execution identities bottom-up."""

    for task in request["tasks"]:
        task["task_input_digest"] = expected_task_input_digest(request, task)
    for task in request["tasks"]:
        task["provider_execution_id"] = expected_provider_execution_id(request, task)


def canonical_relation(relation: dict[str, Any]) -> dict[str, Any]:
    """Canonicalize Registry collections whose schema semantics are unordered."""

    value = copy.deepcopy(relation)
    value.setdefault("references", []).sort(
        key=lambda reference: (
            tuple(reference["source_columns"]),
            str(reference["strength"]),
            str(reference["target_relation"]),
            tuple(reference["target_columns"]),
        )
    )
    value["merge"].setdefault("conflict_columns", []).sort()
    row_constraints = value.get("row_constraints")
    if row_constraints is not None:
        groups = row_constraints.setdefault("all_or_none", [])
        row_constraints["all_or_none"] = sorted(
            (sorted(group) for group in groups),
            key=lambda group: tuple(group),
        )
    return value


def _quoted(value: str) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def runtime_descriptor_canonical_form(relation: dict[str, Any]) -> str:
    """Mirror relation_descriptor::canonical_form() from the exact registry row."""

    relation = canonical_relation(relation)
    columns = sorted(relation["columns"], key=lambda item: item["id"])
    column_values = []
    for column in columns:
        column_values.append(
            "{"
            + '"id":'
            + _quoted(column["id"])
            + ',"name":'
            + _quoted(column["name"])
            + ',"required":'
            + ("true" if column["required"] else "false")
            + ',"type":'
            + _quoted(column["type"])
            + ',"role":'
            + _quoted(column["identity_role"])
            + "}"
        )
    conflict_columns = sorted(relation["merge"].get("conflict_columns", []))
    domain = relation["claim"]["domain_identity"]
    references = sorted(
        relation.get("references", []),
        key=lambda reference: (
            tuple(reference["source_columns"]),
            0 if reference["strength"] == "hard" else 1,
            reference["target_relation"],
            tuple(reference["target_columns"]),
        ),
    )
    reference_values = []
    for reference in references:
        reference_values.append(
            "{"
            + '"source":'
            + json.dumps(reference["source_columns"], ensure_ascii=False, separators=(",", ":"))
            + ',"strength":'
            + _quoted(reference["strength"])
            + ',"target_relation":'
            + _quoted(reference["target_relation"])
            + ',"target":'
            + json.dumps(reference["target_columns"], ensure_ascii=False, separators=(",", ":"))
            + "}"
        )
    result_column = domain["result_column"]
    return (
        '{"columns":['
        + ",".join(column_values)
        + '],"conflict_columns":'
        + json.dumps(conflict_columns, ensure_ascii=False, separators=(",", ":"))
        + ',"domain_identity":{"contract":'
        + _quoted(domain["contract"])
        + ',"projection":'
        + json.dumps(domain["projection"], ensure_ascii=False, separators=(",", ":"))
        + ',"result_column":'
        + ("null" if result_column is None else _quoted(result_column))
        + '},"id":'
        + _quoted(relation["descriptor_id"])
        + ',"key_columns":'
        + json.dumps(relation["claim"]["key"], ensure_ascii=False, separators=(",", ":"))
        + ',"merge":'
        + _quoted(relation["merge"]["mode"])
        + ',"name":'
        + _quoted(relation["name"])
        + ',"owner_namespace":'
        + _quoted(relation["owner_namespace"])
        + ',"references":['
        + ",".join(reference_values)
        + '],"semantic_major":'
        + str(relation["semantic_major"])
        + ',"semantics":'
        + _quoted(relation["semantics"])
        + ',"version":'
        + _quoted(relation["version"])
        + "}"
    )


def registry_semantic_projection(registry: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema": registry["schema"],
        "document_version": registry["document_version"],
        "compatibility": registry["compatibility"],
        "registry_policy": registry["registry_policy"],
        "scalar_value_contract": registry["scalar_value_contract"],
        "system_claim_envelope": registry["system_claim_envelope"],
        "api_projection": registry["api_projection"],
        "symbol_contracts": sorted(
            registry["symbol_contracts"], key=lambda row: row["id"]
        ),
        "evolution_policies": sorted(
            registry["evolution_policies"], key=lambda row: row["id"]
        ),
        "relations": sorted(
            (canonical_relation(row) for row in registry["relations"]),
            key=lambda row: row["name"],
        ),
    }


@functools.lru_cache(maxsize=None)
def descriptor_bindings(root: pathlib.Path) -> tuple[str, list[dict[str, Any]]]:
    registry = load(root / REGISTRY)
    relations = {row["descriptor_id"]: row for row in registry["relations"]}
    missing = [descriptor for descriptor in DESCRIPTOR_IDS if descriptor not in relations]
    if missing:
        fail(
            "materialization.descriptor-binding-mismatch",
            f"relation registry lacks exact materialization descriptors: {missing}",
        )
    bindings = []
    for descriptor_id in DESCRIPTOR_IDS:
        relation = canonical_relation(relations[descriptor_id])
        contract = content_digest(canonical_json(relation))
        runtime = semantic_digest(
            "cxxlens.relation-descriptor-binding.v2",
            contract + "\n" + runtime_descriptor_canonical_form(relation),
        )
        bindings.append(
            {
                "descriptor_id": descriptor_id,
                "descriptor_version": relation["version"],
                "contract_digest": contract,
                "runtime_descriptor_digest": runtime,
                "dependency_group_id": DESCRIPTOR_GROUP[descriptor_id],
                "atomic_output_group_id": "clang22-atomic",
                "batch_id": descriptor_id + "-batch",
                "output_stage": DESCRIPTOR_STAGE[descriptor_id],
            }
        )
    registry_digest = content_digest(canonical_json(registry_semantic_projection(registry)))
    return registry_digest, bindings


@functools.lru_cache(maxsize=None)
def base_descriptor_bindings(root: pathlib.Path) -> list[dict[str, Any]]:
    registry = load(root / REGISTRY)
    relations = {row["descriptor_id"]: row for row in registry["relations"]}
    if any(descriptor not in relations for descriptor in BASE_DESCRIPTOR_IDS):
        fail(
            "materialization.descriptor-binding-mismatch",
            "relation registry lacks an exact base descriptor",
        )
    bindings = []
    for stage_order, descriptor_id in enumerate(BASE_DESCRIPTOR_IDS):
        relation = canonical_relation(relations[descriptor_id])
        contract = content_digest(canonical_json(relation))
        bindings.append(
            {
                "descriptor_id": descriptor_id,
                "descriptor_version": relation["version"],
                "contract_digest": contract,
                "runtime_descriptor_digest": semantic_digest(
                    "cxxlens.relation-descriptor-binding.v2",
                    contract + "\n" + runtime_descriptor_canonical_form(relation),
                ),
                "stage_order": stage_order,
                "output_stage": "canonical_claim",
                "owner": "installed-tool",
            }
        )
    return bindings


def base_registry_relations(root: pathlib.Path) -> dict[str, dict[str, Any]]:
    registry = load(root / REGISTRY)
    relations = {row["descriptor_id"]: row for row in registry["relations"]}
    return {descriptor: relations[descriptor] for descriptor in BASE_DESCRIPTOR_IDS}


@functools.lru_cache(maxsize=None)
def admitted_registry_relations(root: pathlib.Path) -> dict[str, dict[str, Any]]:
    registry = load(root / REGISTRY)
    relations = {row["descriptor_id"]: row for row in registry["relations"]}
    return {descriptor: relations[descriptor] for descriptor in ADMITTED_DESCRIPTOR_IDS}


def _base_scalar_type(type_name: str) -> str:
    while type_name.startswith("optional<") and type_name.endswith(">"):
        type_name = type_name[len("optional<") : -1]
    return type_name


def _detached_scalar_json(type_name: str, value: Any) -> Any:
    scalar = _base_scalar_type(type_name)
    if scalar in {"bytes"} or scalar.startswith("set<"):
        if not isinstance(value, (bytes, bytearray)):
            fail("materialization.claim-invalid", "detached bytes cell is not bytes")
        return bytes(value).hex()
    if scalar == "bool":
        if not isinstance(value, bool):
            fail("materialization.claim-invalid", "detached bool cell is not boolean")
        return value
    if scalar in {"int64", "uint64"}:
        if isinstance(value, bool) or not isinstance(value, int):
            fail("materialization.claim-invalid", "detached integer cell is not integer")
        return value
    if not isinstance(value, str) or not _strict_utf8(value):
        fail("materialization.claim-invalid", "detached text cell is not strict UTF-8")
    return value


def detached_row_canonical_form(
    relation: dict[str, Any],
    row: dict[str, Any],
    *,
    include_absent_optional: bool = False,
) -> str:
    """Mirror detached_row::canonical_form() for one validated Registry row."""

    cells: dict[str, Any] = {}
    for column in sorted(relation["columns"], key=lambda item: item["id"]):
        if column["name"] not in row:
            if column["required"]:
                fail(
                    "materialization.claim-invalid",
                    f"detached row omits required cell {column['id']}",
                )
            if include_absent_optional:
                cells[column["id"]] = {
                    "state": "absent",
                    "type": column["type"],
                }
            continue
        value = row[column["name"]]
        if value is None:
            if not column["type"].startswith("optional<"):
                fail(
                    "materialization.claim-invalid",
                    f"non-optional detached cell is absent: {column['id']}",
                )
            cells[column["id"]] = {
                "state": "absent",
                "type": column["type"],
            }
            continue
        cells[column["id"]] = {
            "state": "present",
            "type": column["type"],
            "value": _detached_scalar_json(column["type"], value),
        }
    return canonical_json(
        {"cells": cells, "descriptor_id": relation["descriptor_id"]}
    ).decode("utf-8")


def _parse_detached_row_canonical_form(
    root: pathlib.Path,
    canonical_form: str,
    descriptor_id: str,
) -> tuple[dict[str, Any], dict[str, dict[str, Any]]]:
    try:
        value = json.loads(
            canonical_form,
            object_pairs_hook=_reject_duplicate_json_members,
            parse_constant=lambda token: (_ for _ in ()).throw(
                ValueError(f"non-finite number {token}")
            ),
        )
    except (UnicodeError, ValueError, json.JSONDecodeError, _DuplicateJsonMember) as error:
        fail("materialization.claim-invalid", f"claim row is not canonical JSON: {error}")
    if (
        not isinstance(value, dict)
        or set(value) != {"cells", "descriptor_id"}
        or value["descriptor_id"] != descriptor_id
        or not isinstance(value["cells"], dict)
        or canonical_json(value).decode("utf-8") != canonical_form
    ):
        fail("materialization.claim-invalid", "claim row canonical form differs")
    relation = admitted_registry_relations(root).get(descriptor_id)
    if relation is None:
        fail("materialization.claim-invalid", "claim row descriptor is not admitted")
    columns = {column["id"]: column for column in relation["columns"]}
    for column_id, cell in value["cells"].items():
        column = columns.get(column_id)
        if (
            column is None
            or not isinstance(cell, dict)
            or cell.get("type") != column["type"]
            or cell.get("state") not in {"present", "absent", "unknown"}
        ):
            fail("materialization.claim-invalid", "claim row cell metadata differs")
        state = cell["state"]
        expected_keys = {"state", "type"}
        if state == "present":
            expected_keys.add("value")
            _detached_scalar_json(column["type"], _decoded_detached_scalar(cell))
        elif state == "absent":
            if not column["type"].startswith("optional<"):
                fail("materialization.claim-invalid", "required claim cell is absent")
        else:
            expected_keys.add("unknown_reason")
            reason = cell.get("unknown_reason")
            if not isinstance(reason, str) or not reason or not _strict_utf8(reason):
                fail("materialization.claim-invalid", "claim cell unknown reason differs")
        if set(cell) != expected_keys:
            fail("materialization.claim-invalid", "claim row cell shape differs")
    for column in relation["columns"]:
        if column["required"] and column["id"] not in value["cells"]:
            fail("materialization.claim-invalid", "claim row omits a required SDK cell")
    return relation, value["cells"]


def _decoded_detached_scalar(cell: dict[str, Any]) -> Any:
    scalar = _base_scalar_type(cell["type"])
    value = cell.get("value")
    if scalar in {"bytes"} or scalar.startswith("set<"):
        if (
            not isinstance(value, str)
            or len(value) % 2
            or any(byte not in "0123456789abcdef" for byte in value)
        ):
            fail("materialization.claim-invalid", "claim row bytes are not lowercase hex")
        return bytes.fromhex(value)
    if scalar == "bool":
        if not isinstance(value, bool):
            fail("materialization.claim-invalid", "claim row boolean differs")
        return value
    if scalar == "int64":
        if isinstance(value, bool) or not isinstance(value, int) or not -(2**63) <= value < 2**63:
            fail("materialization.claim-invalid", "claim row signed integer differs")
        return value
    if scalar == "uint64":
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < 2**64:
            fail("materialization.claim-invalid", "claim row unsigned integer differs")
        return value
    if not isinstance(value, str) or not _strict_utf8(value):
        fail("materialization.claim-invalid", "claim row text differs")
    return value


def _claim_cell_projection(cell: dict[str, Any] | None) -> bytes:
    if cell is None or cell["state"] == "absent":
        return _canonical_projection_value(None)
    if cell["state"] == "unknown":
        return _canonical_tuple(
            (
                _canonical_string("unknown"),
                _canonical_string(cell.get("unknown_reason", "unspecified")),
            )
        )
    scalar = _base_scalar_type(cell["type"])
    value = _decoded_detached_scalar(cell)
    if scalar == "bool":
        return _canonical_boolean(value)
    if scalar == "int64":
        return _canonical_integer(value)
    if scalar == "uint64":
        return _canonical_tuple(
            (_canonical_string("unsigned"), _canonical_string(str(value)))
        )
    if scalar in {"bytes"} or scalar.startswith("set<"):
        return _canonical_bytes(value)
    return _canonical_string(value)


def sdk_claim_identities(
    root: pathlib.Path,
    envelope: dict[str, Any],
) -> tuple[str, str, str]:
    relation, cells = _parse_detached_row_canonical_form(
        root,
        envelope["row_canonical_form"],
        envelope["descriptor_id"],
    )
    semantic_key = canonical_identity_digest_fields(
        "semantic-key",
        (
            _canonical_string(relation["descriptor_id"]),
            _canonical_integer(relation["semantic_major"]),
            _canonical_tuple(
                _claim_cell_projection(cells.get(column_id))
                for column_id in relation["claim"]["key"]
            ),
        ),
    )
    assertion = canonical_identity_digest_fields(
        "assertion",
        (
            _canonical_string(semantic_key),
            _canonical_string(envelope["presence"]["universe"]),
            _canonical_string(claim_condition_canonical_form(envelope["presence"])),
            _canonical_string(envelope["interpretation"]),
            _canonical_string(envelope["producer"]["semantic_contract"]),
        ),
    )
    payload_columns = sorted(
        column["id"]
        for column in relation["columns"]
        if column["identity_role"] == "authoritative_payload"
    )
    content = canonical_identity_digest_fields(
        "claim-content",
        (
            _canonical_string(assertion),
            _canonical_tuple(
                _claim_cell_projection(cells.get(column_id))
                for column_id in payload_columns
            ),
        ),
    )
    return semantic_key, assertion, content


def derive_registry_row_identity(relation: dict[str, Any], row: dict[str, Any]) -> str:
    """Derive any accepted Registry result ID from its declared ordered projection."""

    columns = {column["id"]: column for column in relation["columns"]}
    result_id = relation["claim"]["domain_identity"]["result_column"]
    result_type = columns[result_id]["type"]
    prefix = "typed_id<"
    suffix = "_id>"
    if not result_type.startswith(prefix) or not result_type.endswith(suffix):
        fail("materialization.identity-mismatch", "registry result type is not a typed ID")
    identity_kind = result_type[len(prefix) : -len(suffix)].replace("_", "-")
    projection: list[Any] = []
    for column_id in relation["claim"]["domain_identity"]["projection"]:
        column = columns[column_id]
        if column["name"] not in row:
            if column["type"].startswith("optional<"):
                projection.append(None)
                continue
            fail(
                "materialization.identity-mismatch",
                f"registry identity projection is missing {column_id}",
            )
        projection.append(row[column["name"]])
    return canonical_identity_digest(identity_kind, projection)


def derive_base_row_identity(relation: dict[str, Any], row: dict[str, Any]) -> str:
    return derive_registry_row_identity(relation, row)


def observation_v2_native_row(
    root: pathlib.Path,
    record: Any,
    task: dict[str, Any],
) -> tuple[str, dict[str, Any]]:
    """Construct one exact v2 Registry row from validated provider-native fields."""

    exact_fields = set(
        EXPECTED_OBSERVATION_V2_NATIVE_CODEC["native_record"]["exact_fields"]
    )
    if not isinstance(record, dict) or set(record) != exact_fields:
        fail(
            "materialization.claim-invalid",
            "observation v2 native record does not have the exact field set",
        )
    kind = record["kind"]
    if kind not in OBSERVATION_KIND_TO_DESCRIPTOR:
        fail("materialization.claim-invalid", "observation v2 native kind is unsupported")
    descriptor_id = OBSERVATION_KIND_TO_DESCRIPTOR[kind]
    compile_unit = record["final_relation_compile_unit_id"]
    semantic_key = record["semantic_key"]
    exact = record["exact_equivalence"]
    limitation = record["limitation"]
    if (
        not _valid_strong_id(compile_unit)
        or compile_unit != task.get("compile_unit_id")
    ):
        fail("materialization.claim-invalid", "observation v2 compile unit is invalid")
    if not _strict_utf8(semantic_key, nonempty=True):
        fail("materialization.claim-invalid", "observation v2 semantic key is invalid")
    if not isinstance(exact, bool):
        fail("materialization.claim-invalid", "observation exact-equivalence is not boolean")
    if limitation is not None and not _strict_utf8(limitation, nonempty=True):
        fail("materialization.claim-invalid", "observation limitation is invalid")
    if (exact and limitation is not None) or (not exact and limitation is None):
        fail(
            "materialization.claim-invalid",
            "observation exact-equivalence/limitation coupling is invalid",
        )

    primary_span = record["primary_span"]
    origin_bytes = observation_v2_origin_chain_bytes(record["origin_chain"])
    if kind == "type":
        if primary_span is not None or origin_bytes is not None:
            fail(
                "materialization.claim-invalid",
                "type observation cannot discard source or origin authority",
            )
    else:
        validate_primary_span_bundle(primary_span, task["source"])

    row: dict[str, Any] = {
        "compile_unit": compile_unit,
        "semantic_key": semantic_key.encode("utf-8"),
        "payload_digest": observation_v2_payload_digest(
            descriptor_id,
            record["payload"],
        ),
        "exact_equivalence": exact,
    }
    if limitation is not None:
        row["limitation"] = limitation
    if primary_span is not None:
        row.update(
            {
                "source": primary_span["span_id"],
                "source_snapshot": primary_span["snapshot"],
                "source_file": primary_span["file"],
                "source_begin": primary_span["begin"],
                "source_end": primary_span["end"],
                "source_role": primary_span["role"],
                "source_read_only": primary_span["read_only"],
            }
        )
    if origin_bytes is not None:
        row["source_origin_chain"] = origin_bytes

    registry = load(root / REGISTRY)
    relation = next(
        (
            candidate
            for candidate in registry["relations"]
            if candidate["descriptor_id"] == descriptor_id
        ),
        None,
    )
    if relation is None:
        fail(
            "materialization.descriptor-binding-mismatch",
            "observation v2 descriptor is absent from the Registry",
        )
    row["observation"] = derive_registry_row_identity(relation, row)
    if row["observation"] != derive_registry_row_identity(relation, row):
        fail("materialization.identity-mismatch", "observation v2 identity differs")
    return descriptor_id, row


def base_claim_row_digest(descriptor_id: str, row: dict[str, Any]) -> str:
    return semantic_digest(
        "cxxlens.base-claim-row.v1",
        _canonical_projection_value(
            {"descriptor_id": descriptor_id, "row": row}
        ),
    )


def base_claim_rows(root: pathlib.Path, request: dict[str, Any]) -> dict[str, list[dict[str, Any]]]:
    project = request["project"]
    rows: dict[str, list[dict[str, Any]]] = {
        "build.project.v1": [
            {
                "project": project["project_id"],
                "catalog": project["catalog_id"],
                "catalog_digest": project["catalog_digest"],
                "logical_root": project["logical_root"],
                "environment_digest": project["catalog_environment_digest"],
            }
        ],
        "build.toolchain_context.v1": [],
        "build.variant.v1": [],
        "source.file.v1": [],
        "build.compile_unit.v1": [],
        "source.span.v1": [],
    }
    for task in request["tasks"]:
        toolchain = task["toolchain"]
        variant = task["variant"]
        source = task["source"]
        rows["build.toolchain_context.v1"].append(
            {
                "toolchain": task["toolchain_context_id"],
                **toolchain,
            }
        )
        rows["build.variant.v1"].append(
            {
                "variant": task["build_variant_id"],
                "project": task["project_id"],
                "toolchain": task["toolchain_context_id"],
                **variant,
            }
        )
        rows["source.file.v1"].append(
            {
                "snapshot": source["source_snapshot_id"],
                "file": source["file_id"],
                "project": task["project_id"],
                "logical_path": source["logical_path"],
                "content": source["content_digest"],
                "size": source["size_bytes"],
                "encoding": source["encoding"],
                "line_index": source["line_index_id"],
                "read_only": source["read_only"],
            }
        )
        rows["build.compile_unit.v1"].append(
            {
                "compile_unit": task["compile_unit_id"],
                "project": task["project_id"],
                "main_source": source["source_snapshot_id"],
                "variant": task["build_variant_id"],
                "toolchain": task["toolchain_context_id"],
                "effective_invocation_digest": task["normalized_invocation_digest"],
                "language": task["language"],
                "working_directory": task["working_directory"],
            }
        )
    relations = base_registry_relations(root)
    deduplicated: dict[str, list[dict[str, Any]]] = {}
    identities: dict[str, set[str]] = {}
    for descriptor in BASE_DESCRIPTOR_IDS[:-1]:
        result_field = BASE_RESULT_FIELDS[descriptor]
        by_identity: dict[str, dict[str, Any]] = {}
        for row in rows[descriptor]:
            actual = row[result_field]
            expected = derive_base_row_identity(relations[descriptor], row)
            if actual != expected:
                fail(
                    "materialization.identity-mismatch",
                    f"base row identity differs: {descriptor}",
                )
            if actual in by_identity and by_identity[actual] != row:
                fail(
                    "materialization.claim-invalid",
                    f"conflicting base row: {descriptor}",
                )
            by_identity[actual] = row
        deduplicated[descriptor] = [by_identity[key] for key in sorted(by_identity)]
        identities[descriptor] = set(by_identity)
    references = {
        "build.variant.v1": [("project", "build.project.v1"), ("toolchain", "build.toolchain_context.v1")],
        "source.file.v1": [("project", "build.project.v1")],
        "build.compile_unit.v1": [
            ("project", "build.project.v1"),
            ("main_source", "source.file.v1"),
            ("variant", "build.variant.v1"),
            ("toolchain", "build.toolchain_context.v1"),
        ],
    }
    for descriptor, bindings in references.items():
        for row in deduplicated[descriptor]:
            for field, target in bindings:
                if row[field] not in identities[target]:
                    fail(
                        "materialization.claim-invalid",
                        f"base hard reference differs: {descriptor}.{field}",
                    )
    deduplicated["source.span.v1"] = []
    return deduplicated


def validate_span_registry_columns(
    registry: dict[str, Any],
    binding: dict[str, Any],
) -> None:
    expected_binding = {
        "authority": REGISTRY.as_posix(),
        "abstract_to_registry_column_id": SPAN_REGISTRY_COLUMN_MAPPING,
        "all_or_none": "exact-single-mapped-seven-as-unordered-set",
        "origin_evidence_column_id": SPAN_ORIGIN_COLUMN_MAPPING,
        "origin_evidence_shape": (
            "optional-bytes-excluded-from-primary-mapping-and-all-or-none"
        ),
        "validation": (
            "exact-column-id-name-type-optionality-row-constraint-and-origin-separation-"
            "before-row-adoption"
        ),
    }
    if binding != expected_binding:
        fail("materialization.span-invalid", "primary span registry mapping differs")
    relations = {row["descriptor_id"]: row for row in registry["relations"]}
    for descriptor in SPAN_OBSERVATION_DESCRIPTORS:
        if descriptor not in relations:
            fail(
                "materialization.span-invalid",
                f"primary span registry relation is missing: {descriptor}",
            )
        columns = {column["id"]: column for column in relations[descriptor]["columns"]}
        mapping = binding["abstract_to_registry_column_id"][descriptor]
        if len(set(mapping.values())) != len(PRIMARY_SPAN_BUNDLE_FIELDS):
            fail(
                "materialization.span-invalid",
                f"primary span registry mapping aliases columns: {descriptor}",
            )
        for abstract in PRIMARY_SPAN_BUNDLE_FIELDS:
            column_id = mapping[abstract]
            column = columns.get(column_id)
            expected_name, expected_type = SPAN_REGISTRY_COLUMN_SHAPES[abstract]
            if column is None or (
                column["name"] != expected_name
                or column["type"] != expected_type
                or column["required"] is not False
            ):
                fail(
                    "materialization.span-invalid",
                    f"primary span registry column differs: {descriptor}.{abstract}",
                )
        mapped_columns = sorted(
            mapping[abstract] for abstract in PRIMARY_SPAN_BUNDLE_FIELDS
        )
        if canonical_relation(relations[descriptor]).get("row_constraints") != {
            "all_or_none": [mapped_columns]
        }:
            fail(
                "materialization.span-invalid",
                f"primary span all-or-none registry constraint differs: {descriptor}",
            )
        origin_id = binding["origin_evidence_column_id"][descriptor]
        origin = columns.get(origin_id)
        if (
            origin is None
            or origin["name"] != "source_origin_chain"
            or origin["type"] != "optional<bytes>"
            or origin["required"] is not False
            or origin_id in mapping.values()
            or origin_id in mapped_columns
        ):
            fail(
                "materialization.span-invalid",
                f"origin evidence registry separation differs: {descriptor}",
            )


def materialization_dependency_documents(root: pathlib.Path) -> dict[pathlib.Path, dict[str, Any]]:
    return {
        path: copy.deepcopy(load(root / path))
        for path in (PORTABLE_PROVIDER_TASK, PROVIDER_PROTOCOL, PROVIDER_RUNTIME)
    }


def validate_materialization_contract_dependencies(contract: dict[str, Any]) -> None:
    if contract["dependencies"] != [path.as_posix() for path in GENERIC_DEPENDENCIES]:
        fail(
            "materialization.request-invalid",
            "materialization dependencies are not the exact generic authority set",
        )
    if contract["dependency_policy"] != {
        "kind": "generic-input-authorities-only",
        "self_dependency": "forbidden",
        "reverse_specialization_edges": "non-dependency",
        "cycles": "forbidden",
    }:
        fail("materialization.request-invalid", "materialization dependency policy differs")
    contract_path = CONTRACT.as_posix()
    if contract_path in contract["dependencies"]:
        fail("materialization.request-invalid", "materialization contract depends on itself")


def validate_project_catalog_authority(root: pathlib.Path) -> None:
    authority = load(root / PROJECT_CATALOG)
    if authority.get("canonical_projection") != {
        "codec": "cxxlens-canonical-tuple-v1",
        "domain": "cxxlens.project-catalog.v1",
        "fields": ["contract_tag", "logical_root", "environment_digest", "compile_units"],
        "entry_fields": [
            "compile_unit_id",
            "effective_invocation_digest",
            "source_digest",
            "environment_digest",
        ],
        "ordering": "compile-unit-id-byte-order",
        "duplicate_policy": "reject-identical-and-conflicting",
    }:
        fail(
            "materialization.identity-mismatch",
            "public project catalog canonical codec differs",
        )
    if authority.get("identity") != {
        "catalog_digest": "semantic-digest-v2-over-canonical-projection",
        "catalog_id": "catalog-prefix-plus-exact-catalog-digest",
        "validation": "bottom-up-recompute-and-exact-compare",
    }:
        fail(
            "materialization.identity-mismatch",
            "public project catalog identity contract differs",
        )
    boundary = authority.get("identity_boundary", {})
    if boundary != {
        "catalog_compile_unit_id": "project-upstream-catalog-input-census-identity",
        "build_compile_unit_relation_id": (
            "independently-derived-from-accepted-relation-registry"
        ),
        "implicit_equality_alias": "forbidden",
        "consumer_mapping": "exact-entry-digests-to-final-relation-payload-and-id",
    }:
        fail(
            "materialization.task-binding-mismatch",
            "public catalog/final relation identity boundary differs",
        )


def validate_materialization_dependency_graph(
    contract: dict[str, Any],
    documents: dict[pathlib.Path, dict[str, Any]],
) -> None:
    validate_materialization_contract_dependencies(contract)
    contract_path = CONTRACT.as_posix()
    try:
        portable = documents[PORTABLE_PROVIDER_TASK]["installed_specializations"][
            "clang22_materialization"
        ]
        protocol = documents[PROVIDER_PROTOCOL]["adoption_boundary"]
        runtime_document = documents[PROVIDER_RUNTIME]
        runtime_projection = runtime_document["protocol_session"]["typed_validation"][
            "adoption_projection"
        ]
        runtime_clang22 = runtime_document["clang22"]
    except (KeyError, TypeError) as error:
        fail(
            "materialization.request-invalid",
            f"specialization projection is missing: {error}",
        )
    relevant_projections = {
        PORTABLE_PROVIDER_TASK: (portable, EXPECTED_PORTABLE_SPECIALIZATION),
        PROVIDER_PROTOCOL: (protocol, EXPECTED_PROTOCOL_ADOPTION_BOUNDARY),
        PROVIDER_RUNTIME: (
            runtime_projection,
            EXPECTED_RUNTIME_ADOPTION_PROJECTION,
        ),
    }
    for path, (projection, expected) in relevant_projections.items():
        if projection != expected:
            fail(
                "materialization.request-invalid",
                f"specialization projection differs: {path}",
            )
        if contract_path in documents[path].get("dependencies", []):
            fail(
                "materialization.request-invalid",
                f"reverse specialization became a dependency cycle: {path}",
            )
    if (
        runtime_clang22.get("observations") != DESCRIPTOR_IDS[3:]
        or runtime_clang22.get("legacy_observations_v1")
        != "private-transport-only-non-adoptable"
        or runtime_clang22.get("canonical_outputs") != DESCRIPTOR_IDS[:3]
        or runtime_clang22.get("installed_materialization")
        != EXPECTED_RUNTIME_INSTALLED_MATERIALIZATION
    ):
        fail(
            "materialization.request-invalid",
            "runtime Clang 22 specialization differs",
        )


@functools.lru_cache(maxsize=None)
def authority_bindings(root: pathlib.Path) -> list[dict[str, str]]:
    return [
        {"path": path.as_posix(), "digest": content_digest((root / path).read_bytes())}
        for path in AUTHORITY_PATHS
    ]


@functools.lru_cache(maxsize=None)
def occurrence_authority_bindings(root: pathlib.Path) -> list[dict[str, str]]:
    return [
        {
            "role": role,
            "path": installed_path,
            "digest": content_digest((root / source_path).read_bytes()),
        }
        for role, installed_path, source_path in OCCURRENCE_AUTHORITY_FILES
    ]


def fixture_occurrence_manifest(
    root: pathlib.Path,
    *,
    source_revision: str,
    source_tree: str,
    configuration: str,
    tool_digest: str,
    worker_digest: str,
    shared_runtime_files: list[dict[str, str]] | None = None,
) -> dict[str, Any]:
    """Build the exact closed, ordered, self-excluding occurrence manifest."""

    files: list[dict[str, str]] = [
        {
            "role": "materializer-executable",
            "path": "bin/cxxlens-clang22-materialize",
            "digest": tool_digest,
        },
        {
            "role": "worker-executable",
            "path": "bin/cxxlens-clang-worker-22",
            "digest": worker_digest,
        },
        *occurrence_authority_bindings(root),
    ]
    if configuration == "shared":
        runtime_files = shared_runtime_files or [
            {
                "role": role,
                "path": path,
                "digest": content_digest(
                    f"cxxlens fixture shared runtime {role}".encode("utf-8")
                ),
            }
            for role, path in SHARED_OCCURRENCE_RUNTIME_FILES
        ]
        files.extend(copy.deepcopy(runtime_files))
    payload = {
        "schema": "cxxlens.clang22-materializer-occurrence-manifest.v1",
        "manifest_version": "1.0.0",
        "source_revision": source_revision,
        "source_tree": source_tree,
        "package_configuration": configuration,
        "files": files,
    }
    return {
        **payload,
        "occurrence_payload_digest": content_digest(canonical_json(payload)),
    }


def validate_occurrence_manifest(
    root: pathlib.Path,
    manifest: dict[str, Any],
) -> None:
    """Validate occurrence schema, closed inventory and both digest boundaries."""

    validate_schema(
        manifest,
        load(root / OCCURRENCE_SCHEMA),
        "materializer occurrence manifest",
    )
    if any(row["path"] == OCCURRENCE_MANIFEST_PATH for row in manifest["files"]):
        fail(
            "materialization.identity-mismatch",
            "occurrence manifest inventories its own bytes",
        )
    authority_end = 2 + len(OCCURRENCE_AUTHORITY_FILES)
    if manifest["files"][2:authority_end] != occurrence_authority_bindings(root):
        fail(
            "materialization.identity-mismatch",
            "occurrence authority role/path/digest inventory differs",
        )
    payload = {
        key: copy.deepcopy(value)
        for key, value in manifest.items()
        if key != "occurrence_payload_digest"
    }
    if manifest["occurrence_payload_digest"] != content_digest(
        canonical_json(payload)
    ):
        fail(
            "materialization.identity-mismatch",
            "occurrence payload digest differs",
        )


def fixture_occurrence_measurement(
    root: pathlib.Path,
    *,
    source_revision: str,
    source_tree: str,
    configuration: str,
    tool_digest: str,
    worker_digest: str,
) -> dict[str, Any]:
    """Build one deterministic measured-occurrence receipt from typed inventory."""

    manifest = fixture_occurrence_manifest(
        root,
        source_revision=source_revision,
        source_tree=source_tree,
        configuration=configuration,
        tool_digest=tool_digest,
        worker_digest=worker_digest,
    )
    validate_occurrence_manifest(root, manifest)
    return {
        "manifest_path": OCCURRENCE_MANIFEST_PATH,
        "manifest_file_digest": content_digest(canonical_json(manifest)),
        "occurrence_payload_digest": manifest["occurrence_payload_digest"],
        "inventory_digest": content_digest(canonical_json(manifest["files"])),
        "source_revision": source_revision,
        "source_tree": source_tree,
        "configuration": configuration,
        "files": copy.deepcopy(manifest["files"]),
        "tool": {
            "path": "bin/cxxlens-clang22-materialize",
            "digest": tool_digest,
        },
        "worker": {
            "path": "bin/cxxlens-clang-worker-22",
            "digest": worker_digest,
        },
    }


def measured_occurrence_manifest(measured: dict[str, Any]) -> dict[str, Any]:
    """Recover manifest bytes from explicit measured fields, never from a digest."""

    return {
        "schema": "cxxlens.clang22-materializer-occurrence-manifest.v1",
        "manifest_version": "1.0.0",
        "source_revision": measured["source_revision"],
        "source_tree": measured["source_tree"],
        "package_configuration": measured["configuration"],
        "files": copy.deepcopy(measured["files"]),
        "occurrence_payload_digest": measured["occurrence_payload_digest"],
    }


def validate_measured_occurrence(
    root: pathlib.Path,
    request: dict[str, Any],
    measured: dict[str, Any],
) -> None:
    manifest = measured_occurrence_manifest(measured)
    validate_occurrence_manifest(root, manifest)
    manifest_bytes = canonical_json(manifest)
    files = manifest["files"]
    if (
        measured["manifest_path"] != OCCURRENCE_MANIFEST_PATH
        or measured["manifest_file_digest"] != content_digest(manifest_bytes)
        or measured["inventory_digest"] != content_digest(canonical_json(files))
        or measured["manifest_file_digest"]
        != request["tool"]["occurrence_manifest_digest"]
        or measured["source_revision"] != request["tool"]["source_revision"]
        or measured["source_tree"] != request["tool"]["source_tree"]
        or measured["configuration"]
        != request["tool"]["package_configuration"]
        or measured["tool"]
        != {"path": files[0]["path"], "digest": files[0]["digest"]}
        or measured["worker"]
        != {"path": files[1]["path"], "digest": files[1]["digest"]}
        or files[0]["digest"]
        != request["tool"]["installed_executable_digest"]
        or files[1]["digest"] != request["worker"]["installed_binary_digest"]
    ):
        fail(
            "materialization.identity-mismatch",
            "requested/measured installed occurrence binding differs across explicit files",
        )


def expected_sqlite_effect_root_receipt(
    request: dict[str, Any],
) -> dict[str, Any] | None:
    if request["publication"]["backend"] == "memory":
        return None
    relative_path = request["publication"]["sqlite_path"]
    return {
        "contract": "rooted-vfs-v1",
        "root_observation_digest": semantic_digest(
            "cxxlens.clang22-rooted-sqlite-effect-root.v1",
            _canonical_projection_value(
                {
                    "materialization_request_id": request[
                        "materialization_request_id"
                    ],
                    "relative_path": relative_path,
                }
            ),
        ),
        "relative_path": relative_path,
        "parent_resolution": "openat2-beneath-no-symlinks-no-magiclinks",
        "leaf_resolution": "database-and-sidecars-rooted-no-follow",
    }


def _request_projection(request: dict[str, Any], *, semantic: bool) -> dict[str, Any]:
    value = copy.deepcopy(request)
    for field in (
        "materialization_request_id",
        "request_digest",
        "semantic_request_digest",
    ):
        value.pop(field, None)
    if semantic:
        publication = value["publication"]
        for field in (
            "backend",
            "series_id",
            "expected_parent_publication",
            "sqlite_path",
        ):
            publication.pop(field)
    return value


def expected_semantic_request_digest(request: dict[str, Any]) -> str:
    return semantic_digest(
        "cxxlens.clang22-semantic-request.v2",
        _canonical_projection_value(_request_projection(request, semantic=True)),
    )


def expected_request_digest(request: dict[str, Any]) -> str:
    return semantic_digest(
        "cxxlens.clang22-materialization-request.v2",
        _canonical_projection_value(_request_projection(request, semantic=False)),
    )


def bind_request_identity(request: dict[str, Any]) -> None:
    request["semantic_request_digest"] = expected_semantic_request_digest(request)
    request["request_digest"] = expected_request_digest(request)
    request["materialization_request_id"] = "materialization:" + request["request_digest"]


def _merge_schema_intersection(
    left: dict[str, Any], right: dict[str, Any]
) -> dict[str, Any]:
    """Merge the request schema's local allOf/ref intersections for size derivation."""

    output = copy.deepcopy(left)
    for name, value in right.items():
        if name == "required":
            output[name] = list(dict.fromkeys(output.get(name, []) + value))
        elif name == "properties":
            properties = output.setdefault(name, {})
            for property_name, property_schema in value.items():
                properties[property_name] = _merge_schema_intersection(
                    properties.get(property_name, {}), property_schema
                )
        elif name in {
            "maxLength",
            "maxItems",
            "maximum",
            "x-cxxlens-max-utf8-bytes",
        } and name in output:
            output[name] = min(output[name], value)
        elif name in {"minLength", "minItems", "minimum"} and name in output:
            output[name] = max(output[name], value)
        elif name == "additionalProperties" and name in output:
            output[name] = output[name] is not False and value
        elif name == "enum" and name in output:
            output[name] = [item for item in output[name] if item in value]
            if not output[name]:
                fail(
                    "materialization.request-invalid",
                    "semantic replay size proof enum intersection is empty",
                )
        elif name == "const" and name in output and output[name] != value:
            fail(
                "materialization.request-invalid",
                "semantic replay size proof const intersection conflicts",
            )
        elif name == "type" and name in output and output[name] != value:
            fail(
                "materialization.request-invalid",
                "semantic replay size proof type intersection conflicts",
            )
        elif name == "pattern" and name in output:
            # A second pattern can only narrow the already bounded domain. Retain the first
            # pattern's conservative escaping class rather than guessing their intersection.
            continue
        else:
            output[name] = copy.deepcopy(value)
    return output


def _resolve_local_schema_intersection(
    root: dict[str, Any], node: dict[str, Any]
) -> dict[str, Any]:
    resolved = copy.deepcopy(node)
    reference = resolved.pop("$ref", None)
    if reference is not None:
        if not isinstance(reference, str) or not reference.startswith("#/"):
            fail(
                "materialization.request-invalid",
                "semantic replay size proof encountered a non-local schema reference",
            )
        target: Any = root
        for component in reference[2:].split("/"):
            component = component.replace("~1", "/").replace("~0", "~")
            if not isinstance(target, dict) or component not in target:
                fail(
                    "materialization.request-invalid",
                    "semantic replay size proof encountered an unresolved schema reference",
                )
            target = target[component]
        if not isinstance(target, dict):
            fail(
                "materialization.request-invalid",
                "semantic replay size proof reference is not a schema object",
            )
        resolved = _merge_schema_intersection(
            _resolve_local_schema_intersection(root, target), resolved
        )
    intersections = resolved.pop("allOf", [])
    if not isinstance(intersections, list):
        fail(
            "materialization.request-invalid",
            "semantic replay size proof allOf differs",
        )
    for intersection in intersections:
        if not isinstance(intersection, dict):
            fail(
                "materialization.request-invalid",
                "semantic replay size proof allOf member differs",
            )
        resolved = _merge_schema_intersection(
            resolved, _resolve_local_schema_intersection(root, intersection)
        )
    return resolved


def _canonical_json_scalar_bytes(value: Any) -> int:
    return len(
        json.dumps(
            value,
            ensure_ascii=False,
            allow_nan=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
    )


def _maximum_canonical_json_bytes(
    root: dict[str, Any], node: dict[str, Any]
) -> int:
    """Return a fail-closed selected-schema canonical JSON byte upper bound."""

    schema = _resolve_local_schema_intersection(root, node)
    if "const" in schema:
        return _canonical_json_scalar_bytes(schema["const"])
    if "enum" in schema:
        choices = schema["enum"]
        if not isinstance(choices, list) or not choices:
            fail(
                "materialization.request-invalid",
                "semantic replay size proof enum differs",
            )
        return max(_canonical_json_scalar_bytes(value) for value in choices)
    if "oneOf" in schema:
        alternatives = schema["oneOf"]
        if not isinstance(alternatives, list) or not alternatives:
            fail(
                "materialization.request-invalid",
                "semantic replay size proof oneOf differs",
            )
        base = {name: value for name, value in schema.items() if name != "oneOf"}
        bounds = [
            _maximum_canonical_json_bytes(
                root, _merge_schema_intersection(base, alternative)
            )
            for alternative in alternatives
            if isinstance(alternative, dict)
        ]
        if len(bounds) != len(alternatives):
            fail(
                "materialization.request-invalid",
                "semantic replay size proof oneOf member differs",
            )
        return max(bounds)

    schema_type = schema.get("type")
    if isinstance(schema_type, list):
        if not schema_type:
            fail(
                "materialization.request-invalid",
                "semantic replay size proof type union differs",
            )
        return max(
            _maximum_canonical_json_bytes(root, {**schema, "type": member})
            for member in schema_type
        )
    if schema_type == "string" or (schema_type is None and "pattern" in schema):
        pattern = schema.get("pattern")
        fixed_pattern_bytes = {
            r"^[0-9a-f]{40}$": 40,
            r"^sha256:[0-9a-f]{64}$": 71,
            r"^engine-generation:sha256:[0-9a-f]{64}$": 89,
            r"^snapshot-series:sha256:[0-9a-f]{64}$": 87,
            r"^task:semantic-v2:sha256:[0-9a-f]{64}$": 88,
            (
                r"^(?:cc\.(?:call_direct_target|call_site|entity)\.v1|"
                r"frontend\.clang22\.(?:call|entity|type)_observation\.v2)-batch$"
            ): 44,
        }
        if pattern in fixed_pattern_bytes:
            return fixed_pattern_bytes[pattern] + 2
        maximum_scalars = schema.get("maxLength")
        maximum_utf8_bytes = schema.get("x-cxxlens-max-utf8-bytes")
        ascii_patterns = {
            r"^(?:sha256|semantic-v2:sha256):[0-9a-f]{64}$",
            r"^semantic-v2:sha256:[0-9a-f]{64}$",
            r"^file:sha256:[0-9a-f]{64}$",
            r"^line-index:sha256:[0-9a-f]{64}$",
            CANONICAL_BASE64_PATTERN,
        }
        control_excluding_patterns = {
            r"^[^\u0000-\u001f\u007f]+$",
            (
                r"^(?:project|build|toolchain|sysroot|generated|provider|external)://"
                r"[^\u0000-\u001f\u007f]+$"
            ),
            r"^project://[^\u0000-\u001f\u007f]+$",
        }
        ascii_pattern = pattern in ascii_patterns
        if ascii_pattern and isinstance(maximum_utf8_bytes, int):
            return maximum_utf8_bytes + 2
        if not isinstance(maximum_scalars, int) or maximum_scalars < 0:
            fail(
                "materialization.request-invalid",
                f"semantic replay size proof string scalar bound is missing: {pattern}",
            )
        if maximum_utf8_bytes is None:
            maximum_utf8_bytes = 4 * maximum_scalars
        if not isinstance(maximum_utf8_bytes, int) or maximum_utf8_bytes < 0:
            fail(
                "materialization.request-invalid",
                "semantic replay size proof string UTF-8 bound is missing",
            )
        if ascii_pattern:
            payload_bytes = maximum_utf8_bytes
        elif pattern in control_excluding_patterns:
            payload_bytes = min(
                maximum_utf8_bytes + maximum_scalars, 4 * maximum_scalars
            )
        else:
            payload_bytes = min(
                maximum_utf8_bytes + 5 * maximum_scalars, 6 * maximum_scalars
            )
        return payload_bytes + 2
    if schema_type == "integer":
        minimum = schema.get("minimum")
        maximum = schema.get("maximum")
        if not isinstance(minimum, int) or not isinstance(maximum, int):
            fail(
                "materialization.request-invalid",
                "semantic replay size proof explicit integer bounds differ",
            )
        return max(len(str(minimum)), len(str(maximum)))
    if schema_type == "boolean":
        return 5
    if schema_type == "null":
        return 4
    if schema_type == "array":
        prefix_items = schema.get("prefixItems")
        if prefix_items is not None:
            if not isinstance(prefix_items, list) or schema.get("items") is not False:
                fail(
                    "materialization.request-invalid",
                    "semantic replay size proof prefix array is open",
                )
            item_bounds = [
                _maximum_canonical_json_bytes(root, item) for item in prefix_items
            ]
            if schema.get("maxItems") != len(prefix_items):
                fail(
                    "materialization.request-invalid",
                    "semantic replay size proof prefix array cardinality differs",
                )
        else:
            maximum_items = schema.get("maxItems")
            items = schema.get("items")
            if not isinstance(maximum_items, int) or maximum_items < 0:
                fail(
                    "materialization.request-invalid",
                    "semantic replay size proof array bound is missing",
                )
            if not isinstance(items, dict):
                fail(
                    "materialization.request-invalid",
                    "semantic replay size proof array item schema differs",
                )
            item_bound = _maximum_canonical_json_bytes(root, items)
            return 2 + maximum_items * item_bound + max(0, maximum_items - 1)
        return 2 + sum(item_bounds) + max(0, len(item_bounds) - 1)
    if schema_type == "object" or "properties" in schema:
        properties = schema.get("properties")
        required = schema.get("required")
        if schema.get("additionalProperties") is not False:
            fail(
                "materialization.request-invalid",
                "semantic replay size proof object is open",
            )
        if not isinstance(properties, dict) or not isinstance(required, list):
            fail(
                "materialization.request-invalid",
                "semantic replay size proof object shape differs",
            )
        if set(properties) != set(required):
            fail(
                "materialization.request-invalid",
                "semantic replay size proof object has optional properties",
            )
        member_bounds = []
        for name in required:
            member = properties.get(name)
            if not isinstance(name, str) or not isinstance(member, dict):
                fail(
                    "materialization.request-invalid",
                    "semantic replay size proof object member differs",
                )
            member_bounds.append(
                _canonical_json_scalar_bytes(name)
                + 1
                + _maximum_canonical_json_bytes(root, member)
            )
        return 2 + sum(member_bounds) + max(0, len(member_bounds) - 1)
    fail(
        "materialization.request-invalid",
        "semantic replay size proof encountered an unsupported schema form",
    )


def selected_schema_semantic_replay_bounds(
    request_schema: dict[str, Any],
) -> tuple[int, int]:
    global_schema = copy.deepcopy(request_schema)
    properties = global_schema.get("properties")
    if not isinstance(properties, dict) or "tasks" not in properties:
        fail(
            "materialization.request-invalid",
            "semantic replay global task substitution path differs",
        )
    properties["tasks"] = {"const": []}
    task_schema = copy.deepcopy(
        request_schema.get("properties", {}).get("tasks", {}).get("items")
    )
    if not isinstance(task_schema, dict):
        fail(
            "materialization.request-invalid",
            "semantic replay task schema is missing",
        )
    try:
        task_schema["properties"]["source"]["properties"]["content_base64"] = {
            "const": ""
        }
    except (KeyError, TypeError):
        fail(
            "materialization.request-invalid",
            "semantic replay source substitution path differs",
        )
    return (
        _maximum_canonical_json_bytes(request_schema, global_schema),
        _maximum_canonical_json_bytes(request_schema, task_schema),
    )


def validate_contract_exact(
    contract: dict[str, Any],
    request_schema: dict[str, Any] | None = None,
    report_schema: dict[str, Any] | None = None,
    contract_schema: dict[str, Any] | None = None,
    snapshot_store_contract: dict[str, Any] | None = None,
) -> None:
    validate_materialization_contract_dependencies(contract)
    if request_schema is None:
        request_schema = load(ROOT / REQUEST_SCHEMA)
    if report_schema is None:
        report_schema = load(ROOT / REPORT_SCHEMA)
    if contract_schema is None:
        contract_schema = load(ROOT / CONTRACT_SCHEMA)
    if snapshot_store_contract is None:
        snapshot_store_contract = load(ROOT / SNAPSHOT_STORE)
    if content_digest(canonical_json(report_schema)) != (
        EXPECTED_REPORT_SCHEMA_CANONICAL_DIGEST
    ):
        fail(
            "materialization.report-invalid",
            "full report schema canonical digest differs",
        )
    if request_source_base64_schema(request_schema) != {
        "type": "string",
        "maxLength": 22_369_624,
        "x-cxxlens-max-utf8-bytes": 22_369_624,
        "x-cxxlens-base64-canonicality": CANONICAL_BASE64_ANNOTATION,
        "pattern": CANONICAL_BASE64_PATTERN,
    }:
        fail(
            "materialization.task-binding-mismatch",
            "canonical Base64 request schema differs",
        )
    if content_digest(canonical_json(request_schema)) != (
        EXPECTED_REQUEST_SCHEMA_CANONICAL_DIGEST
    ):
        fail(
            "materialization.request-invalid",
            "selected request schema canonical fingerprint differs",
        )

    semantic_replay_bounds = selected_schema_semantic_replay_bounds(request_schema)
    expected_semantic_replay_bounds = (
        MAXIMUM_GLOBAL_SEMANTIC_JSON_BYTES,
        MAXIMUM_TASK_METADATA_SEMANTIC_JSON_BYTES,
    )
    if semantic_replay_bounds != expected_semantic_replay_bounds or any(
        bound >= MAXIMUM_SEMANTIC_REPLAY_WINDOW_BYTES
        for bound in semantic_replay_bounds
    ):
        fail(
            "materialization.request-invalid",
            "selected-schema semantic replay bound differs",
        )

    request_requirements = (
        request_schema.get("properties", {})
        .get("trust_policy", {})
        .get("properties", {})
        .get("task_sandbox_requirements")
    )
    report_requirements = (
        report_schema.get("$defs", {})
        .get("trust_policy", {})
        .get("properties", {})
        .get("task_sandbox_requirements")
    )
    expected_requirement_bound = {
        "type": "array",
        "minItems": 1,
        "maxItems": MAXIMUM_TASK_SANDBOX_REQUIREMENTS,
        "uniqueItems": True,
    }
    for label, requirements in (
        ("request", request_requirements),
        ("report mirror", report_requirements),
    ):
        if not isinstance(requirements, dict) or {
            key: requirements.get(key) for key in expected_requirement_bound
        } != expected_requirement_bound:
            fail(
                "materialization.request-invalid",
                f"{label} trust requirement schema limit differs",
            )
    if request_requirements.get("items") != report_requirements.get("items"):
        fail(
            "materialization.report-invalid",
            "request/report trust requirement item schemas differ",
        )

    resource_schema = (
        contract_schema.get("properties", {})
        .get("surface", {})
        .get("properties", {})
        .get("resource_limits", {})
    )
    required_resource_schema_fields = {
        "maximum_task_sandbox_requirements",
        "maximum_json_members_per_object",
        "maximum_request_schema_capture_utf8_bytes",
        "maximum_request_version_capture_utf8_bytes",
        "semantic_replay",
        "admission_failure",
    }
    if not required_resource_schema_fields.issubset(
        set(resource_schema.get("required", []))
    ):
        fail(
            "materialization.request-invalid",
            "contract schema admission resource requirements differ",
        )
    resource_schema_properties = resource_schema.get("properties", {})
    expected_resource_schema_consts = {
        "maximum_task_sandbox_requirements": MAXIMUM_TASK_SANDBOX_REQUIREMENTS,
        "maximum_json_members_per_object": MAXIMUM_JSON_MEMBERS_PER_OBJECT,
        "maximum_request_schema_capture_utf8_bytes": (
            MAXIMUM_REQUEST_SCHEMA_CAPTURE_BYTES
        ),
        "maximum_request_version_capture_utf8_bytes": (
            MAXIMUM_REQUEST_VERSION_CAPTURE_BYTES
        ),
    }
    for name, expected in expected_resource_schema_consts.items():
        if resource_schema_properties.get(name) != {"const": expected}:
            fail(
                "materialization.request-invalid",
                f"contract schema {name} const differs",
            )
    if resource_schema_properties.get("semantic_replay") != {
        "const": EXPECTED_SEMANTIC_REPLAY
    }:
        fail(
            "materialization.request-invalid",
            "contract schema semantic replay proof differs",
        )
    if resource_schema_properties.get("admission_failure") != {
        "const": EXPECTED_ADMISSION_FAILURE
    }:
        fail(
            "materialization.request-invalid",
            "contract schema admission failure taxonomy differs",
        )

    semantic_replay = (
        contract.get("surface", {}).get("resource_limits", {}).get("semantic_replay")
    )
    if semantic_replay != EXPECTED_SEMANTIC_REPLAY:
        fail(
            "materialization.request-invalid",
            "contract semantic replay proof differs",
        )
    if content_digest(canonical_json(EXPECTED_SEMANTIC_REPLAY_PROJECTION)) != (
        EXPECTED_SEMANTIC_REPLAY["derivation_digest"]
    ):
        fail(
            "materialization.request-invalid",
            "semantic replay derivation digest differs",
        )

    expected_raw_phase_codes = {
        "input-limit": {"const": "materialization.request-invalid"},
        "json-decode": {
            "enum": [
                "materialization.request-invalid",
                "materialization.spool-failure",
            ]
        },
        "request-envelope": {"const": "materialization.request-invalid"},
        "request-version": {"const": "materialization.version-unsupported"},
        "request-schema": {
            "enum": [
                "materialization.request-invalid",
                "materialization.spool-failure",
            ]
        },
        "request-binding": {
            "enum": [
                "materialization.identity-mismatch",
                "materialization.catalog-census-mismatch",
                "materialization.task-binding-mismatch",
                "materialization.descriptor-binding-mismatch",
                "materialization.spool-failure",
            ]
        },
    }
    observed_raw_phase_codes: dict[str, Any] = {}
    for condition in report_schema.get("allOf", []):
        try:
            phase = condition["if"]["properties"]["error"]["properties"][
                "phase"
            ]["const"]
            code = condition["then"]["properties"]["error"]["properties"][
                "code"
            ]
        except (KeyError, TypeError):
            continue
        if phase in expected_raw_phase_codes:
            observed_raw_phase_codes[phase] = code
    if observed_raw_phase_codes != expected_raw_phase_codes:
        fail(
            "materialization.report-invalid",
            "report raw admission phase/code cross-product differs",
        )

    expected_spool_failure_closure = {
        "if": {
            "properties": {
                "error": {
                    "type": "object",
                    "properties": {
                        "code": {"const": "materialization.spool-failure"}
                    },
                    "required": ["code"],
                }
            },
            "required": ["error"],
        },
        "then": {
            "properties": {
                "response_kind": {"const": "compact_failure"},
                "result": {"const": "failed"},
                "process_exit_status": {"const": 1},
                "raw_input_observation": {
                    "properties": {"complete": {"const": True}}
                },
                "binding": {
                    "properties": {
                        "state": {"const": "raw-input-only"},
                        "request": {"type": "null"},
                    },
                    "required": ["state", "request"],
                },
                "error": {
                    "properties": {
                        "phase": {
                            "enum": [
                                "json-decode",
                                "request-schema",
                                "request-binding",
                            ]
                        }
                    },
                    "required": ["phase"],
                },
            },
            "required": ["response_kind", "binding", "error"],
        },
    }
    observed_spool_failure_closures = []
    for condition in report_schema.get("allOf", []):
        try:
            code = condition["if"]["properties"]["error"]["properties"][
                "code"
            ]["const"]
        except (KeyError, TypeError):
            continue
        if code == "materialization.spool-failure":
            observed_spool_failure_closures.append(condition)
    if observed_spool_failure_closures != [expected_spool_failure_closure]:
        fail(
            "materialization.report-invalid",
            "report spool failure reverse phase/binding closure differs",
        )

    def contains_legacy_report_lifecycle(value: Any) -> bool:
        if isinstance(value, dict):
            return any(contains_legacy_report_lifecycle(item) for item in value.values())
        if isinstance(value, list):
            return any(contains_legacy_report_lifecycle(item) for item in value)
        return value == "bounded-spool-before-publication"

    if contains_legacy_report_lifecycle(contract):
        fail(
            "materialization.report-invalid",
            "legacy prepublication-complete report lifecycle was reintroduced",
        )
    report_construction = (
        contract.get("surface", {})
        .get("resource_limits", {})
        .get("report_construction")
    )
    if report_construction != EXPECTED_REPORT_CONSTRUCTION:
        fail(
            "materialization.report-invalid",
            "bounded two-phase report lifecycle differs",
        )
    if contract["surface"] != {
        "selected_option": "installed-provider-owned-machine-contract",
        "executable": "cxxlens-clang22-materialize",
        "worker_executable": "cxxlens-clang-worker-22",
        "public_cpp_api": "none",
        "generic_adoption_api": "none",
        "clang_native_type_exposure": "forbidden",
        "request_schema": "cxxlens.clang22-materialization-request.v2",
        "report_schema": "cxxlens.clang22-materialization-report.v2",
        "transport": "one-json-request-on-stdin-one-json-response-on-stdout",
        "stderr": "diagnostic-only",
        "shell": "forbidden",
        "cli": {
            "accepted": "argc-exactly-one-no-options-no-operands",
            "argv0": "diagnostic-only",
            "invalid": (
                "before-stdin-authentication-exit-two-zero-stdout-zero-worker-and-"
                "store-effects"
            ),
            "diagnostic": "bounded-stderr-only",
        },
        "json_lexical_policy": EXPECTED_JSON_LEXICAL_POLICY,
        "json_lexical_errors": {
            "request": "materialization.request-invalid",
            "report": "materialization.report-invalid",
        },
        "private_spool": {
            "platform": "linux-only",
            "creation_operation": "memfd_create",
            "creation_flags": ["MFD_CLOEXEC", "MFD_ALLOW_SEALING"],
            "backing": "private-memfd-only",
            "pathname-or-mkstemp-fallback": "forbidden",
            "seal_addition_operation": "F_ADD_SEALS",
            "seal_required_bits": [
                "F_SEAL_WRITE",
                "F_SEAL_GROW",
                "F_SEAL_SHRINK",
                "F_SEAL_SEAL",
            ],
            "seal_verification_operation": "F_GET_SEALS",
            "logical_sealed_publication": (
                "after-required-bits-actual-size-and-sealed-content-verified"
            ),
            "actual_size_verification": (
                "fstat-equals-successful-append-census-after-kernel-seal"
            ),
            "sealed_content_verification": (
                "sealed-byte-sha256-equals-successful-append-transcript-sha256"
            ),
            "capability_gate": (
                "first-raw-spool-create-add-get-and-required-bit-observation-"
                "before-effects"
            ),
            "capability_unavailable": (
                "typed-failure-before-worker-store-or-file-effects"
            ),
            "later-create-or-seal-failure": (
                "phase-authentic-typed-failure-without-mutable-downgrade"
            ),
            "pre_seal_mutation": (
                "content-grow-or-shrink-drift-typed-failure-before-effects"
            ),
            "post_seal_mutation": (
                "write-grow-shrink-and-further-seal-addition-forbidden"
            ),
        },
        "request_validation_pipeline": {
            "order": [
                "byte-limit",
                "strict-json-object",
                "request-envelope",
                "version-dispatch",
                "selected-version-full-schema",
                "derived-identity-and-binding",
            ],
            "request_envelope_fields": ["schema", "request_version"],
            "unsupported_version_phase": "request-version",
            "supported_version_schema_failure_phase": "request-schema",
            "identity_or_binding_failure_phase": "request-binding",
            "first-failing-boundary-is-authoritative": True,
            "streaming_lifecycle": [
                "capture-limit-plus-one-into-one-immutable-raw-spool",
                "pass-one-strict-utf8-json-duplicates-envelope-and-version",
                "pass-two-selected-v2-schema-and-bottom-up-binding",
                "streaming-base64-validation-and-source-receipts",
                "seal-complete-request-before-effects",
                "replay-one-canonical-task-at-a-time",
            ],
            "pass_one_dom": "forbidden",
            "pass_two_global_catalog_owner": "exactly-one-immutable-value",
            "task_index": "compact-spool-backed",
            "raw_json_token_is_decoded_string_authority": False,
            "replay": (
                "same-strict-json-string-decoder-and-receipt-revalidation"
            ),
        },
        "input_observation": {
            "maximum_request_bytes": RAW_INPUT_BYTE_LIMIT,
            "maximum_consumed_bytes": RAW_INPUT_BYTE_LIMIT + 1,
            "within_limit": "complete-byte-count-and-sha256",
            "over_limit": "exact-limit-plus-one-prefix-count-and-sha256-complete-false",
            "unread_suffix_claim": "forbidden",
        },
        "resource_limits": {
            "maximum_task_count": 4096,
            "maximum_task_sandbox_requirements": MAXIMUM_TASK_SANDBOX_REQUIREMENTS,
            "maximum_decoded_source_bytes_per_task": 16777216,
            "maximum_aggregate_decoded_source_bytes": 536870912,
            "maximum_content_base64_characters_per_task": 22369624,
            "maximum_response_bytes": RAW_INPUT_BYTE_LIMIT,
            "maximum_json_depth": 64,
            "maximum_json_members_per_object": MAXIMUM_JSON_MEMBERS_PER_OBJECT,
            "maximum_json_member_name_utf8_bytes": 256,
            "maximum_request_schema_capture_utf8_bytes": (
                MAXIMUM_REQUEST_SCHEMA_CAPTURE_BYTES
            ),
            "maximum_request_version_capture_utf8_bytes": (
                MAXIMUM_REQUEST_VERSION_CAPTURE_BYTES
            ),
            "semantic_replay": EXPECTED_SEMANTIC_REPLAY,
            "maximum_strong_id_unicode_scalars": 512,
            "maximum_strong_id_utf8_bytes": 2048,
            "maximum_logical_path_utf8_bytes": 4096,
            "maximum_sqlite_relative_path_utf8_bytes": 4095,
            "maximum_argv_items": 4096,
            "maximum_argv_item_utf8_bytes": 2048,
            "provider_input_chunk_bytes": TASK_INPUT_CHUNK_BYTES,
            "maximum_provider_task_input_bytes": MAXIMUM_TASK_INPUT_BYTES,
            "maximum_provider_input_chunks": MAXIMUM_TASK_INPUT_CHUNKS,
            "raw_request_storage": "bounded-chunk-read-and-private-spool",
            "json_processing": "streaming-strict-utf8-duplicate-aware-no-one-gib-dom",
            "source_decoding": "streaming-base64-to-bounded-private-spool",
            "admission_failure": EXPECTED_ADMISSION_FAILURE,
            "retained_memory_claim": {
                "excluded_resident_sets": [
                    "raw-one-gib-request",
                    "aggregate-source-bytes",
                    "all-task-payloads",
                    "task-count-times-catalog-count-copies",
                ],
                "bound": (
                    "one-shared-catalog-plus-fixed-parser-and-chunk-buffers-plus-one-"
                    "task-index-window-plus-one-decoded-source-plus-one-output-"
                    "validation-window"
                ),
                "task_count_independent_absolute-rss": "not-claimed",
                "task_index_and_bulk-occurrences": "private-spool",
            },
            "report_construction": EXPECTED_REPORT_CONSTRUCTION,
            "allocation_failure": {
                "prepublication_report_construction": (
                    "schema-valid-compact-zero-effect-if-completable-otherwise-exit-two-"
                    "no-response"
                ),
                "after_publication_attempt": (
                    "exit-two-no-response-no-compact-downgrade"
                ),
                "partial_response": "non-authoritative",
            },
            "boundary_tests": [
                "zero",
                "limit-minus-one",
                "limit",
                "limit-plus-one",
                "limit-plus-two",
                "fragmented-short-reads",
            ],
            "injected_small_limit_state_machine_test": "required",
        },
        "process_exit": {
            "passed_detailed": 0,
            "schema_valid_failure": 1,
            "stdout_transport_failure_no_response_authority": 2,
            "error_kind_from_exit_or_stderr": "forbidden",
            "prepublication_report_failure": (
                "schema-valid-compact-zero-effect-exit-one-or-exit-two-no-response"
            ),
            "post_publication_attempt_finalization_failure": (
                "exit-two-no-response-authority-no-compact-downgrade"
            ),
            "post_commit_finalization_or_stdout_failure": (
                "exit-two-no-response-store-record-only-recovery-authority-and-no-"
                "release-evidence"
            ),
            "partial_stdout": "parsed-response-count-zero-and-non-authoritative",
            "post_commit_broken_stdout": (
                "exit-two-no-response-store-record-only-recovery-authority-and-no-release-evidence"
            ),
            "sqlite_blind_retry_after_exit_two": "forbidden",
            "sqlite_recovery": (
                "read-only-exact-selector-parent-candidate-snapshot-and-publication-inspection"
            ),
            "memory_after_exit_two": "process-local-store-lost-fresh-rerun-only",
        },
    }:
        fail("materialization.request-invalid", "installed machine surface is not exact")
    if contract["versioning"] != {
        "machine_contract": MATERIALIZATION_VERSION,
        "request": MATERIALIZATION_VERSION,
        "report": MATERIALIZATION_VERSION,
        "provider_task_input_codec": "cxxlens.clang22.task.v3",
        "observation_native_codec": "cxxlens.clang22.observation-native.v2",
        "provider_protocol": "1.1.0",
        "provider_protocol_required_feature": TASK_INPUT_FEATURE,
        "provider_protocol_minor_zero_fallback": "forbidden",
        "unknown_member": "reject",
        "missing_required_member": "reject",
        "adjacent_version_fallback": "forbidden",
        "migration": "v1-unimplemented-unqualified-superseded-no-implicit-upgrade",
    }:
        fail("materialization.version-unsupported", "version/fallback policy is not exact")
    compact_failure = contract["report"]["response_union"]["compact_failure"]
    compact_report_construction = compact_failure.get("report_construction_phase")
    if (
        compact_report_construction
        != "prepublication-zero-effect-only-before-publish-call"
    ):
        fail(
            "materialization.report-invalid",
            "compact report-construction boundary differs",
        )
    if (
        compact_failure.get("exact_effects")
        != [
            "worker-launch-attempt-count",
            "worker-launch-success-count",
            "store-draft-state",
            "head-observation",
            "publication-attempted",
            "committed-transaction-count",
            "prior-history-retained",
            "first-store-failure-cause-or-null",
        ]
        or compact_failure.get("store_draft_state_authority")
        != {
            "authority": (
                "invocation-logical-unpublished-snapshot-draft-lifecycle-"
                "not-sdk-writer-existence"
            ),
            "not_created": "no-successful-store-open-adoption",
            "discarded": (
                "successful-store-open-adoption-released-before-compact-authority"
            ),
            "writer_begin_receipt_inference": "forbidden",
        }
        or compact_failure.get("head_observation_states")
        != ["not-observed", "absent", "present", "sdk-error"]
        or compact_failure.get("prepublication_store_failure_cause")
        != {
            "exact_fields": [
                "authenticated-operation",
                "access-path-or-null",
                "exact-sdk-code",
                "exact-sdk-field",
                "stable-or-opaque-exact-detail-observation",
            ],
            "operations": [
                "store_open",
                "head_current",
                "writer_begin",
                "partition_stage",
                "closure_stage",
                "writer_validate",
            ],
            "access_path_by_operation": {
                "head_current": "current-selector",
                "all_other_operations": None,
            },
            "head_current_observation": {
                "store.current-not-found": "absent",
                "every_other_sdk_error": "sdk-error",
                "observed_head_publication": None,
            },
            "verification_source": (
                "source-private-first-sdk-error-observation-not-report-self-consistency"
            ),
            "non_store_compact_failure": None,
        }
    ):
        fail(
            "materialization.store-failure",
            "compact prepublication Store cause authority differs",
        )
    if contract["errors"].get("compact_effect_matrix", {}).get("store-stage") != [
        "launch-attempts-task-count",
        "launch-successes-task-count",
        "draft-discarded",
        "head-observed-absent-present-or-sdk-error",
        "sdk-error-only-for-exact-head-current-cause",
        "exact-first-stage-sdk-error-for-store-failure-otherwise-null",
    ]:
        fail(
            "materialization.store-failure",
            "compact store-stage head observation authority differs",
        )
    required_lifecycle_acceptance = {
        "streaming-bounded-request-source-and-two-phase-report-construction",
        "no-completed-report-or-fabricated-publication-values-before-publication",
        "no-compact-downgrade-after-publication-attempt",
    }
    if not required_lifecycle_acceptance.issubset(set(contract["acceptance"])):
        fail(
            "materialization.report-invalid",
            "two-phase report lifecycle acceptance is incomplete",
        )

    if contract["identity"]["verification_ownership"] != {
        "authority_checker_proves": [
            "machine-shape",
            "bounded-phase-authentic-response",
            "exact-registry-descriptor-bindings",
            "descriptor-id-engine-admission",
            "named-engine-and-policy-identities",
            "complete-store-selector",
            "observation-v2-native-row-codec",
            "project-catalog-bottom-up-identities",
            "project-entry-final-relation-cross-binding",
            "portable-provider-task-identity",
            "clang22-task-v3-input-identity",
            "project-task-census-cross-binding",
            "composite-task-result-matching",
            "source-byte-size-and-digest",
            "source-path-and-line-index-bottom-up-identities",
            "semantic-request-projection",
            "report-request-source-authority-cross-binding",
            "claim-partition-snapshot-publication-identities",
        ],
        "implementation_issue_181_shared_codecs_prove": [
            "project-catalog-bottom-up-identities",
            "clang22-task-v3-input-identity",
            "observation-v2-native-row-codec",
            "portable-provider-task-identity",
            "provider-execution-identity",
            "descriptor-id-engine-admission",
            "complete-store-selector",
            "claim-partition-snapshot-publication-identities",
            "reopened-store-exact-comparison",
        ],
        "checker_fixture_is_production_qualification": False,
        "self_consistent_caller_rebinding_without_shared_codec_validation": "reject",
    }:
        fail("materialization.identity-mismatch", "identity verification ownership differs")
    if (
        contract["identity"]["derived_ids_and_digests_recomputed_and_compared"]
        != RECOMPUTED_IDS_AND_DIGESTS
        or contract["identity"]["validated_or_cross_bound_caller_authority"]
        != CROSS_BOUND_CALLER_AUTHORITY
    ):
        fail("materialization.identity-mismatch", "identity ownership sets differ")
    relation_outputs = contract["relation_outputs"]
    if relation_outputs["exact_six_canonical_order"] != DESCRIPTOR_IDS:
        fail("materialization.descriptor-binding-mismatch", "exact six descriptor set differs")
    if relation_outputs["digest_authority"] != {
        "registry": REGISTRY.as_posix(),
        "contract_digest": "sha256-of-canonical-exact-registry-relation-entry",
        "runtime_descriptor_digest": "cxxlens.relation-descriptor-binding.v2",
        "validation": "recompute-both-and-exact-compare-before-worker-launch",
    }:
        fail("materialization.descriptor-binding-mismatch", "descriptor digest authority differs")
    if relation_outputs["observation_v1"] != {
        "status": "transport-only-non-adoptable",
        "adoption": "forbidden",
        "canonical_form_reuse": "forbidden",
        "payload_digest_alias": "forbidden",
        "origin_chain_reinterpretation": "forbidden",
    } or relation_outputs["descriptor_fallback"] != "forbidden":
        fail("materialization.descriptor-binding-mismatch", "v1 or fallback became adoptable")
    if (
        relation_outputs["observation_v2_native_codec"]
        != EXPECTED_OBSERVATION_V2_NATIVE_CODEC
    ):
        fail(
            "materialization.descriptor-binding-mismatch",
            "observation v2 native row codec differs",
        )
    if contract["base_claims"] != EXPECTED_BASE_CLAIM_CONTRACT:
        fail("materialization.claim-invalid", "base claim construction contract differs")
    if contract["source_identity"] != EXPECTED_SOURCE_IDENTITY_CONTRACT:
        fail("materialization.identity-mismatch", "source identity contract differs")
    if contract["identity"].get("installed_occurrence") != EXPECTED_INSTALLED_OCCURRENCE:
        fail(
            "materialization.identity-mismatch",
            "installed occurrence measurement contract differs",
        )
    topology = contract["group_topology"]
    expected_topology = {
        "dependency_groups": ["canonical", "observation"],
        "atomic_output_group": "clang22-atomic",
        "descriptor_groups": GROUP_DESCRIPTORS,
        "batch_id": "descriptor-id-plus--batch",
        "all_tasks_mandatory": True,
        "all_dependency_groups_mandatory": True,
        "all_batches_mandatory": True,
        "partial_policy": "forbid",
        "unsealed_or_missing": "materialization.group-incomplete",
    }
    if topology != expected_topology:
        fail("materialization.group-incomplete", "mandatory group topology differs")
    optionality = contract["span_adoption"]["optionality"]
    if optionality != {
        "entity_and_call": "optional-all-or-none",
        "absent": "retain-observation-with-typed-unresolved-and-non-exact-guarantee",
        "entity_absent_canonicalization": "canonical-entity-may-remain-without-source-anchor",
        "call_absent_canonicalization": "omit-cc-call-site-and-source-dependent-canonical-row",
        "partial": "reject-entire-materialization",
    }:
        fail("materialization.span-invalid", "primary span optionality differs")
    if contract["span_adoption"]["report_census"] != {
        "entity_plus_call_observation_rows": (
            "observed-bundle-count-plus-absent-bundle-count"
        ),
        "unique_bundle_count": "less-than-or-equal-observed-bundle-count",
        "entity_absent_bundle_count": (
            "less-than-or-equal-entity-observation-rows"
        ),
        "call_absent_bundle_count": "less-than-or-equal-call-observation-rows",
        "passed_constructed_source_span_claim_count": "exact-unique-bundle-count",
        "validated_bundle_binding": (
            "exact-bundle-and-bundle-digest-and-constructed-row-digest-and-originating-"
            "semantic-task-context"
        ),
        "bundle_task_binding_set_digest": "required-and-recomputed",
    }:
        fail("materialization.span-invalid", "span report census contract differs")
    project_tasks = contract["project_and_tasks"]
    if project_tasks["semantic_request_binding"] != {
        "domain": "cxxlens.clang22-semantic-request.v2",
        "codec": "cxxlens-canonical-tuple-v1",
        "object_encoding": "canonical-sorted-key-entry-tuples",
        "includes": [
            "tool",
            "worker",
            "project",
            "authority-registry-and-exact-twelve-engine-descriptors",
            "engine-generation",
            "interpretation-policy",
            "trust-policy",
            "complete-seven-field-selector",
            "group-topology",
            "complete-base-claim-row-payloads",
            "per-tu-source-build-task-condition-budget-sandbox",
            "publication-genesis-partial-policy-transaction-count-reopen-policy",
        ],
        "excludes": [
            "materialization-request-identity-fields",
            "derived-publication-series-id",
            "publication-backend",
            "publication-sqlite-path",
            "publication-expected-parent",
        ],
        "memory_sqlite_equality": (
            "required-for-fresh-genesis-within-package-configuration"
        ),
        "sqlite-genesis-append-equality": "not-claimed",
    }:
        fail("materialization.task-binding-mismatch", "semantic request projection differs")
    if project_tasks["project_catalog"] != {
        "authority": "validated-project-catalog",
        "public_codec": "cxxlens.project-catalog.v1",
        "request_fields": [
            "catalog_id",
            "catalog_digest",
            "logical_root",
            "catalog_environment_digest",
            "catalog_compile_units",
        ],
        "entry_fields": list(CATALOG_COMPILE_UNIT_FIELDS),
        "public_entry_mapping": "catalog_compile_unit_id-to-compile_unit_id",
        "ordering": "catalog-compile-unit-id-byte-order",
        "validation": (
            "independent-bottom-up-recompute-canonical-projection-digest-and-id"
        ),
        "empty": "reject",
        "duplicate_or_conflict": "reject",
    }:
        fail("materialization.identity-mismatch", "project catalog binding differs")
    if project_tasks["project_census"] != {
        "digest_domain": "cxxlens.clang22-catalog-compile-unit-census.v1",
        "digest_projection": "canonical-ordered-catalog-compile-unit-id-tuple",
        "equality": (
            "selected-catalog-compile-unit-set-exactly-equals-catalog-census"
        ),
        "task_order": "selected-catalog-compile-unit-id-byte-order",
    }:
        fail("materialization.catalog-census-mismatch", "project census binding differs")
    if project_tasks["effective_invocation_codec"] != EXPECTED_EFFECTIVE_INVOCATION_CODEC:
        fail(
            "materialization.identity-mismatch",
            "effective invocation codec or cross-binding differs",
        )
    if project_tasks["catalog_entry_mapping"] != {
        "match": "selected-catalog-compile-unit-id",
        "payload": [
            "normalized-invocation-digest",
            "source-content-digest",
            "environment-digest",
        ],
        "final_relation_id": "independently-derived-build-compile-unit-id",
        "catalog-final-id-alias": "forbidden",
        "cardinality": "every-catalog-entry-exactly-one-task",
    }:
        fail("materialization.task-binding-mismatch", "catalog entry mapping differs")
    if project_tasks["portable_provider_task"] != {
        "codec": "cxxlens.provider-task.v1",
        "canonical_projection": [
            "contract-tag",
            "provider-id",
            "provider-version",
            "provider-semantic-contract-digest",
            "global-catalog-id",
            "global-catalog-digest",
            "requested-output-descriptors",
            "derived-condition-ref-id",
            "interpretation",
            "offered-output-descriptors",
            "empty-required-relations",
            "interpretation-domains",
            "input-stage",
            "output-stage",
            "dependency-groups",
        ],
        "condition_binding": {
            "source": ["condition-universe-id", "condition-id"],
            "codec": "cxxlens-canonical-tuple-v1",
            "projection": [
                "cxxlens.clang22.condition-ref.v1",
                "condition-universe-id",
                "condition-id",
            ],
            "digest_domain": "cxxlens.clang22.condition-ref.v1",
            "generic_condition_value": (
                "condition-ref-prefix-plus-semantic-digest-v2"
            ),
            "validation": (
                "recompute-before-portable-task-id-and-exact-cross-bind-worker-v3"
            ),
        },
        "descriptor_digest": "exact-runtime-descriptor-digest",
        "session": [
            "exact-six-offered-outputs",
            "empty-required-relations",
            "cc-clang22-canonical-1-only",
            "observation-input-stage",
            "assertion-output-stage",
        ],
        "per_tu_excluded": [
            "task-input-payload",
            "selected-catalog-compile-unit-id",
            "final-relation-compile-unit-id",
            "provider-execution-id",
        ],
        "task_id": "task-prefix-plus-semantic-digest-v2",
        "validation": "independent-bottom-up-recompute-before-task-accepted",
    }:
        fail("materialization.identity-mismatch", "portable provider task binding differs")
    if project_tasks["worker_task_v3"] != {
        "codec": "cxxlens.clang22.task.v3",
        "canonical_integer_domain": "signed-int64",
        "canonical_projection": [
            "contract-tag",
            "full-global-project-catalog",
            "selected-catalog-compile-unit-id",
            "final-relation-compile-unit-id",
            "exact-per-tu-task-payload",
        ],
        "full_global_project_catalog": [
            "catalog_id",
            "catalog_digest",
            "logical_root",
            "catalog_environment_digest",
            "catalog_compile_units",
        ],
        "selected_entry_validation": (
            "exact-invocation-source-environment-digest-match"
        ),
        "worker_reconstruction": "shared-project-catalog-factory-before-output",
        "payload_authority": "installed-tool-derived-only",
        "task_input_digest": "sha256-content-digest-of-exact-projection-bytes",
        "old_codec_or_caller_payload": "reject",
        "source_content_base64": {
            "input": "canonical-rfc4648-decoded-json-string",
            "projection": (
                "unique-canonical-spelling-derived-from-sealed-source-bytes"
            ),
            "raw_json_escape_affects_task_identity": False,
            "nonzero_discarded_padding_bits": (
                "reject-before-task-input-digest"
            ),
            "cross_binding": (
                "decoded-bytes-size-content-digest-line-index-and-request-string"
            ),
        },
        "logical_transfer": {
            "authority": "exact-canonical-task-v3-bytes-and-task-input-digest",
            "fragmentation_changes-logical-identity": False,
            "ambient-path-or-fd-authority": "forbidden",
        },
        "physical_transfer": {
            "provider_protocol": "1.1.0",
            "required_feature": TASK_INPUT_FEATURE,
            "mode": "authenticated-input-descriptor-and-canonical-chunks",
            "open_task_payload": "empty",
            "canonical_chunk_bytes": TASK_INPUT_CHUNK_BYTES,
            "maximum_task_input_bytes": MAXIMUM_TASK_INPUT_BYTES,
            "maximum_chunk_count": MAXIMUM_TASK_INPUT_CHUNKS,
            "task_accepted": (
                "only-after-length-order-final-digest-task-v3-and-portable-task-"
                "validation"
            ),
            "minor-zero-inline-fallback": "forbidden",
        },
        "maximum_projection_proof": maximum_worker_task_v3_projection_proof(
            request_schema
        ),
    }:
        fail("materialization.task-binding-mismatch", "worker task v3 binding differs")
    if project_tasks["task_execution_matching"] != {
        "key": list(TASK_EXECUTION_KEY_FIELDS),
        "provider_task_id_uniqueness": "not-required",
        "duplicate_missing_or_extra": "reject",
        "report_order": "non-authoritative",
        "correlation_binding": "exact-composite-execution-key",
        "semantic_context_sort": (
            "task-id-then-input-then-selected-catalog-id-then-final-relation-id"
        ),
        "semantic_provenance_binding": (
            "task-id-input-selected-catalog-id-final-relation-id"
        ),
        "physical_occurrence": "provider-execution-id-report-only",
    }:
        fail("materialization.task-binding-mismatch", "task execution matching differs")
    if (
        contract["span_adoption"]["all_or_none_fields"]
        != PRIMARY_SPAN_BUNDLE_FIELDS
        or contract["span_adoption"]["seventh_worker_relation"] != "forbidden"
    ):
        fail("materialization.span-invalid", "full span bundle contract differs")
    expected_registry_binding = {
        "authority": REGISTRY.as_posix(),
        "abstract_to_registry_column_id": SPAN_REGISTRY_COLUMN_MAPPING,
        "all_or_none": "exact-single-mapped-seven-as-unordered-set",
        "origin_evidence_column_id": SPAN_ORIGIN_COLUMN_MAPPING,
        "origin_evidence_shape": (
            "optional-bytes-excluded-from-primary-mapping-and-all-or-none"
        ),
        "validation": (
            "exact-column-id-name-type-optionality-row-constraint-and-origin-separation-"
            "before-row-adoption"
        ),
    }
    if contract["span_adoption"]["registry_column_binding"] != expected_registry_binding:
        fail("materialization.span-invalid", "primary span registry mapping differs")
    if contract["span_adoption"]["independent_validator"] != {
        "owner": "installed-tool-private",
        "timing": "after-row-decode-before-sealed-result",
        "independence": [
            "relation-row-builder",
            "relation-reference-absence-shortcut",
            "registry-row-constraints",
            "public-generic-engine",
        ],
        "presence_rule": "entity-and-call-seven-fields-all-present-or-all-absent",
        "partial_effect": "reject-entire-materialization",
        "absent_effect": (
            "typed-unresolved-non-exact-and-source-dependent-canonical-omission"
        ),
        "public_api": "none",
    }:
        fail("materialization.span-invalid", "independent span validator differs")
    if contract["side_channels"]["unresolved"] != {
        "record_type": "typed-unresolved-item",
        "control_from_message_prose": "forbidden",
        "blocking_item_with_exact_claim": "forbidden",
        "category_order": "lexical",
        "category_count_encoding": "sparse-positive-map",
        "record_count_closure": "exact-sum-of-category-counts",
        "absent_bundle_category": PRIMARY_SPAN_ABSENCE_CATEGORY,
        "absent_bundle_count_binding": (
            "span-validation-absent-bundle-unresolved-count"
        ),
    }:
        fail("materialization.coverage-incomplete", "unresolved accounting differs")
    if contract["side_channels"].get("coverage") != EXPECTED_COVERAGE_CONTRACT:
        fail(
            "materialization.coverage-incomplete",
            "transport/semantic coverage plane contract differs",
        )
    if contract["report"].get("digest_chain") != EXPECTED_REPORT_DIGEST_CHAIN:
        fail("materialization.report-invalid", "report digest chain differs")
    adoption = contract["claim_adoption"]
    try:
        claim_adoption_schema = contract_schema["properties"]["claim_adoption"]
        schema_resolution = contract_schema["$defs"][
            "df_0200_resolution"
        ]["const"]
    except (KeyError, TypeError):
        claim_adoption_schema = {}
        schema_resolution = None
    if (
        adoption.get("df_0200_resolution")
        != EXPECTED_DF_0200_RESOLUTION
        or schema_resolution != EXPECTED_DF_0200_RESOLUTION
        or "df_0200_resolution"
        not in claim_adoption_schema.get("required", [])
        or claim_adoption_schema.get("properties", {}).get(
            "df_0200_resolution"
        )
        != {"$ref": "#/$defs/df_0200_resolution"}
    ):
        fail(
            "materialization.claim-invalid",
            "DF-0200 accepted incremental residency resolution differs",
        )
    try:
        resolution = adoption["df_0200_resolution"]
        validate_df_0200_codec_receipt_closure(
            resolution["partition_event_codec"],
            resolution["d3_store_ingestion"]["external_completeness_authority"],
        )
        store_ingress = snapshot_store_contract[
            "df_0200_materialization_ingress"
        ]
        store_resolution_id = store_ingress["resolution_id"]
        store_events = store_ingress["source"]["events"]
        store_codec = store_ingress["source"]["codec"]
        store_external_authority = store_ingress["source"][
            "external_completeness_authority"
        ]
        store_recomputation = store_ingress["source"]["store-validation"][
            "required-recomputation"
        ]
        store_collection_overflow = store_ingress["counter_model"][
            "collection_overflow_failure"
        ]
        store_sqlite_capacity_decision = store_ingress[
            "sqlite_capacity_decision"
        ]
        store_sqlite_writer_failure = store_ingress["sqlite_backend"][
            "writer_publish_enospc_or_sqlite_toobig"
        ]
    except (KeyError, TypeError):
        store_resolution_id = None
        store_events = None
        store_codec = None
        store_external_authority = None
        store_recomputation = None
        store_collection_overflow = None
        store_sqlite_capacity_decision = None
        store_sqlite_writer_failure = None
    expected_event_kind_codes = {
        name: projection["kind_code"]
        for name, projection in DF_0200_PARTITION_EVENT_CODEC[
            "event_projections"
        ].items()
    }
    expected_codec_authority_binding = {
        "canonical_json_sha256": content_digest(
            canonical_json(resolution["partition_event_codec"])
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
    }
    if (
        resolution["resolution_id"] != store_resolution_id
        or resolution["d3_store_ingestion"]["source_events"] != store_events
        or not isinstance(store_codec, dict)
        or store_codec.get("id") != resolution["partition_event_codec"]["id"]
        or store_codec.get("event_kind_codes") != expected_event_kind_codes
        or store_codec.get("authority_binding")
        != expected_codec_authority_binding
        or resolution["d3_store_ingestion"]["external_completeness_authority"]
        != store_external_authority
        or resolution["d3_store_ingestion"]["independent_validation"][
            "required_recomputation"
        ]
        != store_recomputation
        or resolution["d3_store_ingestion"][
            "private_ingress_collection_overflow"
        ]
        != store_collection_overflow
        or resolution["sqlite_capacity_decision"]
        != store_sqlite_capacity_decision
        or resolution["d5_failure_taxonomy"][
            "sqlite_writer_publish_enospc_or_sqlite_toobig"
        ]
        != store_sqlite_writer_failure
    ):
        fail(
            "materialization.store-failure",
            "DF-0200 materialization/Store resolution binding differs",
        )
    if (
        adoption["boundary"] != "sealed-materialization-result"
        or adoption["visibility"] != "tool-private-immutable-noncopyable"
        or adoption["public_report_frames"]
        != {
            "authority": "diagnostic-only-non-authoritative",
            "adoption": "forbidden",
            "retention_after_report": "forbidden",
        }
    ):
        fail("materialization.claim-invalid", "sealed/raw-frame boundary differs")
    expected_stages = {
        "base_claims": {
            "descriptors": BASE_DESCRIPTOR_IDS,
            "stage": "canonical_claim",
        },
        "provider_observations": {
            "descriptors": DESCRIPTOR_IDS[3:],
            "stage": "assertion",
        },
        "canonical_outputs": {
            "descriptors": DESCRIPTOR_IDS[:3],
            "stage": "canonical_claim",
        },
    }
    if adoption["stages"] != expected_stages:
        fail("materialization.claim-invalid", "claim-stage mapping differs")
    if adoption["hard_reference_validation"] != {
        "base_claims": BASE_DESCRIPTOR_IDS,
        "staged_claims": "exact-two-groups",
        "missing": "reject-entire-materialization",
    }:
        fail(
            "materialization.claim-invalid",
            "hard-reference validation mapping differs",
        )
    if adoption["guarantee"] != {
        "profile": {
            "id": GUARANTEE_PROFILE_ID,
            "digest_domain": GUARANTEE_PROFILE_ID,
            "owner": "exact-materialization-contract-version",
            "assumptions": GUARANTEE_ASSUMPTIONS,
            "verification_modalities": GUARANTEE_MODALITIES,
            "caller-or-report-builder-mutation": "forbidden",
        },
        "exact_preconditions": [
            "zero-non-exact-in-each-observation-descriptor-census",
            "complete-balanced-semantic-coverage",
            "exact-transport-task-coverage",
            "no-blocking-unresolved",
            "no-absent-primary-span-bundle",
            "exact-task-census",
            "exact-six-batches",
            "full-span-validation",
            "complete-provenance",
        ],
        "postpublication_evidence_excluded": [
            "successful-publication",
            "query-parity",
            "store-reopen",
        ],
        "non_exact": (
            "preserve-typed-approximation-assumptions-modalities-and-unresolved"
        ),
        "inference_from-success-or-prose": "forbidden",
    }:
        fail("materialization.coverage-incomplete", "guarantee preconditions differ")
    if contract["side_channels"]["guarantee"] != {
        "record_type": "typed-guarantee",
        "profile": GUARANTEE_PROFILE_ID,
        "fields": [
            "profile_id",
            "profile_digest",
            "approximation",
            "scope",
            "assumptions",
            "verification_modalities",
            "observation_descriptor_censuses",
        ],
        "task_fragment_inputs": [
            "closed-profile",
            "actual-semantic-coverage-census",
            "unresolved",
            "evidence",
            "batch-completeness",
            "observation-equivalence-census",
        ],
        "global_digest_inputs": [
            "profile-id-and-digest",
            "semantic-and-transport-side-channel-digests",
            "task-guarantee-fragments",
            "observation-descriptor-censuses",
        ],
    }:
        fail("materialization.report-invalid", "typed guarantee fields differ")
    publication = contract["publication"]
    required_publication = {
        "target_per_request": "exactly-one-of-memory-or-sqlite",
        "transaction_per_request": "exactly-one-all-tasks-all-groups",
        "partial_policy": "forbid",
        "expected_parent_cas": "required-null-only-for-genesis",
        "stale_parent": "materialization.stale-parent",
        "sqlite_validation": "close-and-reopen-database-before-success-report",
        "publication_authority": "committed-store-record-not-tool-report",
    }
    if any(publication.get(key) != value for key, value in required_publication.items()):
        fail("materialization.store-failure", "publication/CAS contract differs")
    if publication.get("sdk_publish_mapping") != EXPECTED_SDK_PUBLISH_MAPPING:
        fail(
            "materialization.store-failure",
            "operation-authentic SDK publication mapping differs",
        )
    if publication.get("store_failure_cause_union") != {
        "sdk_error": [
            "authenticated-operation",
            "access-path-or-null",
            "exact-sdk-code",
            "exact-sdk-field",
            "stable-or-opaque-exact-detail-observation",
        ],
        "verification_mismatch": [
            "authenticated-operation",
            "access-path-or-null",
            "named-projection",
            "expected-digest",
            "actual-digest",
        ],
        "fabricated-sdk-error-for-successful-call-mismatch": "forbidden",
    }:
        fail(
            "materialization.store-failure",
            "Store failure cause union differs",
        )
    if publication.get("sqlite_effect_root") != EXPECTED_SQLITE_EFFECT_ROOT:
        fail(
            "materialization.store-failure",
            "rooted SQLite effect authority differs",
        )
    if contract["qualification"] != {
        "configurations": ["static", "shared"],
        "backends": ["memory", "sqlite"],
        "required_matrix": "exact-cartesian-product",
        "relocated_prefix": "required",
        "same_project_request_and_worker_semantics": "required",
        "sqlite_reopened": "required",
        "memory_sqlite_semantic_parity": "required",
        "static_shared_semantic_parity": "required",
        "exact_observation_equivalence": "required-zero-non-exact-per-descriptor",
        "scale_and_resource_evidence": {
            "cases": [
                "one-task",
                "four-thousand-ninety-six-tasks",
                "sixteen-mib-source",
                "five-hundred-twelve-mib-aggregate-source",
                "one-gib-raw-request",
                "arbitrary-short-reads",
            ],
            "retained_memory_formula": (
                "one-shared-catalog-plus-fixed-buffers-plus-one-task-window-plus-one-"
                "source-plus-one-output-window"
            ),
            "forbidden_residency": [
                "raw-request",
                "aggregate-source",
                "all-task-payloads",
                "task-count-times-catalog-count",
            ],
            "spool-failure": "zero-effect-before-publication",
        },
        "execution_receipt": {
            "fields": [
                "actual-exit-status",
                "exact-stdout-byte-count",
                "stdout-sha256",
                "parsed-response-count",
                "stderr-sha256-diagnostic-only",
            ],
            "cross_binding": "stdout-bytes-are-the-exact-report-artifact",
            "passed_report": "actual-exit-zero-and-response-count-one",
            "schema_valid_failure": (
                "actual-exit-one-and-response-count-one-release-forbidden"
            ),
            "no_response_failure": (
                "actual-exit-two-and-response-count-zero-release-forbidden"
            ),
            "report_set_entry": [
                "backend",
                "report-digest",
                "execution-receipt-digest",
            ],
        },
        "missing_extra_duplicate_matrix_entry": "reject",
    }:
        fail("materialization.report-invalid", "installed qualification matrix differs")
    if set(contract["errors"]["stable"]) != STABLE_ERRORS:
        fail("materialization.report-invalid", "stable error registry differs")
    try:
        compact_phase_code_policy = {
            scope: {
                phase: set(codes)
                for phase, codes in phase_codes.items()
            }
            for scope, phase_codes in contract["errors"][
                "compact_phase_code_policy"
            ].items()
        }
    except (AttributeError, KeyError, TypeError):
        fail(
            "materialization.report-invalid",
            "compact failure phase/code policy differs",
        )
    expected_compact_phase_code_policy = {
        "raw_input_only": COMPACT_RAW_PHASE_CODES,
        "request_bound": COMPACT_BOUND_PHASE_CODES,
        "detailed_failed": {
            "publication": {
                "materialization.stale-parent",
                "materialization.store-failure",
            },
            "post-publication-verification": {
                "materialization.store-failure",
            },
        },
    }
    if compact_phase_code_policy != expected_compact_phase_code_policy:
        fail(
            "materialization.report-invalid",
            "compact failure phase/code policy differs",
        )
    if contract["errors"]["diagnostic_prose_control_flow"] != "forbidden":
        fail("materialization.report-invalid", "diagnostic prose became control authority")
    if contract["lifetime"]["raw_frames"] != (
        "diagnostic-only-and-destroyed-after-shared-runtime-receipt-and-"
        "immutable-seal"
    ):
        fail("materialization.claim-invalid", "raw-frame lifetime carve-out differs")


def sample_request(
    root: pathlib.Path,
    *,
    configuration: str = "static",
    backend: str = "memory",
    translation_unit_count: int = 1,
) -> dict[str, Any]:
    if translation_unit_count < 1:
        raise ValueError("translation_unit_count must be positive")
    registry_digest, bindings = descriptor_bindings(root)
    base_bindings = base_descriptor_bindings(root)
    relations = base_registry_relations(root)
    digit = "1" if configuration == "static" else "2"
    logical_root = "project://fixture"
    catalog_environment_digest = "sha256:" + "d" * 64
    source_specs = []
    digest_digits = "fedcba9876543210"
    for index in range(translation_unit_count):
        logical_path = "project://main.cpp" if index == 0 else f"project://unit_{index}.cpp"
        source = (
            b"int main() { return 0; }\n"
            if index == 0
            else f"int unit_{index}() {{ return {index}; }}\n".encode("utf-8")
        )
        effective_argv = ["clang++", "-std=c++23", logical_path]
        source_specs.append(
            {
                "catalog_compile_unit_id": f"catalog-unit:{index:04d}",
                "logical_path": logical_path,
                "source": source,
                "effective_argv": effective_argv,
                "normalized_invocation_digest": semantic_digest(
                    "cxxlens.clang22.effective-invocation.v1",
                    effective_invocation_projection(logical_root, effective_argv),
                ),
                "environment_digest": (
                    "sha256:" + digest_digits[(index + 2) % len(digest_digits)] * 64
                ),
            }
        )
    project = {
        "project_id": "pending",
        "catalog_id": "pending",
        "catalog_digest": "pending",
        "logical_root": logical_root,
        "catalog_environment_digest": catalog_environment_digest,
        "catalog_compile_unit_census_digest": "pending",
        "catalog_compile_units": [
            {
                "catalog_compile_unit_id": spec["catalog_compile_unit_id"],
                "effective_invocation_digest": spec[
                    "normalized_invocation_digest"
                ],
                "source_digest": content_digest(spec["source"]),
                "environment_digest": spec["environment_digest"],
            }
            for spec in source_specs
        ],
    }
    bind_project_catalog_identity(project)
    project_row = {
        "catalog": project["catalog_id"],
        "catalog_digest": project["catalog_digest"],
        "logical_root": logical_root,
        "environment_digest": catalog_environment_digest,
    }
    project_id = derive_base_row_identity(relations["build.project.v1"], project_row)
    project["project_id"] = project_id
    toolchain_payload = {
        "family": "clang",
        "exact_version": "22.0.0",
        "target_triple": "x86_64-unknown-linux-gnu",
        "builtin_headers_digest": "sha256:" + "3" * 64,
        "sysroot": None,
        "abi_digest": "sha256:" + "4" * 64,
        "plugin_spec_digest": "sha256:" + "5" * 64,
    }
    toolchain_row = dict(toolchain_payload)
    toolchain_id = derive_base_row_identity(
        relations["build.toolchain_context.v1"], toolchain_row
    )
    toolchain_row["toolchain"] = toolchain_id
    variant_payload = {
        "language": "cxx",
        "language_standard": "cxx23",
        "target_triple": toolchain_payload["target_triple"],
        "predefined_macros_digest": "sha256:" + "6" * 64,
        "include_search_digest": "sha256:" + "7" * 64,
        "semantic_flags_digest": "sha256:" + "8" * 64,
    }
    variant_row = {
        "project": project_id,
        "toolchain": toolchain_id,
        **variant_payload,
    }
    variant_id = derive_base_row_identity(relations["build.variant.v1"], variant_row)
    occurrence = fixture_occurrence_measurement(
        root,
        source_revision="1" * 40,
        source_tree="2" * 40,
        configuration=configuration,
        tool_digest="sha256:" + digit * 64,
        worker_digest="sha256:" + digit * 64,
    )
    request = {
        "schema": "cxxlens.clang22-materialization-request.v2",
        "request_version": MATERIALIZATION_VERSION,
        "materialization_request_id": "pending",
        "request_digest": semantic_digest("cxxlens.fixture.v1", "pending-request"),
        "semantic_request_digest": semantic_digest("cxxlens.fixture.v1", "pending-semantic"),
        "tool": {
            "executable": "cxxlens-clang22-materialize",
            "interface_version": MATERIALIZATION_VERSION,
            "distribution_version": "1.0.0",
            "source_revision": "1" * 40,
            "source_tree": "2" * 40,
            "installed_executable_digest": "sha256:" + digit * 64,
            "package_configuration": configuration,
            "occurrence_manifest_digest": occurrence["manifest_file_digest"],
        },
        "worker": {
            "executable": "cxxlens-clang-worker-22",
            "provider_id": "cxxlens.clang22.reference",
            "provider_version": "1.0.0",
            "installed_binary_digest": "sha256:" + digit * 64,
            "semantic_contract_digest": "sha256:" + "a" * 64,
            "protocol_major": 1,
            "protocol_minor": PROVIDER_PROTOCOL_MINOR,
            "required_features": [TASK_INPUT_FEATURE],
            "sandbox_policy_digest": "sha256:" + "b" * 64,
        },
        "project": project,
        "registry": {
            "path": REGISTRY.as_posix(),
            "authority_registry_digest": registry_digest,
            "base_descriptors": copy.deepcopy(base_bindings),
            "descriptors": copy.deepcopy(bindings),
        },
        "engine": {},
        "interpretation_policy": {},
        "trust_policy": {},
        "group_topology": {
            "dependency_groups": ["canonical", "observation"],
            "atomic_output_group": "clang22-atomic",
            "partial_policy": "forbid",
        },
        "tasks": [],
        "publication": {
            "backend": backend,
            "selector": {},
            "series_id": "pending",
            "genesis": True,
            "expected_parent_publication": None,
            "sqlite_path": "materialization.sqlite" if backend == "sqlite" else None,
            "partial_policy": "forbid",
            "transaction_count": 1,
            "reopen_before_success": True,
        },
    }
    pending_provider_task_id = "task:" + semantic_digest(
        "cxxlens.fixture.v1", "pending-provider-task"
    )
    for spec in source_specs:
        source = spec["source"]
        source_row = {
            "file": file_identity(spec["logical_path"]),
            "project": project_id,
            "logical_path": spec["logical_path"],
            "content": content_digest(source),
            "size": len(source),
            "encoding": "utf8",
            "line_index": line_index_identity(source),
            "read_only": False,
        }
        source_snapshot_id = derive_base_row_identity(
            relations["source.file.v1"], source_row
        )
        compile_unit_row = {
            "project": project_id,
            "main_source": source_snapshot_id,
            "variant": variant_id,
            "toolchain": toolchain_id,
            "effective_invocation_digest": spec[
                "normalized_invocation_digest"
            ],
            "language": variant_payload["language"],
            "working_directory": logical_root,
        }
        compile_unit_id = derive_base_row_identity(
            relations["build.compile_unit.v1"], compile_unit_row
        )
        request["tasks"].append(
            {
                "provider_task_id": pending_provider_task_id,
                "provider_execution_id": "pending",
                "task_input_digest": semantic_digest(
                    "cxxlens.fixture.v1", "pending-task-input"
                ),
                "project_id": project_id,
                "catalog_id": project["catalog_id"],
                "catalog_digest": project["catalog_digest"],
                "selected_catalog_compile_unit_id": spec[
                    "catalog_compile_unit_id"
                ],
                "compile_unit_id": compile_unit_id,
                "build_variant_id": variant_id,
                "toolchain_context_id": toolchain_id,
                "toolchain_digest": base_claim_row_digest(
                    "build.toolchain_context.v1", toolchain_row
                ),
                "toolchain": toolchain_payload,
                "variant": variant_payload,
                "normalized_invocation_digest": spec[
                    "normalized_invocation_digest"
                ],
                "environment_digest": spec["environment_digest"],
                "language": variant_payload["language"],
                "working_directory": logical_root,
                "condition_universe_id": "condition-universe:one",
                "condition_id": "condition:all",
                "interpretation_domain": "cc.clang22-canonical-1",
                "source": {
                    "source_snapshot_id": source_snapshot_id,
                    "file_id": source_row["file"],
                    "logical_path": source_row["logical_path"],
                    "content_digest": content_digest(source),
                    "size_bytes": len(source),
                    "encoding": "utf8",
                    "line_index_id": source_row["line_index"],
                    "read_only": source_row["read_only"],
                    "content_base64": base64.b64encode(source).decode("ascii"),
                },
                "effective_argv": spec["effective_argv"],
                "requested_descriptor_ids": DESCRIPTOR_IDS,
                "dependency_groups": ["canonical", "observation"],
                "budget": {
                    "output_bytes": 1048576,
                    "rows": 1024,
                    "diagnostics": 128,
                    "wall_ms": 10000,
                    "cpu_ms": 10000,
                    "address_space_bytes": 1073741824,
                    "transport_bytes": 2097152,
                    "open_files": 64,
                    "subprocesses": 1,
                },
                "sandbox": {
                    "minimum": "enforced",
                    "policy_digest": "sha256:" + "b" * 64,
                },
            }
        )
    bind_provider_task_identities(request)
    bind_task_execution_identities(request)
    bind_engine_policy_and_selector_identities(request)
    bind_request_identity(request)
    return request


def rebind_request_base_identities(
    root: pathlib.Path,
    request: dict[str, Any],
) -> None:
    """Rebind an acceptance request after an authoritative catalog payload change."""

    relations = base_registry_relations(root)
    project = request["project"]
    bind_project_catalog_identity(project)
    project_row = {
        "catalog": project["catalog_id"],
        "catalog_digest": project["catalog_digest"],
        "logical_root": project["logical_root"],
        "environment_digest": project["catalog_environment_digest"],
    }
    project_id = derive_base_row_identity(relations["build.project.v1"], project_row)
    project["project_id"] = project_id
    for task in request["tasks"]:
        task["project_id"] = project_id
        task["catalog_id"] = project["catalog_id"]
        task["catalog_digest"] = project["catalog_digest"]
        toolchain_row = dict(task["toolchain"])
        toolchain_id = derive_base_row_identity(
            relations["build.toolchain_context.v1"],
            toolchain_row,
        )
        toolchain_row["toolchain"] = toolchain_id
        task["toolchain_context_id"] = toolchain_id
        task["toolchain_digest"] = base_claim_row_digest(
            "build.toolchain_context.v1",
            toolchain_row,
        )
        variant_row = {
            "project": project_id,
            "toolchain": toolchain_id,
            **task["variant"],
        }
        variant_id = derive_base_row_identity(
            relations["build.variant.v1"],
            variant_row,
        )
        task["build_variant_id"] = variant_id
        source = decode_canonical_base64(task["source"]["content_base64"])
        source_row = {
            "file": file_identity(task["source"]["logical_path"]),
            "project": project_id,
            "logical_path": task["source"]["logical_path"],
            "content": content_digest(source),
            "size": len(source),
            "encoding": task["source"]["encoding"],
            "line_index": line_index_identity(source),
            "read_only": task["source"]["read_only"],
        }
        source_snapshot_id = derive_base_row_identity(
            relations["source.file.v1"],
            source_row,
        )
        task["source"].update(
            {
                "source_snapshot_id": source_snapshot_id,
                "file_id": source_row["file"],
                "content_digest": source_row["content"],
                "size_bytes": source_row["size"],
                "line_index_id": source_row["line_index"],
            }
        )
        compile_unit_row = {
            "project": project_id,
            "main_source": source_snapshot_id,
            "variant": variant_id,
            "toolchain": toolchain_id,
            "effective_invocation_digest": task["normalized_invocation_digest"],
            "language": task["language"],
            "working_directory": task["working_directory"],
        }
        task["compile_unit_id"] = derive_base_row_identity(
            relations["build.compile_unit.v1"],
            compile_unit_row,
        )
    bind_provider_task_identities(request)
    bind_task_execution_identities(request)
    bind_engine_policy_and_selector_identities(request)
    bind_request_identity(request)


def validate_request(root: pathlib.Path, request: dict[str, Any]) -> None:
    request_schema = load(root / REQUEST_SCHEMA)
    validate_schema(request, request_schema, "materialization request")
    validate_request_utf8_byte_limits(request, request_schema)
    if request["publication"]["sqlite_path"] is not None:
        canonical_sqlite_relative_path(request["publication"]["sqlite_path"])
    semantic_request = expected_semantic_request_digest(request)
    request_digest = expected_request_digest(request)
    if request["semantic_request_digest"] != semantic_request:
        fail("materialization.identity-mismatch", "semantic request digest differs")
    if request["request_digest"] != request_digest or request[
        "materialization_request_id"
    ] != "materialization:" + request_digest:
        fail("materialization.identity-mismatch", "request ID/digest differs")
    registry_digest, bindings = descriptor_bindings(root)
    base_bindings = base_descriptor_bindings(root)
    if request["registry"] != {
        "path": REGISTRY.as_posix(),
        "authority_registry_digest": registry_digest,
        "base_descriptors": base_bindings,
        "descriptors": bindings,
    }:
        fail(
            "materialization.descriptor-binding-mismatch",
            "request descriptor IDs/digests do not match the current registry",
        )
    inventory = admitted_descriptor_inventory(request["registry"])
    inventory_ids = [binding["descriptor_id"] for binding in inventory]
    if (
        inventory_ids != ADMITTED_DESCRIPTOR_IDS
        or len(inventory_ids) != len(set(inventory_ids))
        or request["engine"]["generation_contract"] != ENGINE_GENERATION_CONTRACT
        or request["engine"]["admitted_descriptors"] != inventory
        or request["engine"]["engine_registry_digest"]
        != expected_engine_registry_digest(inventory)
        or request["engine"]["engine_generation_id"]
        != expected_engine_generation_id(request)
    ):
        fail(
            "materialization.descriptor-binding-mismatch",
            "descriptor-ID engine inventory or generation identity differs",
        )
    interpretation_policy = request["interpretation_policy"]
    if (
        interpretation_policy["policy_id"] != INTERPRETATION_POLICY_ID
        or interpretation_policy["selected_domain"] != INTERPRETATION_DOMAIN
        or interpretation_policy["interpretation_policy_digest"]
        != expected_interpretation_policy_digest(interpretation_policy)
    ):
        fail(
            "materialization.identity-mismatch",
            "interpretation policy identity differs",
        )
    trust_policy = request["trust_policy"]
    expected_trust_fields = {
        "provider_id": request["worker"]["provider_id"],
        "provider_version": request["worker"]["provider_version"],
        "semantic_contract_digest": request["worker"]["semantic_contract_digest"],
        "protocol_major": request["worker"]["protocol_major"],
        "protocol_minor": request["worker"]["protocol_minor"],
        "required_features": request["worker"]["required_features"],
        "worker_sandbox_policy_digest": request["worker"]["sandbox_policy_digest"],
    }
    if (
        trust_policy["policy_id"] != TRUST_POLICY_ID
        or trust_policy["execution_profile"] != "trust.native-worker"
        or trust_policy["required_qualification"]
        != "canonical-semantic-qualified"
        or any(
            trust_policy[field] != value
            for field, value in expected_trust_fields.items()
        )
        or trust_policy["task_sandbox_requirements"]
        != task_sandbox_requirements(request["tasks"])
        or trust_policy["trust_policy_digest"]
        != expected_trust_policy_digest(trust_policy)
    ):
        fail("materialization.identity-mismatch", "trust policy identity differs")
    project = request["project"]
    catalog_entries = project["catalog_compile_units"]
    catalog_ids = [entry["catalog_compile_unit_id"] for entry in catalog_entries]
    if catalog_ids != sorted(catalog_ids) or len(catalog_ids) != len(set(catalog_ids)):
        fail(
            "materialization.catalog-census-mismatch",
            "catalog compile-unit entries are not canonical and unique",
        )
    expected_catalog_digest = expected_project_catalog_digest(project)
    if (
        project["catalog_digest"] != expected_catalog_digest
        or project["catalog_id"] != "catalog:" + expected_catalog_digest
    ):
        fail(
            "materialization.identity-mismatch",
            "project catalog digest/ID differs from the public catalog codec",
        )
    if (
        project["catalog_compile_unit_census_digest"]
        != expected_catalog_compile_unit_census_digest(project)
    ):
        fail(
            "materialization.catalog-census-mismatch",
            "catalog compile-unit census digest differs",
        )
    tasks = request["tasks"]
    task_universes = {task["condition_universe_id"] for task in tasks}
    task_interpretations = {task["interpretation_domain"] for task in tasks}
    publication = request["publication"]
    selector = publication["selector"]
    expected_selector = {
        "catalog_id": project["catalog_id"],
        "channel_id": selector["channel_id"],
        "engine_generation_id": request["engine"]["engine_generation_id"],
        "condition_universe_id": next(iter(task_universes)) if task_universes else "",
        "relation_registry_digest": request["engine"]["engine_registry_digest"],
        "interpretation_policy_digest": interpretation_policy[
            "interpretation_policy_digest"
        ],
        "trust_policy_digest": trust_policy["trust_policy_digest"],
    }
    if (
        len(task_universes) != 1
        or task_interpretations != {interpretation_policy["selected_domain"]}
        or not selector["channel_id"]
        or selector != expected_selector
        or publication["series_id"] != expected_series_id(selector)
    ):
        fail(
            "materialization.task-binding-mismatch",
            "complete Store selector or task policy binding differs",
        )
    selected_ids = [task["selected_catalog_compile_unit_id"] for task in tasks]
    if selected_ids != catalog_ids or len(selected_ids) != len(set(selected_ids)):
        fail(
            "materialization.catalog-census-mismatch",
            "tasks do not select every catalog compile unit exactly once",
        )
    for task in tasks:
        if task["provider_task_id"] != expected_provider_task_id(request, task):
            fail(
                "materialization.identity-mismatch",
                "portable provider task identity differs",
            )
    execution_keys = [task_execution_key(task) for task in tasks]
    if len(execution_keys) != len(set(execution_keys)):
        fail(
            "materialization.task-binding-mismatch",
            "request contains a duplicate task execution tuple",
        )
    entries_by_id = {
        entry["catalog_compile_unit_id"]: entry for entry in catalog_entries
    }
    for task in tasks:
        if any(task[field] != project[project_field] for field, project_field in (
            ("project_id", "project_id"),
            ("catalog_id", "catalog_id"),
            ("catalog_digest", "catalog_digest"),
        )):
            fail("materialization.task-binding-mismatch", "task project/catalog binding differs")
        if task["requested_descriptor_ids"] != DESCRIPTOR_IDS or task["dependency_groups"] != [
            "canonical",
            "observation",
        ]:
            fail("materialization.task-binding-mismatch", "task output/group set differs")
        if task["sandbox"]["policy_digest"] != request["worker"]["sandbox_policy_digest"]:
            fail("materialization.task-binding-mismatch", "task sandbox policy differs")
        entry = entries_by_id[task["selected_catalog_compile_unit_id"]]
        if task["normalized_invocation_digest"] != expected_normalized_invocation_digest(
            task
        ):
            fail(
                "materialization.identity-mismatch",
                "effective argv differs from normalized invocation digest",
            )
        if (
            entry["effective_invocation_digest"]
            != task["normalized_invocation_digest"]
            or entry["source_digest"] != task["source"]["content_digest"]
            or entry["environment_digest"] != task["environment_digest"]
        ):
            fail(
                "materialization.task-binding-mismatch",
                "selected catalog entry payload differs from task input",
            )
        if task["selected_catalog_compile_unit_id"] == task["compile_unit_id"]:
            fail(
                "materialization.task-binding-mismatch",
                "catalog-local and final relation compile-unit IDs are aliased",
            )
        if (
            task["language"] != task["variant"]["language"]
            or task["variant"]["target_triple"]
            != task["toolchain"]["target_triple"]
        ):
            fail(
                "materialization.task-binding-mismatch",
                "task base payload cross-binding differs",
            )
        source = decode_canonical_base64(task["source"]["content_base64"])
        if len(source) != task["source"]["size_bytes"] or content_digest(source) != task["source"][
            "content_digest"
        ]:
            fail("materialization.identity-mismatch", "source size/content digest differs")
        if task["source"]["file_id"] != file_identity(
            task["source"]["logical_path"]
        ):
            fail("materialization.identity-mismatch", "source file identity differs")
        if task["source"]["line_index_id"] != line_index_identity(source):
            fail("materialization.identity-mismatch", "source line-index identity differs")
        if task["task_input_digest"] != expected_task_input_digest(request, task):
            fail(
                "materialization.identity-mismatch",
                "cxxlens.clang22.task.v3 input digest differs",
            )
        if task["provider_execution_id"] != expected_provider_execution_id(
            request, task
        ):
            fail(
                "materialization.identity-mismatch",
                "provider execution identity differs",
            )
    rows = base_claim_rows(root, request)
    toolchain_rows = {
        row["toolchain"]: row for row in rows["build.toolchain_context.v1"]
    }
    for task in tasks:
        if task["toolchain_digest"] != base_claim_row_digest(
            "build.toolchain_context.v1",
            toolchain_rows[task["toolchain_context_id"]],
        ):
            fail("materialization.identity-mismatch", "toolchain row digest differs")
    if publication["genesis"] != (publication["expected_parent_publication"] is None):
        fail("materialization.stale-parent", "genesis/expected-parent binding differs")
    if publication["backend"] == "memory" and (
        not publication["genesis"]
        or publication["expected_parent_publication"] is not None
        or publication["sqlite_path"] is not None
    ):
        fail(
            "materialization.store-failure",
            "memory materialization must be fresh genesis without a path",
        )


def sample_primary_span_bundle(task: dict[str, Any]) -> dict[str, Any]:
    source = task["source"]
    end = min(3, source["size_bytes"])
    return {
        "span_id": source_span_identity(
            source["source_snapshot_id"],
            source["file_id"],
            0,
            end,
            "expression",
        ),
        "snapshot": source["source_snapshot_id"],
        "file": source["file_id"],
        "begin": 0,
        "end": end,
        "role": "expression",
        "read_only": source["read_only"],
    }


_FIXTURE_WORKER_ROW_CANONICAL: dict[tuple[str, str], str] = {}


def _fixture_strong_id(kind: str, seed: str) -> str:
    return canonical_identity_digest(kind, [seed])


def _fixture_scalar_value(type_name: str, seed: str) -> Any:
    scalar = _base_scalar_type(type_name)
    if scalar == "bool":
        return True
    if scalar in {"int64", "uint64"}:
        return 0
    if scalar == "bytes":
        return seed.encode("utf-8")
    if scalar == "digest":
        return semantic_digest("cxxlens.clang22-fixture-cell.v1", seed)
    if scalar == "semantic_version":
        return "1.0.0"
    if scalar.startswith("typed_id<"):
        parameter = scalar[len("typed_id<") : -1]
        kind = parameter.removesuffix("_id").replace("_", "-")
        return _fixture_strong_id(kind, seed)
    if scalar in {"source_span_id", "evidence_id", "condition_ref"}:
        return _fixture_strong_id(scalar.replace("_", "-"), seed)
    if scalar.startswith("closed_symbol<"):
        symbol = scalar[len("closed_symbol<") : -1]
        if symbol == "cc.canonicalization-state/1":
            return "canonicalized"
        return "fixture"
    if scalar.startswith("open_symbol<"):
        symbol = scalar[len("open_symbol<") : -1]
        return {
            "cc.call-kind/1": "function",
            "cc.direct-target-resolution/1": "direct",
            "source.range-role/1": "expression",
        }.get(symbol, "fixture")
    if scalar.startswith("set<"):
        return b""
    return seed


def _fixture_worker_row(
    root: pathlib.Path,
    task: dict[str, Any],
    descriptor_id: str,
    *,
    label: str,
    ordinal: int,
    primary_span_bundle: dict[str, Any] | None,
    exact_equivalence: bool,
    limitation: str | None,
) -> tuple[str, str]:
    relation = admitted_registry_relations(root)[descriptor_id]
    seed = "|".join(
        (
            descriptor_id,
            task["provider_task_id"],
            task["task_input_digest"],
            label,
            str(ordinal),
        )
    )
    result_column = relation["claim"]["domain_identity"]["result_column"]
    row: dict[str, Any] = {}
    for column in relation["columns"]:
        if column["required"] and column["id"] != result_column:
            row[column["name"]] = _fixture_scalar_value(
                column["type"], f"{seed}|{column['id']}"
            )

    bundle = primary_span_bundle
    if descriptor_id == "cc.call_site.v1":
        bundle = bundle or sample_primary_span_bundle(task)
        row.update(
            {
                "compile_unit": task["compile_unit_id"],
                "source": bundle["span_id"],
                "kind": "function",
                "ordinal": ordinal,
            }
        )
    elif descriptor_id == "cc.entity.v1":
        row.update(
            {
                "canonicalization": "canonicalized",
                "kind": "function",
                "structural_signature_digest": semantic_digest(
                    "cxxlens.clang22-fixture-entity-signature.v1", seed
                ),
            }
        )
    elif descriptor_id == "cc.call_direct_target.v1":
        row.update(
            {
                "resolution": "direct",
                "call": _fixture_strong_id("cc-call", f"{seed}|call"),
                "target": _fixture_strong_id("cc-entity", f"{seed}|target"),
            }
        )
    elif descriptor_id in DESCRIPTOR_IDS[3:]:
        row.update(
            {
                "compile_unit": task["compile_unit_id"],
                "semantic_key": seed.encode("utf-8"),
                "payload_digest": semantic_digest(
                    "cxxlens.clang22-fixture-observation-payload.v1", seed
                ),
                "exact_equivalence": exact_equivalence,
            }
        )
        if limitation is not None:
            row["limitation"] = limitation
        if descriptor_id in SPAN_OBSERVATION_DESCRIPTORS and bundle is not None:
            row.update(
                {
                    "source": bundle["span_id"],
                    "source_snapshot": bundle["snapshot"],
                    "source_file": bundle["file"],
                    "source_begin": bundle["begin"],
                    "source_end": bundle["end"],
                    "source_role": bundle["role"],
                    "source_read_only": bundle["read_only"],
                }
            )

    if result_column is not None:
        result_name = next(
            column["name"]
            for column in relation["columns"]
            if column["id"] == result_column
        )
        row[result_name] = derive_registry_row_identity(relation, row)
    canonical_form = detached_row_canonical_form(
        relation,
        row,
        include_absent_optional=True,
    )
    row_digest = content_digest(canonical_form.encode("utf-8"))
    cache_key = (descriptor_id, row_digest)
    previous = _FIXTURE_WORKER_ROW_CANONICAL.setdefault(cache_key, canonical_form)
    if previous != canonical_form:
        fail("materialization.claim-invalid", "fixture row digest collision")
    return row_digest, canonical_form


def expected_guarantee_profile_digest() -> str:
    return semantic_digest(
        GUARANTEE_PROFILE_ID,
        _canonical_projection_value(
            {
                "profile_id": GUARANTEE_PROFILE_ID,
                "materialization_contract_version": MATERIALIZATION_VERSION,
                "assumptions": GUARANTEE_ASSUMPTIONS,
                "verification_modalities": GUARANTEE_MODALITIES,
            }
        ),
    )


def coverage_record_key(record: dict[str, Any]) -> tuple[str, str, str, str]:
    return (
        record["kind"],
        record["id"],
        record["state"],
        record["reason"],
    )


def coverage_record_set_digest(
    task: dict[str, Any],
    plane: str,
    records: list[dict[str, Any]],
) -> str:
    return semantic_digest(
        f"cxxlens.clang22-task-{plane}-coverage.v1",
        _canonical_projection_value(
            {
                "semantic_task_key": semantic_result_key(task),
                "records": records,
            }
        ),
    )


def expected_task_coverage(task: dict[str, Any]) -> dict[str, Any]:
    transport_records = [
        {
            "kind": TRANSPORT_COVERAGE_KIND,
            "id": task["provider_task_id"],
            "state": "covered",
            "reason": "",
        }
    ]
    semantic_records = [
        {
            "kind": kind,
            "id": task["provider_task_id"],
            "state": "covered",
            "reason": "",
        }
        for kind in SEMANTIC_COVERAGE_KINDS
    ]
    return {
        "transport_records": transport_records,
        "transport_record_set_digest": coverage_record_set_digest(
            task,
            "transport",
            transport_records,
        ),
        "semantic_records": semantic_records,
        "semantic_record_set_digest": coverage_record_set_digest(
            task,
            "semantic",
            semantic_records,
        ),
    }


def expected_input_transfer_receipt(
    request: dict[str, Any],
    task: dict[str, Any],
) -> dict[str, Any]:
    logical_input = worker_task_v3_projection(request, task)
    logical_digest = content_digest(logical_input)
    if logical_digest != task["task_input_digest"]:
        fail(
            "materialization.task-binding-mismatch",
            "task-input transfer differs from exact task.v3 digest",
        )
    if len(logical_input) > MAXIMUM_TASK_INPUT_BYTES:
        fail(
            "materialization.task-binding-mismatch",
            "task.v3 exceeds the negotiated logical input limit",
        )
    chunks = [
        logical_input[offset : offset + TASK_INPUT_CHUNK_BYTES]
        for offset in range(0, len(logical_input), TASK_INPUT_CHUNK_BYTES)
    ]
    if len(chunks) > MAXIMUM_TASK_INPUT_CHUNKS:
        fail(
            "materialization.task-binding-mismatch",
            "task.v3 exceeds the negotiated input chunk count",
        )
    chunk_digests = [content_digest(chunk) for chunk in chunks]
    return {
        "protocol_version": "1.1.0",
        "required_feature": TASK_INPUT_FEATURE,
        "task_input_codec": "cxxlens.clang22.task.v3",
        "logical_input_bytes": len(logical_input),
        "logical_input_digest": logical_digest,
        "canonical_chunk_bytes": TASK_INPUT_CHUNK_BYTES,
        "chunk_count": len(chunks),
        "ordered_chunk_payload_digest_set_digest": semantic_digest(
            "cxxlens.provider-input-chunk-payload-set.v1",
            _canonical_projection_value(
                {
                    "task_id": task["provider_task_id"],
                    "input_digest": logical_digest,
                    "chunk_digests": chunk_digests,
                }
            ),
        ),
    }


def expected_task_guarantee_fragment_digest(result: dict[str, Any]) -> str:
    observation_censuses = [
        {
            "descriptor_id": batch["descriptor_id"],
            "census": batch["observation_equivalence_census"],
        }
        for batch in result["batches"]
        if batch["descriptor_id"] in DESCRIPTOR_IDS[3:]
    ]
    components = result["side_channel_components"]
    return semantic_digest(
        "cxxlens.clang22-task-guarantee-fragment.v1",
        _canonical_projection_value(
            {
                "semantic_task_key": semantic_result_key(result),
                "profile_id": components["guarantee_profile_id"],
                "profile_digest": components["guarantee_profile_digest"],
                "semantic_coverage_set_digest": components[
                    "semantic_coverage_set_digest"
                ],
                "unresolved_set_digest": components["unresolved_set_digest"],
                "evidence_set_digest": components["evidence_set_digest"],
                "groups": [
                    {
                        "dependency_group_id": group["dependency_group_id"],
                        "atomic_output_group_id": group[
                            "atomic_output_group_id"
                        ],
                        "descriptor_ids": group["descriptor_ids"],
                        "sealed": group["sealed"],
                    }
                    for group in sorted(
                        result["groups"],
                        key=lambda row: row["dependency_group_id"],
                    )
                ],
                "observation_censuses": observation_censuses,
            }
        ),
    )


def fixture_task_unresolved_records(result: dict[str, Any]) -> list[dict[str, Any]]:
    return []


def fixture_task_evidence_records(result: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        {
            "kind": kind,
            "subject": result["compile_unit_id"],
            "producer": result["provider_task_id"],
            "summary": f"{kind}-retained",
        }
        for kind in (
            "canonicalization",
            "provider_execution",
            "source_observation",
        )
    ]


def runtime_authorized_batches(
    root: pathlib.Path,
    request: dict[str, Any],
) -> list[dict[str, Any]]:
    """Project exact Registry authority into the generic runtime batch input."""

    bindings = {
        binding["descriptor_id"]: binding
        for binding in request["registry"]["descriptors"]
    }
    relations = admitted_registry_relations(root)
    return [
        {
            "descriptor_id": descriptor_id,
            "descriptor_digest": bindings[descriptor_id][
                "runtime_descriptor_digest"
            ],
            "dependency_group_id": bindings[descriptor_id][
                "dependency_group_id"
            ],
            "atomic_output_group_id": bindings[descriptor_id][
                "atomic_output_group_id"
            ],
            "batch_id": bindings[descriptor_id]["batch_id"],
            "columns": [
                {
                    "id": column["id"],
                    "type": column["type"],
                    "required": column["required"],
                }
                for column in relations[descriptor_id]["columns"]
            ],
        }
        for descriptor_id in DESCRIPTOR_IDS
    ]


def expected_runtime_provider_identity(
    root: pathlib.Path,
    request: dict[str, Any],
) -> dict[str, Any]:
    """Project the independently validated installed worker/session authority."""

    worker = request["worker"]
    return {
        "provider_id": worker["provider_id"],
        "provider_version": worker["provider_version"],
        "provider_binary_digest": worker["installed_binary_digest"],
        "provider_semantic_contract_digest": worker["semantic_contract_digest"],
        "protocol_major": worker["protocol_major"],
        "protocol_minor": worker["protocol_minor"],
        "required_features": copy.deepcopy(worker["required_features"]),
        "sandbox_policy_digest": worker["sandbox_policy_digest"],
        "offered_relations": sorted(
            batch["descriptor_id"]
            for batch in runtime_authorized_batches(root, request)
        ),
    }


def report_runtime_provider_identity(report: dict[str, Any]) -> dict[str, Any]:
    """Project the report leaves that must match the raw-validated provider identity."""

    provider = report["provider"]
    return {
        "provider_id": provider["provider_id"],
        "provider_version": provider["provider_version"],
        "provider_binary_digest": report["installation"]["measured"]["worker"][
            "digest"
        ],
        "provider_semantic_contract_digest": provider["semantic_contract_digest"],
        "protocol_major": provider["protocol_major"],
        "protocol_minor": provider["protocol_minor"],
        "required_features": copy.deepcopy(provider["required_features"]),
        "sandbox_policy_digest": provider["sandbox_policy_digest"],
        "offered_relations": sorted(
            binding["descriptor_id"] for binding in report["registry"]["descriptors"]
        ),
    }


def validate_runtime_provider_identity_cross_binding(
    validated_provider_identity: Any,
    expected_provider_identity: dict[str, Any],
    report_provider_identity: dict[str, Any] | None = None,
) -> None:
    """Require one identity across selection, raw hello/task acceptance, and report."""

    if validated_provider_identity != expected_provider_identity:
        fail(
            "materialization.transcript-invalid",
            "runtime-validated provider identity differs from installed authority",
        )
    if (
        report_provider_identity is not None
        and report_provider_identity != validated_provider_identity
    ):
        fail(
            "materialization.identity-mismatch",
            "report provider leaves differ from runtime-validated provider identity",
        )


def validate_runtime_seal_cross_binding(
    sealed: dict[str, Any],
    result: dict[str, Any],
) -> None:
    """Bind every report-visible provider leaf to the raw-derived generic seal."""

    if not isinstance(sealed, dict) or not isinstance(sealed.get("batches"), list):
        fail(
            "materialization.transcript-invalid",
            "runtime-private immutable seal shape differs",
        )
    actual_batches = sealed["batches"]
    if len(actual_batches) != len(result["batches"]):
        fail(
            "materialization.transcript-invalid",
            "runtime-private immutable seal batch census differs",
        )
    expected_batches = []
    for report_batch, actual_batch in zip(
        result["batches"], actual_batches, strict=True
    ):
        if not isinstance(actual_batch, dict) or not isinstance(
            actual_batch.get("batch_digest"), str
        ):
            fail(
                "materialization.transcript-invalid",
                "runtime-private immutable seal batch shape differs",
            )
        expected_batches.append(
            {
                "task_id": result["provider_task_id"],
                "descriptor_id": report_batch["descriptor_id"],
                "descriptor_digest": report_batch["runtime_descriptor_digest"],
                "dependency_group_id": report_batch["dependency_group_id"],
                "atomic_output_group_id": report_batch["atomic_output_group_id"],
                "batch_id": report_batch["batch_id"],
                "batch_digest": actual_batch["batch_digest"],
                "ordered_chunk_digests": report_batch["ordered_chunk_digests"],
                "row_canonical_forms": [
                    row["row_canonical_form"]
                    for row in report_batch["row_bindings"]
                ],
            }
        )
    expected = {
        "task_id": result["provider_task_id"],
        "terminal": result["terminal"],
        "batches": expected_batches,
        "coverage_records": sorted(
            result["coverage"]["transport_records"]
            + result["coverage"]["semantic_records"],
            key=coverage_record_key,
        ),
        "unresolved_records": fixture_task_unresolved_records(result),
        "evidence_records": fixture_task_evidence_records(result),
    }
    if sealed != expected:
        fail(
            "materialization.transcript-invalid",
            "report semantic transcript differs from runtime-private immutable seal",
        )
def materialization_runtime_receipt(
    generic_receipt: dict[str, Any],
) -> dict[str, Any]:
    """Map the generic runtime-owned receipt into the report's stable field names."""

    return {
        "raw_frame_stream_bytes": generic_receipt["raw_stdout_byte_count"],
        "raw_frame_stream_digest": generic_receipt["raw_stdout_sha256"],
        "frame_count": generic_receipt["decoded_frame_count"],
        "frame_transcript_digest": generic_receipt["frame_transcript_digest"],
        "sealed_transcript_digest": generic_receipt["sealed_transcript_digest"],
    }


def encode_fixture_runtime_raw(
    root: pathlib.Path,
    request: dict[str, Any],
    task: dict[str, Any],
    result: dict[str, Any],
) -> bytes:
    """Encode deterministic real provider wire through the shared runtime codec."""

    batch_rows = {
        batch["batch_id"]: [
            row["row_canonical_form"] for row in batch["row_bindings"]
        ]
        for batch in result["batches"]
    }
    try:
        return provider_runtime.encode_runtime_private_fixture(
            load(root / PROVIDER_PROTOCOL),
            expected_runtime_provider_identity(root, request),
            task["provider_task_id"],
            runtime_authorized_batches(root, request),
            batch_rows,
            result["coverage"]["transport_records"]
            + result["coverage"]["semantic_records"],
            fixture_task_unresolved_records(result),
            fixture_task_evidence_records(result),
        )
    except ValueError as error:
        fail(
            "materialization.transcript-invalid",
            f"fixture provider wire construction failed: {error}",
        )


def derive_runtime_observation(
    root: pathlib.Path,
    request: dict[str, Any],
    task: dict[str, Any],
    raw_stdout: bytes,
) -> dict[str, Any]:
    """Decode, validate and seal one runtime-owned raw stdout occurrence once."""

    try:
        expected_identity = expected_runtime_provider_identity(root, request)
        return provider_runtime.derive_runtime_private_observation(
            load(root / PROVIDER_PROTOCOL),
            raw_stdout,
            task["provider_task_id"],
            expected_provider_identity=expected_identity,
            authorized_batches=runtime_authorized_batches(root, request),
        )
    except provider_runtime.ContractError as error:
        fail(
            "materialization.transcript-invalid",
            f"runtime-owned provider observation failed: {error}",
        )


def bind_fixture_runtime_observation(
    root: pathlib.Path,
    request: dict[str, Any],
    task: dict[str, Any],
    result: dict[str, Any],
) -> bytes:
    """Create raw fixture wire first, then derive and bind its receipt and seal."""

    raw_stdout = encode_fixture_runtime_raw(root, request, task, result)
    observation = derive_runtime_observation(
        root,
        request,
        task,
        raw_stdout,
    )
    sealed_batches = {
        batch["batch_id"]: batch
        for batch in observation["sealed_transcript"]["batches"]
    }
    for batch in result["batches"]:
        sealed_batch = sealed_batches.get(batch["batch_id"])
        if sealed_batch is None or sealed_batch["row_canonical_forms"] != [
            row["row_canonical_form"] for row in batch["row_bindings"]
        ]:
            fail(
                "materialization.transcript-invalid",
                "fixture provider rows differ after shared raw-wire validation",
            )
        batch["ordered_chunk_digests"] = sealed_batch[
            "ordered_chunk_digests"
        ]
        bind_batch_digests(task, batch)
    groups = {
        group["dependency_group_id"]: group for group in result["groups"]
    }
    for group_id in GROUP_DESCRIPTORS:
        groups[group_id]["batch_set_digest"] = expected_group_batch_set_digest(
            task,
            groups[group_id],
            result["batches"],
        )
    result["runtime_receipt"] = materialization_runtime_receipt(
        observation["receipt"]
    )
    validate_runtime_provider_identity_cross_binding(
        observation["validated_provider_identity"],
        expected_runtime_provider_identity(root, request),
    )
    validate_runtime_seal_cross_binding(observation["sealed_transcript"], result)
    return raw_stdout


def _task_report(
    root: pathlib.Path,
    request: dict[str, Any],
    task: dict[str, Any],
) -> dict[str, Any]:
    bindings = {row["descriptor_id"]: row for row in request["registry"]["descriptors"]}
    context = task_semantic_context(task)
    batches: list[dict[str, Any]] = []
    for descriptor in DESCRIPTOR_IDS:
        primary_span = (
            sample_primary_span_bundle(task)
            if descriptor in SPAN_OBSERVATION_DESCRIPTORS
            else None
        )
        row_digest, row_canonical_form = _fixture_worker_row(
            root,
            task,
            descriptor,
            label="initial",
            ordinal=0,
            primary_span_bundle=primary_span,
            exact_equivalence=True,
            limitation=None,
        )
        row_binding = {
            "row_digest": row_digest,
            "row_canonical_form": row_canonical_form,
            "worker_assertion_claim_ref": (
                "materialization-claim-envelope:sha256:" + "0" * 64
            ),
            "final_relation_compile_unit_id": task["compile_unit_id"],
            "originating_task": context,
            "primary_span_bundle_digest": (
                source_span_bundle_digest(primary_span)
                if descriptor in SPAN_OBSERVATION_DESCRIPTORS
                else None
            ),
            "exact_equivalence": (
                True if descriptor in DESCRIPTOR_IDS[3:] else None
            ),
            "limitation_digest": None,
        }
        chunk_digest = semantic_digest(
            "cxxlens.clang22-fixture-ordered-chunk.v1",
            _canonical_projection_value(
                {
                    "descriptor_id": descriptor,
                    "originating_task": context,
                    "row_digest": row_digest,
                }
            ),
        )
        provenance_edge_digest = semantic_digest(
            "cxxlens.clang22-fixture-provenance-edge.v2",
            _canonical_projection_value(
                {
                    "descriptor_id": descriptor,
                    "originating_task": context,
                    "row_digest": row_digest,
                }
            ),
        )
        batch: dict[str, Any] = {
            "batch_id": bindings[descriptor]["batch_id"],
            "descriptor_id": descriptor,
            "runtime_descriptor_digest": bindings[descriptor]["runtime_descriptor_digest"],
            "dependency_group_id": bindings[descriptor]["dependency_group_id"],
            "atomic_output_group_id": "clang22-atomic",
            "row_count": 1,
            "ordered_chunk_digests": [chunk_digest],
            "ordered_chunk_set_digest": "pending",
            "row_bindings": [row_binding],
            "row_binding_set_digest": "pending",
            "worker_assertion_claim_refs": [],
            "worker_assertion_claim_occurrence_count": 0,
            "claim_content_ids": [],
            "claim_content_count": 0,
            "claim_content_set_digest": "pending",
            "sdk_claim_occurrence_set_digest": "pending",
            "provenance_edge_digests": [provenance_edge_digest],
            "provenance_edge_set_digest": "pending",
            "batch_digest": "pending",
            "sealed": True,
        }
        if descriptor in DESCRIPTOR_IDS[3:]:
            batch["observation_equivalence_census"] = (
                observation_equivalence_census(
                    descriptor,
                    [
                        {
                            "observation_row_digest": row_digest,
                            "final_relation_compile_unit_id": task["compile_unit_id"],
                            "originating_task": context,
                            "exact_equivalence": True,
                            "limitation": None,
                            "limitation_digest": None,
                        }
                    ],
                )
            )
        bind_batch_digests(task, batch)
        batches.append(batch)
    groups: list[dict[str, Any]] = []
    for group, descriptors in GROUP_DESCRIPTORS.items():
        group_result = {
            "dependency_group_id": group,
            "atomic_output_group_id": "clang22-atomic",
            "descriptor_ids": descriptors,
            "sealed": True,
            "batch_set_digest": "pending",
        }
        group_result["batch_set_digest"] = expected_group_batch_set_digest(
            task,
            group_result,
            batches,
        )
        groups.append(group_result)
    coverage = expected_task_coverage(task)
    side_components = {
        "transport_coverage_set_digest": coverage[
            "transport_record_set_digest"
        ],
        "semantic_coverage_set_digest": coverage["semantic_record_set_digest"],
        "unresolved_set_digest": semantic_digest(
            "cxxlens.clang22-task-unresolved.v1",
            _canonical_projection_value(
                {"originating_task": context, "records": []}
            ),
        ),
        "evidence_set_digest": semantic_digest(
            "cxxlens.clang22-task-evidence.v1",
            _canonical_projection_value(
                {
                    "originating_task": context,
                    "records": fixture_task_evidence_records(task),
                }
            ),
        ),
        "guarantee_profile_id": GUARANTEE_PROFILE_ID,
        "guarantee_profile_digest": expected_guarantee_profile_digest(),
        "guarantee_fragment_digest": "pending",
    }
    result = {
        "provider_task_id": task["provider_task_id"],
        "provider_execution_id": task["provider_execution_id"],
        "selected_catalog_compile_unit_id": task[
            "selected_catalog_compile_unit_id"
        ],
        "compile_unit_id": task["compile_unit_id"],
        "task_input_digest": task["task_input_digest"],
        "terminal": "provider.success",
        "input_transfer": expected_input_transfer_receipt(request, task),
        "runtime_receipt": {},
        "coverage": coverage,
        "groups": groups,
        "batches": batches,
        "side_channel_components": side_components,
        "side_channel_digest": "pending",
        "task_result_digest": "pending",
    }
    bind_fixture_runtime_observation(root, request, task, result)
    side_components["guarantee_fragment_digest"] = (
        expected_task_guarantee_fragment_digest(result)
    )
    result["side_channel_digest"] = expected_task_side_channel_digest(result)
    result["task_result_digest"] = expected_task_result_digest(result)
    return result


def fixture_runtime_raw_occurrences(
    root: pathlib.Path,
    request: dict[str, Any],
) -> dict[tuple[str, str, str], bytes]:
    """Build deterministic exact worker stdout occurrences from the request."""

    occurrences: dict[tuple[str, str, str], bytes] = {}
    for task in request["tasks"]:
        private_result = _task_report(root, request, task)
        key = task_execution_key(task)
        occurrences[key] = encode_fixture_runtime_raw(
            root,
            request,
            task,
            private_result,
        )
    return occurrences


def bind_fixture_runtime_occurrences_for_report(
    root: pathlib.Path,
    request: dict[str, Any],
    report: dict[str, Any],
) -> dict[tuple[str, str, str], bytes]:
    """Test-only adapter: emit raw wire first and bind its derived observation."""

    tasks = {task_execution_key(task): task for task in request["tasks"]}
    occurrences: dict[tuple[str, str, str], bytes] = {}
    for result in report["task_results"]:
        key = task_execution_key(result)
        occurrences[key] = bind_fixture_runtime_observation(
            root,
            request,
            tasks[key],
            result,
        )
    return occurrences


def _digest_projection(domain: str, value: Any) -> str:
    return semantic_digest(domain, _canonical_projection_value(value))


def append_fixture_materialized_row(
    task: dict[str, Any],
    batch: dict[str, Any],
    *,
    label: str,
    exact_equivalence: bool = True,
    limitation: str | None = None,
    primary_span_bundle: dict[str, Any] | None = None,
) -> None:
    """Add one deterministic sealed-row leaf for acceptance scenario fixtures."""

    context = task_semantic_context(task)
    descriptor = batch["descriptor_id"]
    row_digest, row_canonical_form = _fixture_worker_row(
        ROOT,
        task,
        descriptor,
        label=label,
        ordinal=len(batch["row_bindings"]),
        primary_span_bundle=primary_span_bundle,
        exact_equivalence=exact_equivalence,
        limitation=limitation,
    )
    batch["row_bindings"].append(
        {
            "row_digest": row_digest,
            "row_canonical_form": row_canonical_form,
            "worker_assertion_claim_ref": (
                "materialization-claim-envelope:sha256:" + "0" * 64
            ),
            "final_relation_compile_unit_id": task["compile_unit_id"],
            "originating_task": context,
            "primary_span_bundle_digest": (
                None
                if primary_span_bundle is None
                else source_span_bundle_digest(primary_span_bundle)
            ),
            "exact_equivalence": (
                exact_equivalence if descriptor in DESCRIPTOR_IDS[3:] else None
            ),
            "limitation_digest": (
                None
                if limitation is None
                else content_digest(limitation.encode("utf-8"))
            ),
        }
    )
    batch["row_bindings"].sort(
        key=lambda row: (
            row["row_digest"],
            task_semantic_context_key(row["originating_task"]),
        )
    )
    batch["ordered_chunk_digests"].append(
        _digest_projection(
            "cxxlens.clang22-fixture-ordered-chunk.v1",
            {
                "descriptor_id": descriptor,
                "originating_task": context,
                "row_digest": row_digest,
                "label": label,
            },
        )
    )
    batch["provenance_edge_digests"].append(
        _digest_projection(
            "cxxlens.clang22-fixture-provenance-edge.v2",
            {
                "descriptor_id": descriptor,
                "originating_task": context,
                "row_digest": row_digest,
            },
        )
    )
    batch["provenance_edge_digests"].sort()
    batch["row_count"] = len(batch["row_bindings"])
    if descriptor in DESCRIPTOR_IDS[3:]:
        if exact_equivalence and limitation is not None:
            raise ValueError("exact fixture observation cannot have a limitation")
        if not exact_equivalence and not limitation:
            raise ValueError("non-exact fixture observation requires a limitation")
        census = batch["observation_equivalence_census"]
        census["rows"].append(
            {
                "observation_row_digest": row_digest,
                "final_relation_compile_unit_id": task["compile_unit_id"],
                "originating_task": context,
                "exact_equivalence": exact_equivalence,
                "limitation": limitation,
                "limitation_digest": (
                    None
                    if limitation is None
                    else content_digest(limitation.encode("utf-8"))
                ),
            }
        )
        batch["observation_equivalence_census"] = observation_equivalence_census(
            descriptor,
            census["rows"],
        )


def observation_equivalence_census(
    descriptor_id: str,
    rows: list[dict[str, Any]],
) -> dict[str, Any]:
    canonical_rows = sorted(
        copy.deepcopy(rows),
        key=lambda row: (
            row["observation_row_digest"],
            task_semantic_context_key(row["originating_task"]),
        ),
    )
    exact_count = sum(bool(row["exact_equivalence"]) for row in canonical_rows)
    return {
        "rows": canonical_rows,
        "exact_equivalence_count": exact_count,
        "non_exact_equivalence_count": len(canonical_rows) - exact_count,
        "row_equivalence_set_digest": _digest_projection(
            "cxxlens.clang22-observation-equivalence-set.v1",
            {
                "descriptor_id": descriptor_id,
                "rows": canonical_rows,
            },
        ),
    }


def validate_observation_equivalence_census(
    descriptor_id: str,
    census: dict[str, Any],
    row_bindings: list[dict[str, Any]],
) -> None:
    rows = census["rows"]
    for row in rows:
        exact = row["exact_equivalence"]
        limitation = row["limitation"]
        limitation_digest = row["limitation_digest"]
        if exact:
            if limitation is not None or limitation_digest is not None:
                fail(
                    "materialization.claim-invalid",
                    "exact observation retained a limitation",
                )
        elif (
            not isinstance(limitation, str)
            or not limitation
            or not _strict_utf8(limitation, nonempty=True)
            or limitation_digest != content_digest(limitation.encode("utf-8"))
        ):
            fail(
                "materialization.claim-invalid",
                "non-exact observation lacks its typed limitation digest",
            )
    expected_rows = [
        {
            "observation_row_digest": binding["row_digest"],
            "final_relation_compile_unit_id": binding[
                "final_relation_compile_unit_id"
            ],
            "originating_task": binding["originating_task"],
            "exact_equivalence": binding["exact_equivalence"],
            "limitation_digest": binding["limitation_digest"],
        }
        for binding in row_bindings
    ]
    actual_rows = [
        {
            "observation_row_digest": row["observation_row_digest"],
            "final_relation_compile_unit_id": row[
                "final_relation_compile_unit_id"
            ],
            "originating_task": row["originating_task"],
            "exact_equivalence": row["exact_equivalence"],
            "limitation_digest": row["limitation_digest"],
        }
        for row in rows
    ]
    if actual_rows != expected_rows:
        fail(
            "materialization.task-binding-mismatch",
            "observation equivalence rows differ from adopted row bindings",
        )
    if census != observation_equivalence_census(descriptor_id, rows):
        fail(
            "materialization.claim-invalid",
            "observation equivalence census or digest differs",
        )


def _batch_leaf_projection(task: dict[str, Any], batch: dict[str, Any]) -> dict[str, Any]:
    return {
        "task_execution_key": list(task_execution_key(task)),
        "descriptor_id": batch["descriptor_id"],
        "ordered_chunk_digests": batch["ordered_chunk_digests"],
        "row_bindings": batch["row_bindings"],
        "worker_assertion_claim_refs": batch["worker_assertion_claim_refs"],
        "claim_content_ids": batch["claim_content_ids"],
        "provenance_edge_digests": batch["provenance_edge_digests"],
    }


def bind_batch_digests(task: dict[str, Any], batch: dict[str, Any]) -> None:
    leaf = _batch_leaf_projection(task, batch)
    batch["ordered_chunk_set_digest"] = _digest_projection(
        "cxxlens.clang22-ordered-chunk-set.v1",
        {
            "task_execution_key": leaf["task_execution_key"],
            "descriptor_id": leaf["descriptor_id"],
            "ordered_chunk_digests": leaf["ordered_chunk_digests"],
        },
    )
    batch["row_binding_set_digest"] = _digest_projection(
        "cxxlens.clang22-batch-row-binding-set.v1",
        {
            "task_execution_key": leaf["task_execution_key"],
            "descriptor_id": leaf["descriptor_id"],
            "row_bindings": leaf["row_bindings"],
        },
    )
    batch["claim_content_set_digest"] = _digest_projection(
        "cxxlens.clang22-batch-claim-content-set.v2",
        {
            "task_execution_key": leaf["task_execution_key"],
            "descriptor_id": leaf["descriptor_id"],
            "claim_content_ids": leaf["claim_content_ids"],
        },
    )
    batch["sdk_claim_occurrence_set_digest"] = _digest_projection(
        "cxxlens.clang22-batch-sdk-occurrence-set.v1",
        {
            "task_execution_key": leaf["task_execution_key"],
            "descriptor_id": leaf["descriptor_id"],
            "worker_assertion_claim_refs": leaf["worker_assertion_claim_refs"],
        },
    )
    batch["provenance_edge_set_digest"] = _digest_projection(
        "cxxlens.clang22-batch-provenance-edge-set.v1",
        {
            "task_execution_key": leaf["task_execution_key"],
            "descriptor_id": leaf["descriptor_id"],
            "provenance_edge_digests": leaf["provenance_edge_digests"],
        },
    )
    projection = {
        key: batch[key]
        for key in (
            "batch_id",
            "descriptor_id",
            "runtime_descriptor_digest",
            "dependency_group_id",
            "atomic_output_group_id",
            "row_count",
            "ordered_chunk_set_digest",
            "row_binding_set_digest",
            "worker_assertion_claim_occurrence_count",
            "claim_content_count",
            "claim_content_set_digest",
            "sdk_claim_occurrence_set_digest",
            "provenance_edge_set_digest",
            "sealed",
        )
    }
    projection["task_execution_key"] = list(task_execution_key(task))
    if "observation_equivalence_census" in batch:
        projection["observation_equivalence_census"] = batch[
            "observation_equivalence_census"
        ]
    batch["batch_digest"] = _digest_projection(
        "cxxlens.clang22-batch-result.v1",
        projection,
    )


def expected_group_batch_set_digest(
    task: dict[str, Any],
    group: dict[str, Any],
    batches: list[dict[str, Any]],
) -> str:
    by_descriptor = {batch["descriptor_id"]: batch for batch in batches}
    summaries = [
        {
            key: by_descriptor[descriptor][key]
            for key in (
                "descriptor_id",
                "batch_digest",
                "ordered_chunk_set_digest",
                "row_count",
                "row_binding_set_digest",
                "worker_assertion_claim_occurrence_count",
                "claim_content_count",
                "claim_content_set_digest",
                "sdk_claim_occurrence_set_digest",
                "provenance_edge_set_digest",
            )
        }
        for descriptor in group["descriptor_ids"]
    ]
    return _digest_projection(
        "cxxlens.clang22-group-batch-set.v1",
        {
            "task_execution_key": list(task_execution_key(task)),
            "dependency_group_id": group["dependency_group_id"],
            "atomic_output_group_id": group["atomic_output_group_id"],
            "descriptor_ids": group["descriptor_ids"],
            "sealed": group["sealed"],
            "batches": summaries,
        },
    )


def expected_task_side_channel_digest(result: dict[str, Any]) -> str:
    return _digest_projection(
        "cxxlens.clang22-task-side-channels.v1",
        {
            "task_execution_key": list(task_execution_key(result)),
            "components": result["side_channel_components"],
        },
    )


def expected_task_result_digest(result: dict[str, Any]) -> str:
    groups = sorted(result["groups"], key=lambda row: row["dependency_group_id"])
    return _digest_projection(
        "cxxlens.clang22-task-result.v1",
        {
            "task_execution_key": list(task_execution_key(result)),
            "selected_catalog_compile_unit_id": result[
                "selected_catalog_compile_unit_id"
            ],
            "compile_unit_id": result["compile_unit_id"],
            "terminal": result["terminal"],
            "input_transfer": result["input_transfer"],
            "runtime_receipt": result["runtime_receipt"],
            "coverage": result["coverage"],
            "groups": [
                {
                    "dependency_group_id": group["dependency_group_id"],
                    "batch_set_digest": group["batch_set_digest"],
                }
                for group in groups
            ],
            "side_channel_digest": result["side_channel_digest"],
        },
    )


def expected_task_result_set_digest(results: Iterable[dict[str, Any]]) -> str:
    rows = sorted(
        (
            {
                "task_execution_key": list(task_execution_key(result)),
                "task_result_digest": result["task_result_digest"],
            }
            for result in results
        ),
        key=lambda row: tuple(row["task_execution_key"]),
    )
    return _digest_projection("cxxlens.clang22-task-result-set.v1", rows)


def expected_raw_frame_set_digest(results: Iterable[dict[str, Any]]) -> str:
    rows = sorted(
        (
            {
                "task_execution_key": list(task_execution_key(result)),
                "raw_frame_stream_bytes": result["runtime_receipt"][
                    "raw_frame_stream_bytes"
                ],
                "raw_frame_stream_digest": result["runtime_receipt"][
                    "raw_frame_stream_digest"
                ],
                "frame_count": result["runtime_receipt"]["frame_count"],
                "frame_transcript_digest": result["runtime_receipt"][
                    "frame_transcript_digest"
                ],
            }
            for result in results
        ),
        key=lambda row: tuple(row["task_execution_key"]),
    )
    return _digest_projection("cxxlens.clang22-raw-frame-set.v1", rows)


def base_claim_row_set_digest(descriptor_id: str, rows: list[dict[str, Any]]) -> str:
    return semantic_digest(
        "cxxlens.base-claim-row-set.v1",
        _canonical_projection_value(
            {"descriptor_id": descriptor_id, "rows": rows}
        ),
    )


def task_semantic_context(task: dict[str, Any]) -> dict[str, Any]:
    """Return the semantic task context; physical provider execution is excluded."""

    return {
        "provider_task_id": task["provider_task_id"],
        "task_input_digest": task["task_input_digest"],
        "selected_catalog_compile_unit_id": task[
            "selected_catalog_compile_unit_id"
        ],
        "compile_unit_id": task["compile_unit_id"],
        "condition_universe_id": task["condition_universe_id"],
        "condition_id": task["condition_id"],
        "interpretation_domain": task["interpretation_domain"],
    }


def task_semantic_context_key(context: dict[str, Any]) -> tuple[str, ...]:
    return tuple(
        context[field]
        for field in (
            "provider_task_id",
            "task_input_digest",
            "selected_catalog_compile_unit_id",
            "compile_unit_id",
            "condition_universe_id",
            "condition_id",
            "interpretation_domain",
        )
    )


def source_span_bundle_digest(bundle: dict[str, Any]) -> str:
    return semantic_digest(
        "cxxlens.source-span-bundle.v2",
        _canonical_projection_value(
            {field: bundle[field] for field in PRIMARY_SPAN_BUNDLE_FIELDS}
        ),
    )


def sample_span_bundle_bindings(
    request: dict[str, Any],
    task_results: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    bindings = []
    results = {task_execution_key(result): result for result in task_results}
    for task in request["tasks"]:
        result = results[task_execution_key(task)]
        batches = {batch["descriptor_id"]: batch for batch in result["batches"]}
        source = task["source"]
        bundle = sample_primary_span_bundle(task)
        row = source_span_base_row(bundle, source)
        for descriptor in SPAN_OBSERVATION_DESCRIPTORS:
            observation_rows = batches[descriptor]["row_bindings"]
            if len(observation_rows) != 1:
                fail(
                    "materialization.span-invalid",
                    "fixture span observation row census differs",
                )
            bindings.append(
                {
                    "bundle": bundle,
                    "bundle_digest": source_span_bundle_digest(bundle),
                    "row_digest": base_claim_row_digest("source.span.v1", row),
                    "observation_descriptor_id": descriptor,
                    "observation_row_digest": observation_rows[0]["row_digest"],
                    "originating_task": task_semantic_context(task),
                }
            )
    bindings.sort(
        key=lambda item: (
            item["bundle"]["span_id"],
            task_semantic_context_key(item["originating_task"]),
            item["observation_descriptor_id"],
            item["observation_row_digest"],
            item["bundle_digest"],
        )
    )
    return bindings


def span_bundle_binding_set_digest(bindings: list[dict[str, Any]]) -> str:
    return semantic_digest(
        "cxxlens.source-span-bundle-task-binding-set.v2",
        _canonical_projection_value(bindings),
    )


def validated_span_rows(
    request: dict[str, Any],
    bindings: Any,
    observation_rows: dict[tuple[tuple[str, ...], str, str], str] | None = None,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Revalidate report bundles and return canonical unique span rows/bindings."""

    if not isinstance(bindings, list):
        fail("materialization.span-invalid", "validated bundle bindings are not a list")
    tasks_by_context = {
        task_semantic_context_key(task_semantic_context(task)): task
        for task in request["tasks"]
    }
    canonical: list[dict[str, Any]] = []
    seen_bindings: set[tuple[str, tuple[str, ...], str, str, str]] = set()
    seen_observation_rows: set[tuple[tuple[str, ...], str, str]] = set()
    rows_by_identity: dict[str, dict[str, Any]] = {}
    for binding in bindings:
        if not isinstance(binding, dict) or set(binding) != {
            "bundle",
            "bundle_digest",
            "row_digest",
            "observation_descriptor_id",
            "observation_row_digest",
            "originating_task",
        }:
            fail(
                "materialization.span-invalid",
                "validated bundle binding field set differs",
            )
        context = binding["originating_task"]
        if not isinstance(context, dict):
            fail("materialization.span-invalid", "span task context is invalid")
        context_key = task_semantic_context_key(context)
        task = tasks_by_context.get(context_key)
        if task is None or context != task_semantic_context(task):
            fail(
                "materialization.span-invalid",
                "span bundle originating task binding differs",
            )
        bundle = binding["bundle"]
        validate_primary_span_bundle(bundle, task["source"])
        row = source_span_base_row(bundle, task["source"])
        bundle_digest = source_span_bundle_digest(bundle)
        row_digest = base_claim_row_digest("source.span.v1", row)
        observation_descriptor = binding["observation_descriptor_id"]
        observation_row_digest = binding["observation_row_digest"]
        if observation_descriptor not in SPAN_OBSERVATION_DESCRIPTORS:
            fail(
                "materialization.span-invalid",
                "span bundle observation descriptor differs",
            )
        observation_key = (
            context_key,
            observation_descriptor,
            observation_row_digest,
        )
        if observation_rows is not None:
            expected_observation_bundle = observation_rows.get(observation_key)
            if expected_observation_bundle != bundle_digest:
                fail(
                    "materialization.span-invalid",
                    "span bundle is not bound to its adopted observation row",
                )
            seen_observation_rows.add(observation_key)
        if (
            binding["bundle_digest"] != bundle_digest
            or binding["row_digest"] != row_digest
        ):
            fail(
                "materialization.span-invalid",
                "span bundle or constructed row digest differs",
            )
        identity = bundle["span_id"]
        binding_key = (
            identity,
            context_key,
            observation_descriptor,
            observation_row_digest,
            bundle_digest,
        )
        if binding_key in seen_bindings:
            fail("materialization.span-invalid", "duplicate span bundle task binding")
        seen_bindings.add(binding_key)
        if identity in rows_by_identity and rows_by_identity[identity] != row:
            fail("materialization.span-invalid", "conflicting source span row")
        rows_by_identity[identity] = row
        canonical.append(copy.deepcopy(binding))
    canonical.sort(
        key=lambda item: (
            item["bundle"]["span_id"],
            task_semantic_context_key(item["originating_task"]),
            item["observation_descriptor_id"],
            item["observation_row_digest"],
            item["bundle_digest"],
        )
    )
    if canonical != bindings:
        fail("materialization.span-invalid", "span bundle bindings are not canonical")
    if observation_rows is not None and seen_observation_rows != set(observation_rows):
        fail(
            "materialization.span-invalid",
            "a source-bearing observation row lacks its exact span bundle binding",
        )
    rows = [rows_by_identity[identity] for identity in sorted(rows_by_identity)]
    return rows, canonical


def span_report_digests(
    bindings: list[dict[str, Any]],
    rows: list[dict[str, Any]],
) -> dict[str, str]:
    unique_bundles = {
        (binding["bundle"]["span_id"], binding["bundle_digest"]): {
            "bundle": binding["bundle"],
            "bundle_digest": binding["bundle_digest"],
        }
        for binding in bindings
    }
    bundle_projection = [unique_bundles[key] for key in sorted(unique_bundles)]
    return {
        "bundle_task_binding_set_digest": span_bundle_binding_set_digest(bindings),
        "bundle_set_digest": semantic_digest(
            "cxxlens.source-span-bundle-set.v2",
            _canonical_projection_value(bundle_projection),
        ),
        "source_span_claim_set_digest": base_claim_row_set_digest(
            "source.span.v1",
            rows,
        ),
        "evidence_digest": semantic_digest(
            "cxxlens.source-span-bundle-row-evidence-set.v2",
            _canonical_projection_value(bindings),
        ),
    }


def _originating_tasks(
    descriptor_id: str,
    row: dict[str, Any],
    tasks: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    if descriptor_id == "build.project.v1":
        matching = list(tasks)
    elif descriptor_id == "build.toolchain_context.v1":
        matching = [task for task in tasks if task["toolchain_context_id"] == row["toolchain"]]
    elif descriptor_id == "build.variant.v1":
        matching = [task for task in tasks if task["build_variant_id"] == row["variant"]]
    elif descriptor_id == "source.file.v1":
        matching = [
            task
            for task in tasks
            if task["source"]["source_snapshot_id"] == row["snapshot"]
            and task["source"]["file_id"] == row["file"]
        ]
    elif descriptor_id == "build.compile_unit.v1":
        matching = [task for task in tasks if task["compile_unit_id"] == row["compile_unit"]]
    else:
        fail("materialization.claim-invalid", "unsupported base task binding descriptor")
    matching.sort(key=lambda task: task_semantic_context_key(task_semantic_context(task)))
    if not matching:
        fail("materialization.claim-invalid", "base row has no originating task context")
    return matching


def _request_base_origin_association(
    descriptor_id: str,
    row: dict[str, Any],
    task: dict[str, Any],
    catalog_entries: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    if descriptor_id == "build.project.v1":
        evidence = [("compile_context", task["catalog_digest"])]
    elif descriptor_id == "build.toolchain_context.v1":
        evidence = [("compile_context", task["toolchain_digest"])]
    elif descriptor_id == "build.variant.v1":
        evidence = [
            ("compile_context", base_claim_row_digest(descriptor_id, row))
        ]
    elif descriptor_id == "source.file.v1":
        evidence = [("source_observation", task["source"]["content_digest"])]
    elif descriptor_id == "build.compile_unit.v1":
        entry = catalog_entries[task["selected_catalog_compile_unit_id"]]
        evidence = [
            (
                "compile_context",
                _digest_projection(
                    "cxxlens.clang22-catalog-entry-evidence.v1",
                    entry,
                ),
            )
        ]
    else:
        fail("materialization.claim-invalid", "unsupported request base association")
    return {
        "originating_task": task_semantic_context(task),
        "provenance_edge": {
            "kind": "request_task_input",
            "subject_digest": task["task_input_digest"],
        },
        "evidence_edges": [
            {"kind": kind, "subject_digest": subject}
            for kind, subject in sorted(evidence)
        ],
        "source_bundle": None,
    }


def _span_base_origin_association(binding: dict[str, Any]) -> dict[str, Any]:
    return {
        "originating_task": binding["originating_task"],
        "provenance_edge": {
            "kind": "validated_span_bundle",
            "subject_digest": binding["bundle_digest"],
        },
        "evidence_edges": [
            {
                "kind": "dynamic_observation",
                "subject_digest": binding["observation_row_digest"],
            },
            {
                "kind": "source_observation",
                "subject_digest": binding["bundle_digest"],
            },
        ],
        "source_bundle": {
            "bundle_digest": binding["bundle_digest"],
            "observation_descriptor_id": binding["observation_descriptor_id"],
            "observation_row_digest": binding["observation_row_digest"],
        },
    }


def _origin_association_key(association: dict[str, Any]) -> tuple[Any, ...]:
    source_bundle = association["source_bundle"] or {}
    return (
        task_semantic_context_key(association["originating_task"]),
        source_bundle.get("observation_descriptor_id", ""),
        source_bundle.get("observation_row_digest", ""),
        source_bundle.get("bundle_digest", ""),
        association["provenance_edge"]["kind"],
        association["provenance_edge"]["subject_digest"],
    )


def base_claim_report(
    root: pathlib.Path,
    request: dict[str, Any],
    guarantee_digest: str,
    span_bundle_bindings: list[dict[str, Any]],
) -> dict[str, Any]:
    rows = base_claim_rows(root, request)
    span_rows, span_bundle_bindings = validated_span_rows(
        request,
        span_bundle_bindings,
    )
    rows["source.span.v1"] = span_rows
    producer = {
        key: request["tool"][key]
        for key in (
            "executable",
            "interface_version",
            "distribution_version",
            "source_revision",
            "source_tree",
        )
    }
    producer_digest = semantic_digest(
        "cxxlens.base-claim-producer.v1",
        _canonical_projection_value(producer),
    )
    catalog_entries = {
        entry["catalog_compile_unit_id"]: entry
        for entry in request["project"]["catalog_compile_units"]
    }
    results = []
    for descriptor_id in BASE_DESCRIPTOR_IDS:
        relation = admitted_registry_relations(root)[descriptor_id]
        descriptor_rows = rows[descriptor_id]
        row_count = len(descriptor_rows)
        row_set_digest = base_claim_row_set_digest(descriptor_id, descriptor_rows)
        row_bindings: list[dict[str, Any]] = []
        for row in descriptor_rows:
            row_identity = row[BASE_RESULT_FIELDS[descriptor_id]]
            if descriptor_id == "source.span.v1":
                matching_span_bindings = [
                    binding
                    for binding in span_bundle_bindings
                    if binding["bundle"]["span_id"] == row_identity
                ]
                associations = [
                    _span_base_origin_association(binding)
                    for binding in matching_span_bindings
                ]
                if not associations:
                    fail(
                        "materialization.claim-invalid",
                        "source span row lacks its validated bundle/task edge",
                    )
            else:
                associations = [
                    _request_base_origin_association(
                        descriptor_id,
                        row,
                        task,
                        catalog_entries,
                    )
                    for task in _originating_tasks(
                        descriptor_id,
                        row,
                        request["tasks"],
                    )
                ]
            associations.sort(key=_origin_association_key)
            row_bindings.append(
                {
                    "row_identity": row_identity,
                    "row_digest": base_claim_row_digest(descriptor_id, row),
                    "row_canonical_form": detached_row_canonical_form(
                        relation, row
                    ),
                    "origin_associations": associations,
                }
            )
        row_bindings.sort(
            key=lambda binding: (
                binding["row_identity"],
                binding["row_digest"],
                tuple(
                    _origin_association_key(association)
                    for association in binding["origin_associations"]
                ),
            )
        )
        row_binding_digest = semantic_digest(
            "cxxlens.base-claim-row-envelope-binding-set.v2",
            _canonical_projection_value(
                {
                    "descriptor_id": descriptor_id,
                    "row_envelope_bindings": row_bindings,
                }
            ),
        )
        condition_digest = semantic_digest(
            "cxxlens.base-claim-condition-fragment-set.v1",
            _canonical_projection_value(
                {
                    "descriptor_id": descriptor_id,
                    "row_bindings": [
                        {
                            "row_identity": binding["row_identity"],
                            "contexts": [
                                {
                                    "provider_task_id": context["provider_task_id"],
                                    "task_input_digest": context["task_input_digest"],
                                    "selected_catalog_compile_unit_id": context[
                                        "selected_catalog_compile_unit_id"
                                    ],
                                    "compile_unit_id": context["compile_unit_id"],
                                    "condition_universe_id": context[
                                        "condition_universe_id"
                                    ],
                                    "condition_id": context["condition_id"],
                                }
                                for association in binding["origin_associations"]
                                for context in [association["originating_task"]]
                            ],
                        }
                        for binding in row_bindings
                    ],
                }
            ),
        )
        interpretation_digest = semantic_digest(
            "cxxlens.base-claim-interpretation-domain-set.v1",
            _canonical_projection_value(
                {
                    "descriptor_id": descriptor_id,
                    "row_bindings": [
                        {
                            "row_identity": binding["row_identity"],
                            "contexts": [
                                {
                                    "provider_task_id": context["provider_task_id"],
                                    "task_input_digest": context["task_input_digest"],
                                    "selected_catalog_compile_unit_id": context[
                                        "selected_catalog_compile_unit_id"
                                    ],
                                    "compile_unit_id": context["compile_unit_id"],
                                    "interpretation_domain": context[
                                        "interpretation_domain"
                                    ],
                                }
                                for association in binding["origin_associations"]
                                for context in [association["originating_task"]]
                            ],
                        }
                        for binding in row_bindings
                    ],
                }
            ),
        )
        provenance_digest = semantic_digest(
            "cxxlens.base-claim-provenance-edge-set.v2",
            _canonical_projection_value(
                {
                    "descriptor_id": descriptor_id,
                    "rows": [
                        {
                            "row_identity": binding["row_identity"],
                            "row_digest": binding["row_digest"],
                            "provenance_edges": [
                                association["provenance_edge"]
                                for association in binding["origin_associations"]
                            ],
                        }
                        for binding in row_bindings
                    ],
                }
            ),
        )
        evidence_digest = semantic_digest(
            "cxxlens.base-claim-evidence-edge-set.v2",
            _canonical_projection_value(
                {
                    "descriptor_id": descriptor_id,
                    "rows": [
                        {
                            "row_identity": binding["row_identity"],
                            "row_digest": binding["row_digest"],
                            "evidence_edges": [
                                association["evidence_edges"]
                                for association in binding["origin_associations"]
                            ],
                        }
                        for binding in row_bindings
                    ],
                }
            ),
        )
        envelope_projection = {
            "descriptor_id": descriptor_id,
            "row_set_digest": row_set_digest,
            "row_envelope_bindings": row_bindings,
            "row_envelope_binding_set_digest": row_binding_digest,
            "condition_fragment_set_digest": condition_digest,
            "interpretation_domain_set_digest": interpretation_digest,
            "producer_identity_digest": producer_digest,
            "provenance_edge_set_digest": provenance_digest,
            "evidence_edge_set_digest": evidence_digest,
            "guarantee_digest": guarantee_digest,
        }
        results.append(
            {
                "descriptor_id": descriptor_id,
                "row_count": row_count,
                "claim_count": row_count,
                **{key: value for key, value in envelope_projection.items() if key != "descriptor_id"},
                "envelope_set_digest": semantic_digest(
                    "cxxlens.base-claim-envelope-set.v1",
                    _canonical_projection_value(envelope_projection),
                ),
            }
        )
    total_rows = sum(result["row_count"] for result in results)
    total_claims = sum(result["claim_count"] for result in results)
    return {
        "descriptor_ids": BASE_DESCRIPTOR_IDS,
        "stage": "canonical_claim",
        "transaction_visibility": "unpublished-until-single-commit",
        "descriptor_results": results,
        "total_row_count": total_rows,
        "total_claim_count": total_claims,
        "claim_set_digest": semantic_digest(
            "cxxlens.base-claim-set.v1",
            _canonical_projection_value(results),
        ),
        "validated_before_hard_references": True,
    }


def _task_component_rows(
    results: Iterable[dict[str, Any]],
    component: str,
) -> list[dict[str, Any]]:
    return sorted(
        (
            {
                "semantic_task_key": semantic_result_key(result),
                "component_digest": result["side_channel_components"][component],
            }
            for result in results
        ),
        key=lambda row: tuple(row["semantic_task_key"]),
    )


def semantic_result_key(result: dict[str, Any]) -> list[str]:
    return [
        result["provider_task_id"],
        result["task_input_digest"],
        result["selected_catalog_compile_unit_id"],
        result["compile_unit_id"],
    ]


def expected_global_side_channel_digest(
    channel: str,
    summary: dict[str, Any],
    results: Iterable[dict[str, Any]],
) -> str:
    component = {
        "transport_coverage": "transport_coverage_set_digest",
        "coverage": "semantic_coverage_set_digest",
        "unresolved": "unresolved_set_digest",
        "evidence": "evidence_set_digest",
    }[channel]
    projection = {key: value for key, value in summary.items() if key != "digest"}
    projection["task_components"] = _task_component_rows(results, component)
    domain = {
        "transport_coverage": "cxxlens.clang22-global-transport-coverage.v1",
        "coverage": "cxxlens.clang22-global-coverage.v1",
        "unresolved": "cxxlens.clang22-global-unresolved.v1",
        "evidence": "cxxlens.clang22-global-evidence.v1",
    }[channel]
    return _digest_projection(domain, projection)


def expected_coverage_summary(
    results: Iterable[dict[str, Any]],
    plane: str,
) -> dict[str, Any]:
    result_list = list(results)
    records_key = f"{plane}_records"
    records = [
        record
        for result in result_list
        for record in result["coverage"][records_key]
    ]
    state_counts = {state: 0 for state in COVERAGE_STATES}
    for record in records:
        state = record["state"]
        if state not in state_counts:
            fail(
                "materialization.coverage-incomplete",
                f"unknown {plane} coverage state",
            )
        state_counts[state] += 1
    channel = "transport_coverage" if plane == "transport" else "coverage"
    summary = {
        "record_type": (
            "typed-transport-coverage-unit"
            if plane == "transport"
            else "typed-coverage-unit"
        ),
        "record_count": len(records),
        "state_counts": state_counts,
        "balance": "exact",
        "digest": "pending",
    }
    summary["digest"] = expected_global_side_channel_digest(
        channel,
        summary,
        result_list,
    )
    return summary


def aggregate_observation_equivalence_census(
    results: Iterable[dict[str, Any]],
    descriptor_id: str,
) -> dict[str, Any]:
    rows: list[dict[str, Any]] = []
    for result in results:
        batch = next(
            batch
            for batch in result["batches"]
            if batch["descriptor_id"] == descriptor_id
        )
        rows.extend(batch["observation_equivalence_census"]["rows"])
    return observation_equivalence_census(descriptor_id, rows)


def observation_descriptor_censuses(
    results: Iterable[dict[str, Any]],
) -> list[dict[str, Any]]:
    result_list = list(results)
    censuses = []
    for descriptor in DESCRIPTOR_IDS[3:]:
        census = aggregate_observation_equivalence_census(result_list, descriptor)
        censuses.append(
            {
                "descriptor_id": descriptor,
                "exact_equivalence_count": census["exact_equivalence_count"],
                "non_exact_equivalence_count": census[
                    "non_exact_equivalence_count"
                ],
                "row_equivalence_set_digest": census[
                    "row_equivalence_set_digest"
                ],
            }
        )
    return censuses


def expected_guarantee_digest(
    guarantee: dict[str, Any],
    side_channels: dict[str, Any],
    results: Iterable[dict[str, Any]],
) -> str:
    projection = {key: value for key, value in guarantee.items() if key != "digest"}
    projection["global_side_channel_digests"] = {
        channel: side_channels[channel]["digest"]
        for channel in (
            "transport_coverage",
            "coverage",
            "unresolved",
            "evidence",
        )
    }
    projection["task_guarantee_fragments"] = _task_component_rows(
        results,
        "guarantee_fragment_digest",
    )
    return _digest_projection(
        "cxxlens.clang22-materialization-guarantee.v1",
        projection,
    )


def expected_claim_stage(
    results: Iterable[dict[str, Any]],
    descriptor_id: str,
    guarantee_digest: str,
    store: dict[str, Any],
) -> dict[str, Any]:
    result_list = list(results)
    final_envelopes = sorted(
        (
            envelope
            for envelope in store["claim_envelopes"]
            if envelope["role"] == "stored_final"
            and envelope["descriptor_id"] == descriptor_id
        ),
        key=lambda envelope: envelope["claim_ref"],
    )
    refs = [envelope["claim_ref"] for envelope in final_envelopes]
    contents = sorted({envelope["content"] for envelope in final_envelopes})
    ref_set = set(refs)
    association_ids = sorted(
        association["association_id"]
        for association in store["origin_associations"]
        if association["stored_claim_ref"] in ref_set
    )
    provenance_roots = sorted(
        envelope["provenance_root"] for envelope in final_envelopes
    )
    stage: dict[str, Any] = {
        "descriptor_id": descriptor_id,
        "stage": DESCRIPTOR_STAGE[descriptor_id],
        "claim_content_ids": contents,
        "claim_content_count": len(contents),
        "stored_claim_refs": refs,
        "sdk_claim_occurrence_count": len(refs),
        "origin_association_ids": association_ids,
        "origin_association_count": len(association_ids),
        "claim_content_set_digest": _digest_projection(
            "cxxlens.clang22-claim-stage-content-set.v1",
            {
                "descriptor_id": descriptor_id,
                "claim_content_ids": contents,
            },
        ),
        "sdk_claim_occurrence_set_digest": _digest_projection(
            "cxxlens.clang22-claim-stage-sdk-occurrence-set.v1",
            {"descriptor_id": descriptor_id, "stored_claim_refs": refs},
        ),
        "origin_association_set_digest": _digest_projection(
            "cxxlens.clang22-claim-stage-origin-association-set.v1",
            {
                "descriptor_id": descriptor_id,
                "origin_association_ids": association_ids,
            },
        ),
        "provenance_edge_set_digest": _digest_projection(
            "cxxlens.clang22-claim-stage-provenance-set.v1",
            {
                "descriptor_id": descriptor_id,
                "provenance_roots": provenance_roots,
            },
        ),
        "guarantee_digest": guarantee_digest,
    }
    if descriptor_id in DESCRIPTOR_IDS[3:]:
        stage["observation_equivalence_census"] = (
            aggregate_observation_equivalence_census(result_list, descriptor_id)
        )
    stage["claim_stage_digest"] = _digest_projection(
        "cxxlens.clang22-claim-stage.v1",
        stage,
    )
    return stage


def expected_global_provenance_digest(
    provenance: dict[str, Any],
    claim_stages: list[dict[str, Any]],
) -> str:
    projection = {key: value for key, value in provenance.items() if key != "edge_set_digest"}
    stages = {stage["descriptor_id"]: stage for stage in claim_stages}
    projection["canonical_claim_stages"] = [
        {
            "descriptor_id": descriptor,
            "sdk_claim_occurrence_count": stages[descriptor][
                "sdk_claim_occurrence_count"
            ],
            "origin_association_count": stages[descriptor][
                "origin_association_count"
            ],
            "provenance_edge_set_digest": stages[descriptor][
                "provenance_edge_set_digest"
            ],
            "claim_stage_digest": stages[descriptor]["claim_stage_digest"],
        }
        for descriptor in DESCRIPTOR_IDS[:3]
    ]
    return _digest_projection(
        "cxxlens.clang22-global-provenance.v1",
        projection,
    )


def claim_condition_canonical_form(condition: dict[str, Any]) -> str:
    fragments = condition["fragments"]
    if not fragments or fragments != sorted(set(fragments)):
        fail("materialization.claim-invalid", "Store condition is not canonical")
    universe = condition["universe"]
    return str(len(universe.encode("utf-8"))) + ":" + universe + "".join(
        ";" + str(len(fragment.encode("utf-8"))) + ":" + fragment
        for fragment in fragments
    )


def direct_input_basis_digest(basis_digest: str) -> str:
    identity = canonical_identity_digest("producer-input-direct", [basis_digest])
    return identity.split(":", 1)[1]


def materializer_transform_digest(
    domain: str,
    materializer_semantics_digest: str,
    engine_registry_digest: str,
) -> str:
    return semantic_digest(
        domain,
        _canonical_tuple(
            _canonical_string(value)
            for value in (
                domain,
                materializer_semantics_digest,
                engine_registry_digest,
            )
        ),
    )


def expected_materializer_semantics_digest(
    root: pathlib.Path,
    request: dict[str, Any],
) -> str:
    tool = request["tool"]
    projection = _canonical_tuple(
        (
            _canonical_string(tool["executable"]),
            _canonical_string(tool["interface_version"]),
            _canonical_string(tool["distribution_version"]),
            _canonical_string(tool["source_revision"]),
            _canonical_string(tool["source_tree"]),
            _canonical_tuple(
                _canonical_tuple(
                    (
                        _canonical_string(binding["path"]),
                        _canonical_string(binding["digest"]),
                    )
                )
                for binding in sorted(
                    authority_bindings(root),
                    key=lambda row: row["path"].encode("utf-8"),
                )
            ),
        )
    )
    return semantic_digest("cxxlens.clang22-materializer-semantics.v1", projection)


def expected_direct_basis(
    root: pathlib.Path,
    request: dict[str, Any],
) -> dict[str, str]:
    materializer_semantics = expected_materializer_semantics_digest(root, request)
    semantic_tasks = sorted(
        (
            {
                "originating_task": task_semantic_context(task),
                "task_input_digest": task["task_input_digest"],
            }
            for task in request["tasks"]
        ),
        key=lambda row: task_semantic_context_key(row["originating_task"]),
    )
    project = request["project"]
    projection = _canonical_tuple(
        (
            _canonical_string("cxxlens.clang22-direct-materialization-basis.v1"),
            _canonical_string(materializer_semantics),
            _canonical_tuple(
                (
                    _canonical_string(request["worker"]["provider_id"]),
                    _canonical_string(request["worker"]["provider_version"]),
                    _canonical_string(
                        request["worker"]["semantic_contract_digest"]
                    ),
                    _canonical_integer(request["worker"]["protocol_major"]),
                    _canonical_integer(request["worker"]["protocol_minor"]),
                )
            ),
            _canonical_tuple(
                _canonical_string(project[field])
                for field in ("project_id", "catalog_id", "catalog_digest")
            ),
            _canonical_tuple(
                (
                    _canonical_string(request["engine"]["generation_contract"]),
                    _canonical_string(request["engine"]["engine_generation_id"]),
                    _canonical_string(request["engine"]["engine_registry_digest"]),
                    _canonical_projection_value(
                        request["engine"]["admitted_descriptors"]
                    ),
                )
            ),
            _canonical_tuple(
                _canonical_tuple(
                    (
                        _canonical_projection_value(row["originating_task"]),
                        _canonical_string(row["task_input_digest"]),
                    )
                )
                for row in semantic_tasks
            ),
        )
    )
    basis_digest = semantic_digest(
        "cxxlens.clang22-direct-materialization-basis.v1",
        projection,
    )
    return {
        "projection_version": "cxxlens.clang22-direct-materialization-basis.v1",
        "materializer_semantics_digest": materializer_semantics,
        "basis_digest": basis_digest,
        "producer_input_basis_digest": direct_input_basis_digest(basis_digest),
        "canonical_adoption_transform_digest": materializer_transform_digest(
            "cxxlens.clang22-canonical-adoption-transform.v1",
            materializer_semantics,
            request["engine"]["engine_registry_digest"],
        ),
        "base_ingestion_transform_digest": materializer_transform_digest(
            "cxxlens.clang22-base-ingestion-transform.v1",
            materializer_semantics,
            request["engine"]["engine_registry_digest"],
        ),
    }


def expected_assumption_set_id(assumptions: list[str]) -> str:
    if assumptions != sorted(set(assumptions)):
        fail("materialization.claim-invalid", "guarantee assumptions are not canonical")
    return "assumption-set:" + semantic_digest(
        "cxxlens.clang22-assumption-set.v1",
        _canonical_tuple(_canonical_string(value) for value in assumptions),
    )


def claim_input_basis_digest(input_basis: dict[str, Any]) -> str:
    if input_basis != {
        "kind": "direct",
        "basis_digest": input_basis.get("basis_digest"),
    }:
        fail("materialization.claim-invalid", "reported claim basis is not exact direct basis")
    return direct_input_basis_digest(input_basis["basis_digest"])


def sdk_claim_occurrence_projection(envelope: dict[str, Any]) -> bytes:
    stage = {"assertion": 0, "canonical_claim": 1}.get(envelope["stage"])
    if stage is None:
        fail("materialization.claim-invalid", "reported claim stage is not an SDK stage")
    basis = _canonical_tuple(
        (
            _canonical_string("direct"),
            _canonical_string(envelope["input_basis"]["basis_digest"]),
        )
    )
    guarantee = envelope["guarantee"]
    modalities = guarantee["verification_modalities"]
    if modalities != sorted(set(modalities)):
        fail("materialization.claim-invalid", "claim guarantee modalities are not canonical")
    return _canonical_tuple(
        (
            _canonical_string(envelope["descriptor_id"]),
            _canonical_string(envelope["semantic_key"]),
            _canonical_string(envelope["interpretation"]),
            _canonical_string(envelope["assertion"]),
            _canonical_string(envelope["content"]),
            _canonical_string(envelope["row_canonical_form"]),
            _canonical_string(claim_condition_canonical_form(envelope["presence"])),
            _canonical_integer(stage),
            _canonical_string(envelope["producer"]["id"]),
            _canonical_string(envelope["producer"]["semantic_contract"]),
            basis,
            _canonical_string(envelope["provenance_root"]),
            _canonical_string(guarantee["approximation"]),
            _canonical_string(guarantee["scope"]),
            _canonical_string(guarantee["assumptions"]),
            _canonical_tuple(_canonical_string(value) for value in modalities),
        )
    )


def sdk_claim_batch_content_digest(envelopes: Iterable[dict[str, Any]]) -> str:
    ordered = sorted(
        (
            sdk_claim_occurrence_projection(envelope),
            envelope["content"],
        )
        for envelope in envelopes
    )
    claim_records = _canonical_tuple(
        _canonical_tuple(
            (
                _canonical_string("claim"),
                _canonical_string(content),
                _canonical_bytes(occurrence),
            )
        )
        for occurrence, content in ordered
    )
    encoded = _canonical_tuple(
        (
            _canonical_string("cxxlens.claim-batch.v2"),
            claim_records,
            _canonical_tuple(()),
            _canonical_tuple(()),
            _canonical_tuple(()),
        )
    )
    return semantic_digest("cxxlens.claim-batch.v2", encoded)


def _bind_sdk_claim_envelope(
    root: pathlib.Path,
    envelope: dict[str, Any],
) -> dict[str, Any]:
    envelope = copy.deepcopy(envelope)
    envelope["row_ref"] = canonical_identity_digest(
        "materialization-claim-row",
        [envelope["descriptor_id"], envelope["row_canonical_form"].encode("utf-8")],
    )
    semantic_key, assertion, content = sdk_claim_identities(root, envelope)
    envelope.update(
        {
            "semantic_key": semantic_key,
            "assertion": assertion,
            "content": content,
        }
    )
    singleton = sdk_claim_batch_content_digest([envelope])
    envelope["sdk_singleton_claim_batch_digest"] = singleton
    envelope["claim_ref"] = canonical_identity_digest(
        "materialization-claim-envelope",
        [envelope["role"], singleton],
    )
    return envelope


def _claim_guarantee(
    request: dict[str, Any],
    report: dict[str, Any],
) -> dict[str, Any]:
    guarantee = report["side_channels"]["guarantee"]
    guarantee.update(
        {
            "profile_id": GUARANTEE_PROFILE_ID,
            "profile_digest": expected_guarantee_profile_digest(),
            "assumptions": list(GUARANTEE_ASSUMPTIONS),
            "verification_modalities": list(GUARANTEE_MODALITIES),
        }
    )
    modalities = guarantee["verification_modalities"]
    if modalities != sorted(set(modalities)):
        fail("materialization.claim-invalid", "report modalities are not canonical")
    return {
        "approximation": "exact",
        "scope": request["project"]["project_id"],
        "assumptions": expected_assumption_set_id(guarantee["assumptions"]),
        "verification_modalities": copy.deepcopy(modalities),
    }


def _make_sdk_claim_envelope(
    root: pathlib.Path,
    *,
    role: str,
    descriptor_id: str,
    row_canonical_form: str,
    condition: dict[str, Any],
    interpretation: str,
    stage: str,
    producer: dict[str, str],
    basis_digest: str,
    provenance_root: str,
    guarantee: dict[str, Any],
) -> dict[str, Any]:
    return _bind_sdk_claim_envelope(
        root,
        {
            "claim_ref": "pending",
            "role": role,
            "row_ref": "pending",
            "row_canonical_form": row_canonical_form,
            "descriptor_id": descriptor_id,
            "semantic_key": "pending",
            "assertion": "pending",
            "content": "pending",
            "presence": copy.deepcopy(condition),
            "interpretation": interpretation,
            "stage": stage,
            "producer": copy.deepcopy(producer),
            "input_basis": {"kind": "direct", "basis_digest": basis_digest},
            "provenance_root": provenance_root,
            "guarantee": copy.deepcopy(guarantee),
            "sdk_singleton_claim_batch_digest": "pending",
        },
    )


def _canonical_basis_digest(precursor_content: str, transform_semantics: str) -> str:
    return semantic_digest(
        "cxxlens.canonical-input-basis.v1",
        precursor_content + "\n" + transform_semantics,
    )


def _claim_association(
    *,
    stored_claim_ref: str,
    originating_task: dict[str, Any],
    sealed_row_digest: str,
    source_evidence_digest: str | None,
) -> dict[str, Any]:
    association_id = canonical_identity_digest(
        "materialization-claim-association",
        [
            stored_claim_ref,
            list(task_semantic_context_key(originating_task)),
            sealed_row_digest,
            source_evidence_digest or "",
        ],
    )
    return {
        "association_id": association_id,
        "stored_claim_ref": stored_claim_ref,
        "originating_task": copy.deepcopy(originating_task),
        "sealed_row_digest": sealed_row_digest,
        "source_evidence_digest": source_evidence_digest,
    }


def coverage_unit_identity(unit: dict[str, str]) -> str:
    return canonical_identity_digest(
        "coverage-unit",
        [unit["domain"], unit["key"], unit["state"], unit["reason"]],
    )


def _semantic_task_key(context: dict[str, Any]) -> str:
    return canonical_identity_digest(
        "materialization-task",
        [
            context[field]
            for field in (
                "provider_task_id",
                "task_input_digest",
                "selected_catalog_compile_unit_id",
                "compile_unit_id",
                "condition_universe_id",
                "condition_id",
                "interpretation_domain",
            )
        ],
    )


def _coverage_units(
    context: dict[str, Any],
    descriptor_id: str,
    *,
    base: bool,
) -> list[dict[str, str]]:
    task_key = _semantic_task_key(context)
    if base:
        return [
            {
                "domain": "materialization.base-descriptor",
                "key": canonical_identity_digest(
                    "materialization-base-descriptor", [task_key, descriptor_id]
                ),
                "state": "covered",
                "reason": "",
            }
        ]
    return [
        {
            "domain": "materialization.task",
            "key": task_key,
            "state": "covered",
            "reason": "",
        },
        {
            "domain": "materialization.dependency-group",
            "key": canonical_identity_digest(
                "materialization-dependency-group",
                [task_key, DESCRIPTOR_GROUP[descriptor_id]],
            ),
            "state": "covered",
            "reason": "",
        },
    ]


def _canonical_claim_basis_digest(
    precursor_content: str,
    transform_semantics: str,
) -> str:
    basis = semantic_digest(
        "cxxlens.canonical-input-basis.v1",
        precursor_content + "\n" + transform_semantics,
    )
    return direct_input_basis_digest(basis)


def _empty_partition_basis_digest(
    direct_basis_digest: str,
    descriptor_id: str,
    condition: dict[str, Any],
    interpretation: str,
    producer_semantics: str,
    transform_semantics: str,
) -> str:
    basis = semantic_digest(
        "cxxlens.clang22-empty-partition-basis.v1",
        _canonical_tuple(
            _canonical_string(value)
            for value in (
                direct_basis_digest,
                descriptor_id,
                claim_condition_canonical_form(condition),
                interpretation,
                producer_semantics,
                transform_semantics,
            )
        ),
    )
    return direct_input_basis_digest(basis)


def _partition_identity_fields(partition: dict[str, Any]) -> list[str]:
    return [
        partition["relation_descriptor_id"],
        partition["scope"],
        claim_condition_canonical_form(partition["condition"]),
        partition["interpretation"],
        partition["producer_semantics"],
        partition["producer_input_basis_digest"],
        partition["precision_profile"],
        partition["assumption_set_id"],
    ]


def bind_store_partition_identities(partition: dict[str, Any]) -> None:
    refs = sorted(set(partition["stored_claim_refs"]))
    partition["stored_claim_refs"] = refs
    contents = sorted(set(partition["claim_content_digests"]))
    partition["claim_content_digests"] = contents
    partition["sdk_claim_occurrence_count"] = len(refs)
    partition["claim_count"] = len(contents)
    coverage_by_identity = {
        coverage_unit_identity(unit): unit for unit in partition["coverage_units"]
    }
    partition["coverage_units"] = [
        coverage_by_identity[key] for key in sorted(coverage_by_identity)
    ]
    partition["partition_id"] = canonical_identity_digest(
        "partition", _partition_identity_fields(partition)
    )
    partition["claim_set_digest"] = canonical_identity_digest("claim-set", contents)
    coverage_ids = [
        coverage_unit_identity(unit) for unit in partition["coverage_units"]
    ]
    partition["coverage_digest"] = canonical_identity_digest(
        "coverage", coverage_ids
    )
    partition["content_digest"] = canonical_identity_digest(
        "partition-content",
        [
            partition["partition_id"],
            partition["claim_set_digest"],
            partition["coverage_digest"],
        ],
    )
    partition["complete"] = (
        bool(partition["coverage_units"])
        and not partition["unresolved"]
        and all(unit["state"] == "covered" for unit in partition["coverage_units"])
    )


def _compute_store_binding(
    root: pathlib.Path,
    request: dict[str, Any],
    report: dict[str, Any],
    *,
    construct_fixture_rows: bool,
) -> tuple[
    dict[str, Any],
    dict[tuple[tuple[str, str, str], str, str], list[dict[str, Any]]],
]:
    direct_basis = expected_direct_basis(root, request)
    materializer_semantics = direct_basis["materializer_semantics_digest"]
    worker_semantics = request["worker"]["semantic_contract_digest"]
    materializer = {
        "id": "cxxlens.clang22.materializer",
        "semantic_contract": materializer_semantics,
    }
    worker = {
        "id": request["worker"]["provider_id"],
        "semantic_contract": worker_semantics,
    }
    guarantee = _claim_guarantee(request, report)
    assumption_set = guarantee["assumptions"]
    task_by_execution = {
        task_execution_key(task): task for task in request["tasks"]
    }
    task_by_context = {
        task_semantic_context_key(task_semantic_context(task)): task
        for task in request["tasks"]
    }
    supplied_store = report.get("store") or {}
    supplied_envelopes = {
        row.get("claim_ref"): row
        for row in supplied_store.get("claim_envelopes", [])
        if isinstance(row, dict)
    }
    supplied_associations = supplied_store.get("origin_associations", [])

    claim_rows: dict[str, dict[str, Any]] = {}
    envelopes: dict[str, dict[str, Any]] = {}
    edges: dict[tuple[str, str, str], dict[str, Any]] = {}
    associations: dict[str, dict[str, Any]] = {}
    final_occurrences: list[dict[str, Any]] = []
    worker_assertions: dict[
        tuple[tuple[str, str, str], str, str], list[dict[str, Any]]
    ] = {}

    def add_envelope(envelope: dict[str, Any]) -> dict[str, Any]:
        previous = envelopes.setdefault(envelope["claim_ref"], envelope)
        if previous != envelope:
            fail("materialization.claim-invalid", "claim ref aliases different occurrences")
        row = {
            "row_ref": envelope["row_ref"],
            "descriptor_id": envelope["descriptor_id"],
            "row_canonical_form": envelope["row_canonical_form"],
        }
        prior_row = claim_rows.setdefault(row["row_ref"], row)
        if prior_row != row:
            fail("materialization.claim-invalid", "claim row ref aliases different bytes")
        return previous

    def add_association(value: dict[str, Any]) -> None:
        previous = associations.setdefault(value["association_id"], value)
        if previous != value:
            fail("materialization.claim-invalid", "claim association ID collision")

    def worker_row_canonical(
        descriptor_id: str,
        binding: dict[str, Any],
    ) -> str:
        canonical_form = binding.get("row_canonical_form")
        if (
            not isinstance(canonical_form, str)
            or content_digest(canonical_form.encode("utf-8"))
            != binding["row_digest"]
        ):
            fail(
                "materialization.claim-invalid",
                "sealed worker row digest/canonical form differs",
            )
        _parse_detached_row_canonical_form(root, canonical_form, descriptor_id)
        return canonical_form

    def claim_pair(
        *,
        descriptor_id: str,
        row_canonical_form: str,
        condition: dict[str, Any],
        interpretation: str,
        provenance_root: str,
        precursor_producer: dict[str, str],
        transform: str | None,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        precursor = add_envelope(
            _make_sdk_claim_envelope(
                root,
                role=("stored_final" if transform is None else "hidden_precursor"),
                descriptor_id=descriptor_id,
                row_canonical_form=row_canonical_form,
                condition=condition,
                interpretation=interpretation,
                stage="assertion",
                producer=precursor_producer,
                basis_digest=direct_basis["basis_digest"],
                provenance_root=provenance_root,
                guarantee=guarantee,
            )
        )
        if transform is None:
            return precursor, precursor
        final = add_envelope(
            _make_sdk_claim_envelope(
                root,
                role="stored_final",
                descriptor_id=descriptor_id,
                row_canonical_form=row_canonical_form,
                condition=condition,
                interpretation=interpretation,
                stage="canonical_claim",
                producer=materializer,
                basis_digest=_canonical_basis_digest(precursor["content"], transform),
                provenance_root=provenance_root,
                guarantee=guarantee,
            )
        )
        edge = {
            "precursor_claim_ref": precursor["claim_ref"],
            "final_claim_ref": final["claim_ref"],
            "transform_semantics": transform,
        }
        edges[
            (
                edge["precursor_claim_ref"],
                edge["final_claim_ref"],
                edge["transform_semantics"],
            )
        ] = edge
        return precursor, final

    for result in report["task_results"]:
        task = task_by_execution.get(task_execution_key(result))
        if task is None:
            fail("materialization.task-binding-mismatch", "Store task is unknown")
        context = task_semantic_context(task)
        for batch in result["batches"]:
            descriptor_id = batch["descriptor_id"]
            expected_roots = sorted(
                _digest_projection(
                    "cxxlens.clang22-fixture-provenance-edge.v2",
                    {
                        "descriptor_id": descriptor_id,
                        "originating_task": binding["originating_task"],
                        "row_digest": binding["row_digest"],
                    },
                )
                for binding in batch["row_bindings"]
            )
            if batch["provenance_edge_digests"] != expected_roots:
                fail(
                    "materialization.claim-invalid",
                    "worker row/provenance occurrence binding differs",
                )
            for binding in batch["row_bindings"]:
                if binding["originating_task"] != context:
                    fail(
                        "materialization.task-binding-mismatch",
                        "worker row is attributed to another task",
                    )
                provenance_root = _digest_projection(
                    "cxxlens.clang22-fixture-provenance-edge.v2",
                    {
                        "descriptor_id": descriptor_id,
                        "originating_task": binding["originating_task"],
                        "row_digest": binding["row_digest"],
                    },
                )
                condition = {
                    "universe": context["condition_universe_id"],
                    "fragments": [context["condition_id"]],
                }
                precursor, final = claim_pair(
                    descriptor_id=descriptor_id,
                    row_canonical_form=worker_row_canonical(descriptor_id, binding),
                    condition=condition,
                    interpretation=context["interpretation_domain"],
                    provenance_root=provenance_root,
                    precursor_producer=worker,
                    transform=(
                        direct_basis["canonical_adoption_transform_digest"]
                        if DESCRIPTOR_STAGE[descriptor_id] == "canonical_claim"
                        else None
                    ),
                )
                if (
                    not construct_fixture_rows
                    and binding["worker_assertion_claim_ref"]
                    != precursor["claim_ref"]
                ):
                    fail(
                        "materialization.claim-invalid",
                        "worker row assertion claim ref differs",
                    )
                key = (task_execution_key(task), descriptor_id, binding["row_digest"])
                worker_assertions.setdefault(key, []).append(precursor)
                association = _claim_association(
                    stored_claim_ref=final["claim_ref"],
                    originating_task=context,
                    sealed_row_digest=binding["row_digest"],
                    source_evidence_digest=(
                        binding["primary_span_bundle_digest"]
                        or binding["limitation_digest"]
                    ),
                )
                add_association(association)
                final_occurrences.append(
                    {
                        "envelope": final,
                        "context": context,
                        "association_id": association["association_id"],
                        "base": False,
                    }
                )

    base_rows = base_claim_rows(root, request)
    span_rows, _ = validated_span_rows(
        request,
        report["span_validation"]["validated_bundle_bindings"],
    )
    base_rows["source.span.v1"] = span_rows
    base_by_identity = {
        descriptor_id: {
            row[BASE_RESULT_FIELDS[descriptor_id]]: row
            for row in rows
        }
        for descriptor_id, rows in base_rows.items()
    }
    base_seen: set[tuple[tuple[str, ...], str]] = set()
    for result in report["base_claims"]["descriptor_results"]:
        descriptor_id = result["descriptor_id"]
        relation = admitted_registry_relations(root)[descriptor_id]
        for binding in result["row_envelope_bindings"]:
            base_row = base_by_identity[descriptor_id].get(binding["row_identity"])
            if (
                base_row is None
                or base_claim_row_digest(descriptor_id, base_row)
                != binding["row_digest"]
            ):
                fail("materialization.claim-invalid", "base Store row binding differs")
            row_canonical_form = detached_row_canonical_form(relation, base_row)
            for base_association in binding["origin_associations"]:
                context = base_association["originating_task"]
                context_key = task_semantic_context_key(context)
                if context_key not in task_by_context:
                    fail(
                        "materialization.claim-invalid",
                        "base occurrence references an unknown semantic task",
                    )
                base_seen.add((context_key, descriptor_id))
                condition = {
                    "universe": context["condition_universe_id"],
                    "fragments": [context["condition_id"]],
                }
                precursor, final = claim_pair(
                    descriptor_id=descriptor_id,
                    row_canonical_form=row_canonical_form,
                    condition=condition,
                    interpretation=context["interpretation_domain"],
                    provenance_root=base_association["provenance_edge"][
                        "subject_digest"
                    ],
                    precursor_producer=materializer,
                    transform=direct_basis["base_ingestion_transform_digest"],
                )
                source_evidence = _digest_projection(
                    "cxxlens.clang22-base-source-evidence.v1",
                    base_association["evidence_edges"],
                )
                association = _claim_association(
                    stored_claim_ref=final["claim_ref"],
                    originating_task=context,
                    sealed_row_digest=binding["row_digest"],
                    source_evidence_digest=source_evidence,
                )
                add_association(association)
                final_occurrences.append(
                    {
                        "envelope": final,
                        "context": context,
                        "association_id": association["association_id"],
                        "base": True,
                    }
                )

    partition_groups: dict[tuple[str, ...], dict[str, Any]] = {}

    def partition_for(
        descriptor_id: str,
        context: dict[str, Any],
        producer_semantics: str,
        producer_input_basis_digest: str,
        *,
        empty: bool,
        base: bool,
    ) -> dict[str, Any]:
        condition = {
            "universe": context["condition_universe_id"],
            "fragments": [context["condition_id"]],
        }
        candidate = {
            "relation_descriptor_id": descriptor_id,
            "scope": request["project"]["project_id"],
            "condition": condition,
            "interpretation": context["interpretation_domain"],
            "producer_semantics": producer_semantics,
            "producer_input_basis_digest": producer_input_basis_digest,
            "precision_profile": "exact",
            "assumption_set_id": assumption_set,
            "empty_partition": empty,
            "stored_claim_refs": [],
            "claim_content_digests": [],
            "sdk_claim_occurrence_count": 0,
            "origin_association_count": 0,
            "coverage_units": [],
            "unresolved": [],
            "partition_id": "pending",
            "claim_set_digest": "pending",
            "coverage_digest": "pending",
            "content_digest": "pending",
            "claim_count": 0,
            "complete": True,
        }
        key = tuple(_partition_identity_fields(candidate))
        partition = partition_groups.setdefault(key, candidate)
        if partition["empty_partition"] != empty:
            fail("materialization.claim-invalid", "empty/nonempty partition identity alias")
        partition["coverage_units"].extend(
            _coverage_units(context, descriptor_id, base=base)
        )
        return partition

    association_rows = list(associations.values())
    for occurrence in final_occurrences:
        envelope = occurrence["envelope"]
        context = occurrence["context"]
        partition = partition_for(
            envelope["descriptor_id"],
            context,
            envelope["producer"]["semantic_contract"],
            claim_input_basis_digest(envelope["input_basis"]),
            empty=False,
            base=occurrence["base"],
        )
        partition["stored_claim_refs"].append(envelope["claim_ref"])
        partition["claim_content_digests"].append(envelope["content"])
    for partition in partition_groups.values():
        refs = set(partition["stored_claim_refs"])
        partition["origin_association_count"] = sum(
            association["stored_claim_ref"] in refs
            for association in association_rows
        )

    for result in report["task_results"]:
        task = task_by_execution[task_execution_key(result)]
        context = task_semantic_context(task)
        for batch in result["batches"]:
            if batch["row_bindings"]:
                continue
            descriptor_id = batch["descriptor_id"]
            canonical = DESCRIPTOR_STAGE[descriptor_id] == "canonical_claim"
            producer_semantics = materializer_semantics if canonical else worker_semantics
            transform = (
                direct_basis["canonical_adoption_transform_digest"]
                if canonical
                else worker_semantics
            )
            condition = {
                "universe": context["condition_universe_id"],
                "fragments": [context["condition_id"]],
            }
            partition_for(
                descriptor_id,
                context,
                producer_semantics,
                _empty_partition_basis_digest(
                    direct_basis["basis_digest"],
                    descriptor_id,
                    condition,
                    context["interpretation_domain"],
                    producer_semantics,
                    transform,
                ),
                empty=True,
                base=False,
            )
    for context_key, task in task_by_context.items():
        context = task_semantic_context(task)
        for descriptor_id in BASE_DESCRIPTOR_IDS:
            if (context_key, descriptor_id) in base_seen:
                continue
            condition = {
                "universe": context["condition_universe_id"],
                "fragments": [context["condition_id"]],
            }
            partition_for(
                descriptor_id,
                context,
                materializer_semantics,
                _empty_partition_basis_digest(
                    direct_basis["basis_digest"],
                    descriptor_id,
                    condition,
                    context["interpretation_domain"],
                    materializer_semantics,
                    direct_basis["base_ingestion_transform_digest"],
                ),
                empty=True,
                base=True,
            )

    partitions = list(partition_groups.values())
    for partition in partitions:
        bind_store_partition_identities(partition)
    partitions.sort(key=lambda partition: partition["partition_id"])
    manifests = [
        {
            "partition_id": partition["partition_id"],
            "relation_descriptor_id": partition["relation_descriptor_id"],
            "input_basis_digest": partition["producer_input_basis_digest"],
            "claim_set_digest": partition["claim_set_digest"],
            "coverage_digest": partition["coverage_digest"],
            "content_digest": partition["content_digest"],
            "claim_count": partition["claim_count"],
            "complete": partition["complete"],
        }
        for partition in partitions
    ]
    snapshot = {
        "schema": "cxxlens.snapshot-manifest.v1",
        "snapshot_semantics_version": "1.0.0",
        "catalog_semantic_digest": request["project"]["catalog_digest"],
        "condition_universe_id": request["publication"]["selector"][
            "condition_universe_id"
        ],
        "relation_registry_digest": request["engine"]["engine_registry_digest"],
        "interpretation_policy_digest": request["interpretation_policy"][
            "interpretation_policy_digest"
        ],
        "partitions": manifests,
        "closure_ids": [],
        "snapshot_id": "pending",
    }
    snapshot["snapshot_id"] = canonical_identity_digest(
        "snapshot",
        [
            snapshot["snapshot_semantics_version"],
            snapshot["catalog_semantic_digest"],
            snapshot["condition_universe_id"],
            snapshot["relation_registry_digest"],
            snapshot["interpretation_policy_digest"],
            [
                [
                    manifest["partition_id"],
                    manifest["content_digest"],
                    manifest["coverage_digest"],
                ]
                for manifest in manifests
            ],
            [],
        ],
    )
    final_by_ref = {
        occurrence["envelope"]["claim_ref"]: occurrence["envelope"]
        for occurrence in final_occurrences
    }
    final_envelopes = [final_by_ref[key] for key in sorted(final_by_ref)]
    claim_batch = {
        "contract": "cxxlens.claim-batch.v2",
        "final_claim_refs": sorted(final_by_ref),
        "sdk_claim_occurrence_count": len(final_envelopes),
        "unique_claim_content_count": len(
            {envelope["content"] for envelope in final_envelopes}
        ),
        "unresolved_count": 0,
        "conflict_count": 0,
        "differential_disagreement_count": 0,
        "content_digest": sdk_claim_batch_content_digest(final_envelopes),
    }
    store = {
        "selector": {
            "fields": copy.deepcopy(request["publication"]["selector"]),
            "series_id": request["publication"]["series_id"],
        },
        "direct_basis": direct_basis,
        "claim_rows": [claim_rows[key] for key in sorted(claim_rows)],
        "claim_envelopes": [envelopes[key] for key in sorted(envelopes)],
        "canonicalization_edges": [edges[key] for key in sorted(edges)],
        "origin_associations": [associations[key] for key in sorted(associations)],
        "claim_batch_validation": claim_batch,
        "partitions": partitions,
        "snapshot_manifest": snapshot,
    }
    return store, worker_assertions


def expected_store_binding(
    root: pathlib.Path,
    request: dict[str, Any],
    report: dict[str, Any],
    *,
    construct_fixture_rows: bool = False,
) -> dict[str, Any]:
    store, _ = _compute_store_binding(
        root,
        request,
        report,
        construct_fixture_rows=construct_fixture_rows,
    )
    return store


def bind_batch_sdk_claim_summaries(
    root: pathlib.Path,
    request: dict[str, Any],
    report: dict[str, Any],
    *,
    construct_fixture_rows: bool,
) -> dict[str, Any]:
    store, assertions = _compute_store_binding(
        root,
        request,
        report,
        construct_fixture_rows=construct_fixture_rows,
    )
    for result in report["task_results"]:
        execution_key = task_execution_key(result)
        for batch in result["batches"]:
            claims: dict[str, dict[str, Any]] = {}
            for binding in batch["row_bindings"]:
                key = (execution_key, batch["descriptor_id"], binding["row_digest"])
                binding_claims = assertions.get(key, [])
                binding_refs = {envelope["claim_ref"] for envelope in binding_claims}
                if len(binding_refs) != 1:
                    fail(
                        "materialization.claim-invalid",
                        "worker row does not bind exactly one assertion occurrence",
                    )
                binding["worker_assertion_claim_ref"] = next(iter(binding_refs))
                for envelope in binding_claims:
                    previous = claims.setdefault(envelope["claim_ref"], envelope)
                    if previous != envelope:
                        fail(
                            "materialization.claim-invalid",
                            "worker assertion ref aliases different occurrences",
                        )
            ordered = [claims[key] for key in sorted(claims)]
            batch["worker_assertion_claim_refs"] = [
                envelope["claim_ref"] for envelope in ordered
            ]
            batch["worker_assertion_claim_occurrence_count"] = len(ordered)
            batch["claim_content_ids"] = sorted(
                {envelope["content"] for envelope in ordered}
            )
            batch["claim_content_count"] = len(batch["claim_content_ids"])
    return store


def _publication_identity_projection(record: dict[str, Any]) -> dict[str, Any]:
    return {
        key: record[key]
        for key in (
            "publication_id",
            "series_id",
            "snapshot_id",
            "sequence",
            "parent_publication",
        )
    }


def _reopened_annotation(envelope: dict[str, Any]) -> dict[str, Any]:
    return {
        "relation_descriptor_id": envelope["descriptor_id"],
        "row_canonical_form": envelope["row_canonical_form"],
        "presence": copy.deepcopy(envelope["presence"]),
        "interpretation": envelope["interpretation"],
        "semantic_key": envelope["semantic_key"],
        "assertion": envelope["assertion"],
        "content": envelope["content"],
        "producer": copy.deepcopy(envelope["producer"]),
        "provenance_root": envelope["provenance_root"],
        "guarantee": copy.deepcopy(envelope["guarantee"]),
    }


def _reopened_handle_projection(
    request: dict[str, Any],
    store: dict[str, Any],
    record: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]]:
    descriptors = sorted(
        copy.deepcopy(request["engine"]["admitted_descriptors"]),
        key=lambda row: row["descriptor_id"],
    )
    final_envelopes = [
        envelope
        for envelope in store["claim_envelopes"]
        if envelope["role"] == "stored_final"
    ]
    annotations = sorted(
        (_reopened_annotation(envelope) for envelope in final_envelopes),
        key=lambda row: canonical_json(row),
    )
    rows_by_descriptor: dict[str, list[str]] = {
        descriptor["descriptor_id"]: [] for descriptor in descriptors
    }
    annotations_by_descriptor: dict[str, list[dict[str, Any]]] = {
        descriptor["descriptor_id"]: [] for descriptor in descriptors
    }
    coverage_by_descriptor: dict[str, list[dict[str, Any]]] = {
        descriptor["descriptor_id"]: [] for descriptor in descriptors
    }
    for envelope in final_envelopes:
        rows_by_descriptor[envelope["descriptor_id"]].append(
            envelope["row_canonical_form"]
        )
    for annotation in annotations:
        annotations_by_descriptor[annotation["relation_descriptor_id"]].append(
            annotation
        )
    coverage: list[dict[str, Any]] = []
    for partition in store["partitions"]:
        descriptor_id = partition["relation_descriptor_id"]
        for unit in partition["coverage_units"]:
            row = {
                "relation_descriptor_id": descriptor_id,
                "unit": copy.deepcopy(unit),
            }
            coverage.append(row)
            coverage_by_descriptor[descriptor_id].append(copy.deepcopy(unit))
    coverage.sort(key=lambda row: canonical_json(row))
    partition_bindings = [
        {
            key: copy.deepcopy(partition[key])
            for key in (
                "partition_id",
                "relation_descriptor_id",
                "scope",
                "condition",
                "interpretation",
                "producer_semantics",
                "producer_input_basis_digest",
                "precision_profile",
                "assumption_set_id",
            )
        }
        for partition in store["partitions"]
    ]
    partition_bindings.sort(key=lambda row: row["partition_id"])
    relations = []
    for descriptor in descriptors:
        descriptor_id = descriptor["descriptor_id"]
        relation = {
            "relation_descriptor_id": descriptor_id,
            "row_canonical_forms": sorted(set(rows_by_descriptor[descriptor_id])),
            "claim_annotations": sorted(
                annotations_by_descriptor[descriptor_id],
                key=lambda row: canonical_json(row),
            ),
            "coverage": sorted(
                coverage_by_descriptor[descriptor_id],
                key=lambda row: canonical_json(row),
            ),
        }
        relations.append(relation)
    cursor = {
        "specification": "cxxlens.clang22-materialization-reopen-cursor.v1",
        "relations": relations,
        "digest": "pending",
    }
    cursor["digest"] = _digest_projection(
        "cxxlens.clang22-materialization-reopen-cursor.v1",
        {key: cursor[key] for key in ("specification", "relations")},
    )
    canonical_export_projection = {
        "schema": "cxxlens.snapshot-export.v1",
        "snapshot_manifest": store["snapshot_manifest"],
        "claim_contents": sorted(
            {envelope["content"] for envelope in final_envelopes}
        ),
        "rows": relations,
        "partition_bindings": partition_bindings,
        "partition_envelopes": [
            {
                "partition_id": partition["partition_id"],
                "stored_claim_refs": partition["stored_claim_refs"],
                "coverage_units": partition["coverage_units"],
                "unresolved": partition["unresolved"],
            }
            for partition in store["partitions"]
        ],
    }
    canonical_export_digest = content_digest(
        canonical_json(canonical_export_projection)
    )
    semantic_fields = {
        "backend": request["publication"]["backend"],
        "snapshot_manifest": store["snapshot_manifest"],
        "snapshot_manifest_digest": content_digest(
            canonical_json(store["snapshot_manifest"])
        ),
        "descriptors": descriptors,
        "partition_binding_multiset_digest": _digest_projection(
            "cxxlens.clang22-reopened-partition-binding-multiset.v1",
            partition_bindings,
        ),
        "row_multiset_digest": _digest_projection(
            "cxxlens.clang22-reopened-row-multiset.v1",
            [
                [relation["relation_descriptor_id"], relation["row_canonical_forms"]]
                for relation in relations
            ],
        ),
        "claim_annotation_multiset_digest": _digest_projection(
            "cxxlens.clang22-reopened-claim-annotation-multiset.v1",
            annotations,
        ),
        "coverage_multiset_digest": _digest_projection(
            "cxxlens.clang22-reopened-coverage-multiset.v1",
            coverage,
        ),
        "unresolved_digest": _digest_projection(
            "cxxlens.clang22-reopened-unresolved.v1", []
        ),
        "closure_digest": _digest_projection(
            "cxxlens.clang22-reopened-closure.v1", []
        ),
        "cursor_projection_digest": cursor["digest"],
        "canonical_export_digest": canonical_export_digest,
    }
    semantic_digest_value = _digest_projection(
        "cxxlens.clang22-reopened-semantic-projection.v1", semantic_fields
    )
    projection = {
        "backend": semantic_fields["backend"],
        "publication_record": copy.deepcopy(record),
        "snapshot_manifest": copy.deepcopy(store["snapshot_manifest"]),
        "snapshot_manifest_digest": semantic_fields["snapshot_manifest_digest"],
        "descriptors": descriptors,
        "partition_binding_multiset_digest": semantic_fields[
            "partition_binding_multiset_digest"
        ],
        "row_multiset_digest": semantic_fields["row_multiset_digest"],
        "claim_annotation_multiset_digest": semantic_fields[
            "claim_annotation_multiset_digest"
        ],
        "coverage_multiset_digest": semantic_fields["coverage_multiset_digest"],
        "unresolved_digest": semantic_fields["unresolved_digest"],
        "closure_digest": semantic_fields["closure_digest"],
        "cursor_projection_digest": semantic_fields["cursor_projection_digest"],
        "canonical_export_digest": canonical_export_digest,
        "semantic_projection_digest": semantic_digest_value,
        "handle_projection_digest": "pending",
    }
    projection["handle_projection_digest"] = _digest_projection(
        "cxxlens.clang22-reopened-handle-projection.v1",
        {key: value for key, value in projection.items() if key != "handle_projection_digest"},
    )
    reopened = {
        "backend": request["publication"]["backend"],
        "selector": copy.deepcopy(store["selector"]),
        "publication_record": copy.deepcopy(record),
        "snapshot_manifest": copy.deepcopy(store["snapshot_manifest"]),
        "descriptors": descriptors,
        "partition_bindings": partition_bindings,
        "claim_annotations": annotations,
        "coverage": coverage,
        "unresolved": [],
        "canonical_export_digest": canonical_export_digest,
        "cursor_projection": cursor,
        "handle_receipts": [],
    }
    return projection, reopened


def _present_reopened_receipt(
    access_path: str,
    lookup: dict[str, Any],
    projection: dict[str, Any],
) -> dict[str, Any]:
    return {
        "access_path": access_path,
        "lookup": copy.deepcopy(lookup),
        "status": "present",
        "sdk_code": None,
        "sdk_field": None,
        "projection": copy.deepcopy(projection),
    }


def expected_reopened_store(
    request: dict[str, Any],
    store: dict[str, Any],
    record: dict[str, Any],
) -> dict[str, Any]:
    projection, reopened = _reopened_handle_projection(request, store, record)
    receipts = [
        _present_reopened_receipt(
            "current-selector", {"selector": store["selector"]}, projection
        ),
        _present_reopened_receipt(
            "open-publication",
            {"publication_id": record["publication_id"]},
            projection,
        ),
        _present_reopened_receipt(
            "open-snapshot", {"snapshot_id": record["snapshot_id"]}, projection
        ),
    ]
    reopened["handle_receipts"] = receipts
    return reopened


def validate_committed_verified_reopened_store(
    request: dict[str, Any],
    report: dict[str, Any],
) -> None:
    publication = report["publication"]
    invocation_record = publication["invocation_committed_record"]
    reopened = report["semantic_verification"]["reopened_store"]
    expected = expected_reopened_store(request, report["store"], invocation_record)
    outer_fields = {
        key: value
        for key, value in reopened.items()
        if key not in {"publication_record", "handle_receipts"}
    }
    expected_outer_fields = {
        key: value
        for key, value in expected.items()
        if key not in {"publication_record", "handle_receipts"}
    }
    if outer_fields != expected_outer_fields:
        fail(
            "materialization.store-failure",
            "reopened Store semantic projection differs",
        )
    receipts = reopened["handle_receipts"]
    expected_receipts = expected["handle_receipts"]
    semantic_projection_digests: set[str] = set()
    for receipt, expected_receipt in zip(receipts, expected_receipts):
        if (
            receipt["access_path"] != expected_receipt["access_path"]
            or receipt["lookup"] != expected_receipt["lookup"]
            or receipt["status"] != "present"
            or receipt["sdk_code"] is not None
            or receipt["sdk_field"] is not None
        ):
            fail(
                "materialization.store-failure",
                "passed reopened Store handle receipt differs",
            )
        projection = receipt["projection"]
        expected_projection = expected_receipt["projection"]
        semantic_fields = {
            key: value
            for key, value in projection.items()
            if key not in {"publication_record", "handle_projection_digest"}
        }
        expected_semantic_fields = {
            key: value
            for key, value in expected_projection.items()
            if key not in {"publication_record", "handle_projection_digest"}
        }
        if semantic_fields != expected_semantic_fields:
            fail(
                "materialization.store-failure",
                "reopened handle semantic snapshot projection differs",
            )
        _validate_reopened_handle_projection(
            projection,
            expected_series_id=(
                projection["publication_record"]["series_id"]
                if receipt["access_path"] == "open-snapshot"
                else publication["series_id"]
            ),
        )
        returned_record = projection["publication_record"]
        if receipt["access_path"] in {"current-selector", "open-publication"}:
            _validate_recovered_publication_transition(
                invocation_record,
                returned_record,
                subject=receipt["access_path"],
            )
        elif returned_record["snapshot_id"] != publication["candidate_snapshot_id"]:
            fail(
                "materialization.store-failure",
                "open-snapshot returned a different semantic snapshot",
            )
        semantic_projection_digests.add(projection["semantic_projection_digest"])
    if len(semantic_projection_digests) != 1:
        fail(
            "materialization.store-failure",
            "reopened handle semantic projections differ across access paths",
        )
    if reopened["publication_record"] != receipts[0]["projection"]["publication_record"]:
        fail(
            "materialization.store-failure",
            "reopened Store current publication record differs",
        )


def bind_committed_verified_publication(
    request: dict[str, Any],
    report: dict[str, Any],
) -> None:
    snapshot_id = report["store"]["snapshot_manifest"]["snapshot_id"]
    parent = request["publication"]["expected_parent_publication"]
    sequence = 1
    record = {
        "publication_id": "pending",
        "series_id": request["publication"]["series_id"],
        "snapshot_id": snapshot_id,
        "sequence": sequence,
        "physical_generation": 1,
        "parent_publication": parent,
        "state": "committed",
        "corrupt": False,
    }
    record["publication_id"] = canonical_identity_digest(
        "publication",
        [record["series_id"], snapshot_id, sequence, parent or ""],
    )
    backend = request["publication"]["backend"]
    report["publication"] = {
        "backend": backend,
        "selector": copy.deepcopy(request["publication"]["selector"]),
        "series_id": request["publication"]["series_id"],
        "genesis": request["publication"]["genesis"],
        "expected_parent_publication": parent,
        "observed_parent_publication": parent,
        "observed_parent_record": None,
        "head_observation": "absent",
        "publication_attempted": True,
        "outcome": "committed_verified",
        "partial_policy": "forbid",
        "candidate_snapshot_id": snapshot_id,
        "candidate_identity_state": "constructed",
        "candidate_identity": _publication_identity_projection(record),
        "invocation_commit_state": "committed",
        "committed_transaction_count": 1,
        "invocation_committed_record": copy.deepcopy(record),
        "terminal_head": {"status": "present", "record": copy.deepcopy(record)},
        "candidate_visibility": "present_by_invocation",
        "prior_history_retained": True,
        "head_effect": "advanced_to_candidate",
        "store_failure": None,
        "sqlite_effect_root_receipt": expected_sqlite_effect_root_receipt(request),
        "sqlite_reopen_status": "opened" if backend == "sqlite" else "not_applicable",
        "recovery_receipt": None,
    }
    report["semantic_verification"] = {
        "status": "passed",
        "reopened_store": expected_reopened_store(
            request, report["store"], record
        ),
        "reopen_attempt": None,
        "failure": None,
    }


def rebind_report_digest_chain(
    root: pathlib.Path,
    request: dict[str, Any],
    report: dict[str, Any],
) -> None:
    """Rebind a constructed report bottom-up; leaf records remain caller-owned."""

    tasks = {task_execution_key(task): task for task in request["tasks"]}
    results = report["task_results"]
    span_rows, span_bindings = validated_span_rows(
        request,
        report["span_validation"]["validated_bundle_bindings"],
    )
    report["span_validation"].update(span_report_digests(span_bindings, span_rows))
    report["base_claims"] = base_claim_report(
        root,
        request,
        report["side_channels"]["guarantee"]["digest"],
        span_bindings,
    )
    report["store"] = bind_batch_sdk_claim_summaries(
        root,
        request,
        report,
        construct_fixture_rows=True,
    )
    for result in results:
        task = tasks[task_execution_key(result)]
        for batch in result["batches"]:
            if batch["descriptor_id"] in DESCRIPTOR_IDS[3:]:
                census = batch["observation_equivalence_census"]
                batch["observation_equivalence_census"] = (
                    observation_equivalence_census(
                        batch["descriptor_id"],
                        census["rows"],
                    )
                )
            bind_batch_digests(task, batch)
        groups = {group["dependency_group_id"]: group for group in result["groups"]}
        for group in GROUP_DESCRIPTORS:
            groups[group]["batch_set_digest"] = expected_group_batch_set_digest(
                task,
                groups[group],
                result["batches"],
            )
        result["input_transfer"] = expected_input_transfer_receipt(request, task)
        result["coverage"]["transport_record_set_digest"] = (
            coverage_record_set_digest(
                result,
                "transport",
                result["coverage"]["transport_records"],
            )
        )
        result["coverage"]["semantic_record_set_digest"] = coverage_record_set_digest(
            result,
            "semantic",
            result["coverage"]["semantic_records"],
        )
        result["side_channel_components"].update(
            {
                "transport_coverage_set_digest": result["coverage"][
                    "transport_record_set_digest"
                ],
                "semantic_coverage_set_digest": result["coverage"][
                    "semantic_record_set_digest"
                ],
                "guarantee_profile_id": GUARANTEE_PROFILE_ID,
                "guarantee_profile_digest": expected_guarantee_profile_digest(),
            }
        )
        result["side_channel_components"]["guarantee_fragment_digest"] = (
            expected_task_guarantee_fragment_digest(result)
        )
        result["side_channel_digest"] = expected_task_side_channel_digest(result)
        result["task_result_digest"] = expected_task_result_digest(result)
    report["adoption"]["task_result_set_digest"] = expected_task_result_set_digest(
        results
    )
    report["adoption"]["raw_frames"]["frame_count"] = sum(
        result["runtime_receipt"]["frame_count"] for result in results
    )
    report["adoption"]["raw_frames"][
        "frame_set_digest"
    ] = expected_raw_frame_set_digest(results)
    report["side_channels"]["transport_coverage"] = expected_coverage_summary(
        results,
        "transport",
    )
    report["side_channels"]["coverage"] = expected_coverage_summary(
        results,
        "semantic",
    )
    for channel in ("unresolved", "evidence"):
        report["side_channels"][channel][
            "digest"
        ] = expected_global_side_channel_digest(
            channel,
            report["side_channels"][channel],
            results,
        )
    guarantee = report["side_channels"]["guarantee"]
    guarantee["observation_descriptor_censuses"] = (
        observation_descriptor_censuses(results)
    )
    guarantee["digest"] = expected_guarantee_digest(
        guarantee,
        report["side_channels"],
        results,
    )
    report["base_claims"] = base_claim_report(
        root,
        request,
        guarantee["digest"],
        span_bindings,
    )
    report["store"] = expected_store_binding(
        root,
        request,
        report,
        construct_fixture_rows=True,
    )
    report["claim_stages"] = [
        expected_claim_stage(results, descriptor, guarantee["digest"], report["store"])
        for descriptor in DESCRIPTOR_IDS
    ]
    report["provenance"]["edge_set_digest"] = expected_global_provenance_digest(
        report["provenance"],
        report["claim_stages"],
    )
    if report["result"] == "passed":
        bind_committed_verified_publication(request, report)


def sample_report(
    root: pathlib.Path,
    request: dict[str, Any],
    *,
    request_bytes: bytes | None = None,
) -> dict[str, Any]:
    configuration = request["tool"]["package_configuration"]
    backend = request["publication"]["backend"]
    task_count = len(request["tasks"])
    task_results = [_task_report(root, request, task) for task in request["tasks"]]
    observation_rows = {
        (
            task_semantic_context_key(binding["originating_task"]),
            batch["descriptor_id"],
            binding["row_digest"],
        ): binding["primary_span_bundle_digest"]
        for result in task_results
        for batch in result["batches"]
        if batch["descriptor_id"] in SPAN_OBSERVATION_DESCRIPTORS
        for binding in batch["row_bindings"]
        if binding["primary_span_bundle_digest"] is not None
    }
    span_bindings = sample_span_bundle_bindings(request, task_results)
    span_rows, _ = validated_span_rows(
        request,
        span_bindings,
        observation_rows,
    )
    span_digests = span_report_digests(span_bindings, span_rows)
    transport_coverage = expected_coverage_summary(task_results, "transport")
    coverage = expected_coverage_summary(task_results, "semantic")
    unresolved = {
        "record_type": "typed-unresolved-item",
        "record_count": 0,
        "blocking_count": 0,
        "categories": [],
        "category_counts": {},
        "digest": "pending",
    }
    evidence = {
        "record_type": "typed-evidence-edge",
        "record_count": 6 * task_count,
        "kinds": [
            "canonicalization",
            "provider_execution",
            "source_observation",
        ],
        "kind_counts": {
            "canonicalization": 2 * task_count,
            "provider_execution": 2 * task_count,
            "source_observation": 2 * task_count,
        },
        "subject_binding": "exact-claim-or-task-identity",
        "digest": "pending",
    }
    side_channels: dict[str, Any] = {
        "transport_coverage": transport_coverage,
        "coverage": coverage,
        "unresolved": unresolved,
        "evidence": evidence,
    }
    for channel in ("unresolved", "evidence"):
        side_channels[channel]["digest"] = expected_global_side_channel_digest(
            channel,
            side_channels[channel],
            task_results,
        )
    guarantee = {
        "record_type": "typed-guarantee",
        "profile_id": GUARANTEE_PROFILE_ID,
        "profile_digest": expected_guarantee_profile_digest(),
        "approximation": "exact",
        "scope": "project:fixture",
        "assumptions": list(GUARANTEE_ASSUMPTIONS),
        "verification_modalities": list(GUARANTEE_MODALITIES),
        "observation_descriptor_censuses": observation_descriptor_censuses(
            task_results
        ),
        "digest": "pending",
    }
    side_channels["guarantee"] = guarantee
    guarantee["digest"] = expected_guarantee_digest(
        guarantee,
        side_channels,
        task_results,
    )
    claim_stages: list[dict[str, Any]] = []
    provenance = {
        "record_type": "typed-provenance-edge-summary",
        "edge_count": 3 * task_count,
        "canonical_claim_count": 3 * task_count,
        "canonical_claims_with_exact_input_edges": 3 * task_count,
        "orphan_count": 0,
        "ambiguous_count": 0,
        "edge_set_digest": "pending",
    }
    provenance["edge_set_digest"] = "pending"
    exact_request_bytes = canonical_json(request) if request_bytes is None else request_bytes
    report = {
        "schema": "cxxlens.clang22-materialization-report.v2",
        "report_version": MATERIALIZATION_VERSION,
        "response_kind": "detailed",
        "result": "passed",
        "generated_at": datetime.datetime(
            2026, 7, 19, tzinfo=datetime.timezone.utc
        ).isoformat().replace("+00:00", "Z"),
        "process_exit_status": 0,
        "raw_input_observation": raw_input_observation(exact_request_bytes),
        "source": {
            "revision": request["tool"]["source_revision"],
            "tree": request["tool"]["source_tree"],
        },
        "request": {
            "materialization_request_id": request["materialization_request_id"],
            "request_digest": request["request_digest"],
            "semantic_request_digest": request["semantic_request_digest"],
        },
        "installation": {
            "requested": {
                "occurrence_manifest_digest": request["tool"][
                    "occurrence_manifest_digest"
                ]
            },
            "measured": fixture_occurrence_measurement(
                root,
                source_revision=request["tool"]["source_revision"],
                source_tree=request["tool"]["source_tree"],
                configuration=configuration,
                tool_digest=request["tool"]["installed_executable_digest"],
                worker_digest=request["worker"]["installed_binary_digest"],
            ),
        },
        "provider": {
            "tool_executable": request["tool"]["executable"],
            "tool_interface_version": request["tool"]["interface_version"],
            "worker_executable": request["worker"]["executable"],
            "provider_id": request["worker"]["provider_id"],
            "provider_version": request["worker"]["provider_version"],
            "semantic_contract_digest": request["worker"]["semantic_contract_digest"],
            "protocol_major": request["worker"]["protocol_major"],
            "protocol_minor": request["worker"]["protocol_minor"],
            "required_features": copy.deepcopy(
                request["worker"]["required_features"]
            ),
            "sandbox_policy_digest": request["worker"]["sandbox_policy_digest"],
        },
        "project": {
            key: request["project"][key]
            for key in (
                "project_id",
                "catalog_id",
                "catalog_digest",
                "logical_root",
                "catalog_environment_digest",
                "catalog_compile_unit_census_digest",
                "catalog_compile_units",
            )
        },
        "registry": {
            "authority_registry_digest": request["registry"][
                "authority_registry_digest"
            ],
            "base_descriptors": copy.deepcopy(
                request["registry"]["base_descriptors"]
            ),
            "descriptors": copy.deepcopy(request["registry"]["descriptors"]),
        },
        "engine": copy.deepcopy(request["engine"]),
        "interpretation_policy": copy.deepcopy(request["interpretation_policy"]),
        "trust_policy": copy.deepcopy(request["trust_policy"]),
        "adoption": {
            "boundary": "sealed-materialization-result",
            "visibility": "tool-private-immutable",
            "state": "sealed",
            "partial_policy": "forbid",
            "all_tasks_mandatory": True,
            "all_groups_mandatory": True,
            "all_batches_mandatory": True,
            "task_result_set_digest": expected_task_result_set_digest(task_results),
            "raw_frames": {
                "authority": "diagnostic-only-non-authoritative",
                "retained": False,
                "frame_count": sum(
                    result["runtime_receipt"]["frame_count"]
                    for result in task_results
                ),
                "frame_set_digest": expected_raw_frame_set_digest(task_results),
            },
        },
        "task_results": task_results,
        "span_validation": {
            "contract": "full-primary-span-bundle-v2",
            "bundle_fields": ["span_id", "snapshot", "file", "begin", "end", "role", "read_only"],
            "optionality": "entity-and-call-optional-all-or-none",
            "origin_evidence": "separately-retained-and-digest-bound",
            "observed_bundle_count": len(span_bindings),
            "absent_bundle_count": 0,
            "entity_absent_bundle_count": 0,
            "call_absent_bundle_count": 0,
            "absent_bundle_unresolved_count": 0,
            "source_dependent_canonical_omission_count": 0,
            "unique_bundle_count": len(span_rows),
            "constructed_source_span_claim_count": len(span_rows),
            "recomputed_id_mismatch_count": 0,
            "invalid_range_count": 0,
            "task_binding_mismatch_count": 0,
            "hard_references_resolved": True,
            "validated_bundle_bindings": span_bindings,
            **span_digests,
        },
        "base_claims": base_claim_report(
            root,
            request,
            guarantee["digest"],
            span_bindings,
        ),
        "side_channels": side_channels,
        "claim_stages": claim_stages,
        "provenance": provenance,
        "store": {},
        "publication": {
            "backend": backend,
            "selector": copy.deepcopy(request["publication"]["selector"]),
            "series_id": request["publication"]["series_id"],
            "genesis": request["publication"]["genesis"],
            "expected_parent_publication": request["publication"]["expected_parent_publication"],
            "observed_parent_publication": request["publication"]["expected_parent_publication"],
            "observed_parent_record": None,
            "head_observation": "absent",
            "publication_attempted": True,
            "outcome": "committed_verified",
            "partial_policy": "forbid",
            "candidate_snapshot_id": "pending",
            "candidate_identity_state": "constructed",
            "candidate_identity": None,
            "invocation_commit_state": "committed",
            "committed_transaction_count": 1,
            "invocation_committed_record": None,
            "terminal_head": {"status": "absent", "record": None},
            "candidate_visibility": "present_by_invocation",
            "prior_history_retained": True,
            "head_effect": "advanced_to_candidate",
            "store_failure": None,
            "sqlite_effect_root_receipt": expected_sqlite_effect_root_receipt(
                request
            ),
            "sqlite_reopen_status": "opened" if backend == "sqlite" else "not_applicable",
            "recovery_receipt": None,
        },
        "semantic_verification": {
            "status": "passed",
            "reopened_store": None,
            "reopen_attempt": None,
            "failure": None,
        },
        "authority_digests": copy.deepcopy(authority_bindings(root)),
        "error": None,
    }
    rebind_report_digest_chain(root, request, report)
    return report


def _publication_record_identity(record: dict[str, Any]) -> str:
    return canonical_identity_digest(
        "publication",
        [
            record["series_id"],
            record["snapshot_id"],
            record["sequence"],
            record["parent_publication"] or "",
        ],
    )


def _validate_publication_record(
    record: dict[str, Any],
    *,
    expected_series_id: str,
    subject: str,
) -> None:
    if (
        record["series_id"] != expected_series_id
        or record["publication_id"] != _publication_record_identity(record)
    ):
        fail(
            "materialization.store-failure",
            f"{subject} publication identity differs",
        )


def _same_semantic_publication(
    left: dict[str, Any],
    right: dict[str, Any],
) -> bool:
    return all(
        left[field] == right[field]
        for field in (
            "publication_id",
            "series_id",
            "snapshot_id",
            "sequence",
            "parent_publication",
            "state",
            "corrupt",
        )
    )


def _validate_recovered_publication_transition(
    earlier: dict[str, Any],
    recovered: dict[str, Any],
    *,
    subject: str,
) -> None:
    if (
        not _same_semantic_publication(earlier, recovered)
        or recovered["physical_generation"] < earlier["physical_generation"]
    ):
        fail(
            "materialization.store-failure",
            f"{subject} recovery publication transition differs",
        )


def _expected_recovery_selector(request: dict[str, Any]) -> dict[str, Any]:
    return {
        "fields": copy.deepcopy(request["publication"]["selector"]),
        "series_id": request["publication"]["series_id"],
    }


def _lookup_error_is_corruption(code: str | None) -> bool:
    return code is not None and "corrupt" in code


def _validate_publication_recovery_receipt(
    request: dict[str, Any],
    publication: dict[str, Any],
) -> None:
    receipt = publication["recovery_receipt"]
    if receipt is None or receipt["selector"] != _expected_recovery_selector(request):
        fail("materialization.store-failure", "publication recovery selector differs")
    candidate = publication["candidate_identity"]
    candidate_id = None if candidate is None else candidate["publication_id"]
    expected_parent = request["publication"]["expected_parent_publication"]
    lookups = receipt["current"], receipt["expected_parent"], receipt["candidate"]
    if receipt["reopen_status"] == "open_failed":
        expected_statuses = (
            "not_attempted",
            "not_attempted",
            "not_attempted" if candidate_id is not None else "not_applicable",
        )
        expected_ids = (None, expected_parent, candidate_id)
        if (
            receipt["open_error_code"] is None
            or receipt["open_error_field"] is None
            or tuple(row["status"] for row in lookups) != expected_statuses
            or tuple(row["requested_publication_id"] for row in lookups)
            != expected_ids
        ):
            fail(
                "materialization.store-failure",
                "open-failed publication recovery receipt differs",
            )
    else:
        expected_ids = (None, expected_parent, candidate_id)
        if (
            receipt["open_error_code"] is not None
            or receipt["open_error_field"] is not None
            or tuple(row["requested_publication_id"] for row in lookups)
            != expected_ids
            or (expected_parent is None)
            != (receipt["expected_parent"]["status"] == "not_applicable")
            or (candidate_id is None)
            != (receipt["candidate"]["status"] == "not_applicable")
        ):
            fail(
                "materialization.store-failure",
                "opened publication recovery receipt differs",
            )
    for name, lookup in zip(("current", "expected-parent", "candidate"), lookups):
        record = lookup["record"]
        if lookup["status"] == "present":
            _validate_publication_record(
                record,
                expected_series_id=publication["series_id"],
                subject=f"recovered {name}",
            )
            requested = lookup["requested_publication_id"]
            if requested is not None and record["publication_id"] != requested:
                fail(
                    "materialization.store-failure",
                    f"recovered {name} publication lookup differs",
                )
    terminal = publication["terminal_head"]
    current = receipt["current"]
    if receipt["reopen_status"] == "open_failed":
        expected_terminal_status = (
            "corrupt"
            if _lookup_error_is_corruption(receipt["open_error_code"])
            else "unavailable"
        )
        if terminal != {"status": expected_terminal_status, "record": None}:
            fail("materialization.store-failure", "open-failed terminal head differs")
    elif current["status"] == "present":
        if terminal != {"status": "present", "record": current["record"]}:
            fail("materialization.store-failure", "recovered terminal head differs")
    elif current["status"] == "not_found":
        if terminal != {"status": "absent", "record": None}:
            fail("materialization.store-failure", "recovered absent head differs")
    else:
        expected_terminal_status = (
            "corrupt"
            if _lookup_error_is_corruption(current["error_code"])
            else "unavailable"
        )
        if terminal != {"status": expected_terminal_status, "record": None}:
            fail("materialization.store-failure", "recovered erroneous head differs")


def _validate_candidate_publication_identity(
    request: dict[str, Any],
    publication: dict[str, Any],
    expected_store: dict[str, Any],
) -> None:
    if publication["candidate_identity_state"] == "not_constructed":
        if publication["candidate_identity"] is not None:
            fail("materialization.store-failure", "unconstructed candidate has identity")
        return
    candidate = publication["candidate_identity"]
    candidate_record = {
        **candidate,
        "physical_generation": 1,
        "state": "committed",
        "corrupt": False,
    }
    if (
        candidate["series_id"] != request["publication"]["series_id"]
        or candidate["snapshot_id"]
        != expected_store["snapshot_manifest"]["snapshot_id"]
        or candidate["parent_publication"]
        != request["publication"]["expected_parent_publication"]
        or candidate["publication_id"] != _publication_record_identity(candidate_record)
        or (request["publication"]["genesis"] and candidate["sequence"] != 1)
    ):
        fail("materialization.store-failure", "candidate publication identity differs")


def stale_parent_report(root: pathlib.Path, request: dict[str, Any]) -> dict[str, Any]:
    if request["publication"]["backend"] != "sqlite":
        raise ValueError("stale-parent fixture requires the SQLite backend")
    report = sample_report(root, request)
    store = report["store"]
    snapshot_id = store["snapshot_manifest"]["snapshot_id"]
    candidate_record = {
        "publication_id": "pending",
        "series_id": request["publication"]["series_id"],
        "snapshot_id": snapshot_id,
        "sequence": 1,
        "physical_generation": 2,
        "parent_publication": request["publication"]["expected_parent_publication"],
        "state": "committed",
        "corrupt": False,
    }
    candidate_record["publication_id"] = canonical_identity_digest(
        "publication",
        [
            candidate_record["series_id"],
            candidate_record["snapshot_id"],
            candidate_record["sequence"],
            candidate_record["parent_publication"] or "",
        ],
    )
    external_snapshot = "snapshot:sha256:" + "e" * 64
    observed_record = {
        "publication_id": "pending",
        "series_id": request["publication"]["series_id"],
        "snapshot_id": external_snapshot,
        "sequence": 1,
        "physical_generation": 3,
        "parent_publication": None,
        "state": "committed",
        "corrupt": False,
    }
    observed_record["publication_id"] = canonical_identity_digest(
        "publication",
        [
            observed_record["series_id"],
            observed_record["snapshot_id"],
            observed_record["sequence"],
            "",
        ],
    )
    not_applicable = {
        "status": "not_applicable",
        "requested_publication_id": None,
        "record": None,
        "error_code": None,
        "error_field": None,
    }
    report["result"] = "failed"
    report["process_exit_status"] = 1
    report["publication"] = {
        "backend": "sqlite",
        "selector": copy.deepcopy(request["publication"]["selector"]),
        "series_id": request["publication"]["series_id"],
        "genesis": request["publication"]["genesis"],
        "expected_parent_publication": request["publication"][
            "expected_parent_publication"
        ],
        "observed_parent_publication": observed_record["publication_id"],
        "observed_parent_record": copy.deepcopy(observed_record),
        "head_observation": "present",
        "publication_attempted": True,
        "outcome": "rejected_stale",
        "partial_policy": "forbid",
        "candidate_snapshot_id": snapshot_id,
        "candidate_identity_state": "constructed",
        "candidate_identity": _publication_identity_projection(candidate_record),
        "invocation_commit_state": "not_committed",
        "committed_transaction_count": 0,
        "invocation_committed_record": None,
        "terminal_head": {
            "status": "present",
            "record": copy.deepcopy(observed_record),
        },
        "candidate_visibility": "absent",
        "prior_history_retained": True,
        "head_effect": "unchanged",
        "store_failure": {
            "category": "stale_parent",
            "cause": {
                "kind": "sdk_error",
                "operation": "writer_publish",
                "access_path": None,
                "code": "store.publication-conflict",
                "field": request["publication"]["series_id"],
                "detail": {"kind": "stable", "value": ""},
            },
            "diagnostic_digest": content_digest(b"fixture stale parent"),
        },
        "sqlite_effect_root_receipt": expected_sqlite_effect_root_receipt(request),
        "sqlite_reopen_status": "opened",
        "recovery_receipt": {
            "selector": copy.deepcopy(store["selector"]),
            "reopen_status": "opened",
            "open_error_code": None,
            "open_error_field": None,
            "current": {
                "status": "present",
                "requested_publication_id": None,
                "record": copy.deepcopy(observed_record),
                "error_code": None,
                "error_field": None,
            },
            "expected_parent": not_applicable,
            "candidate": {
                "status": "not_found",
                "requested_publication_id": candidate_record["publication_id"],
                "record": None,
                "error_code": "store.publication-not-found",
                "error_field": None,
            },
        },
    }
    report["semantic_verification"] = {
        "status": "not_published",
        "reopened_store": None,
        "reopen_attempt": None,
        "failure": None,
    }
    report["error"] = {
        "code": "materialization.stale-parent",
        "phase": "publication",
        "subject": request["publication"]["series_id"],
        "diagnostic": "expected parent differs",
    }
    return report


def compact_failure_report(
    request_bytes: bytes,
    *,
    request: dict[str, Any] | None,
    phase: str,
    code: str,
    subject: str = "materialization-request",
    store_failure_cause: dict[str, Any] | None = None,
    head_observation: str | None = None,
    observed_head_publication: str | None = None,
) -> dict[str, Any]:
    request_bound = request is not None
    launch_attempt_count = int(
        phase
        in {
            "worker-launch",
            "transcript",
            "materialization-validation",
            "store-open",
            "store-stage",
            "report-construction",
        }
    )
    launch_success_count = int(
        phase
        in {
            "transcript",
            "materialization-validation",
            "store-open",
            "store-stage",
            "report-construction",
        }
    )
    if head_observation is None:
        if (
            store_failure_cause is not None
            and store_failure_cause.get("operation") == "head_current"
        ):
            head_observation = (
                "absent"
                if store_failure_cause.get("code") == "store.current-not-found"
                else "sdk-error"
            )
        elif phase in {"store-stage", "report-construction"}:
            head_observation = "absent"
        else:
            head_observation = "not-observed"
    return {
        "schema": "cxxlens.clang22-materialization-report.v2",
        "report_version": MATERIALIZATION_VERSION,
        "response_kind": "compact_failure",
        "result": "failed",
        "generated_at": datetime.datetime(
            2026, 7, 19, tzinfo=datetime.timezone.utc
        ).isoformat().replace("+00:00", "Z"),
        "process_exit_status": 1,
        "raw_input_observation": raw_input_observation(request_bytes),
        "binding": {
            "state": "request-bound" if request_bound else "raw-input-only",
            "request": _request_binding(request) if request_bound else None,
        },
        "effects": {
            "worker_launch_attempt_count": launch_attempt_count,
            "worker_launch_success_count": launch_success_count,
            "store_draft_state": (
                "discarded"
                if phase in {"store-stage", "report-construction"}
                else "not-created"
            ),
            "head_observation": head_observation,
            "observed_head_publication": observed_head_publication,
            "publication_attempted": False,
            "committed_transaction_count": 0,
            "prior_history_retained": True,
            "store_failure_cause": copy.deepcopy(store_failure_cause),
        },
        "error": {
            "code": code,
            "phase": phase,
            "subject": subject,
            "diagnostic": "fixture failure",
        },
    }


def _exact_task_result(
    root: pathlib.Path,
    request: dict[str, Any],
    task: dict[str, Any],
    result: dict[str, Any],
    raw_stdout: bytes,
    report_provider_identity: dict[str, Any],
) -> None:
    for field in (
        "provider_task_id",
        "provider_execution_id",
        "selected_catalog_compile_unit_id",
        "compile_unit_id",
        "task_input_digest",
    ):
        if result[field] != task[field]:
            fail("materialization.task-binding-mismatch", f"task report differs at {field}")
    if result["terminal"] != "provider.success":
        fail("materialization.worker-failure", "passed report has non-success terminal")
    if result["input_transfer"] != expected_input_transfer_receipt(request, task):
        fail(
            "materialization.task-binding-mismatch",
            "authenticated task-input transfer receipt differs",
        )
    runtime_observation = derive_runtime_observation(
        root,
        request,
        task,
        raw_stdout,
    )
    expected_runtime_receipt = materialization_runtime_receipt(
        runtime_observation["receipt"]
    )
    validate_runtime_provider_identity_cross_binding(
        runtime_observation["validated_provider_identity"],
        expected_runtime_provider_identity(root, request),
        report_provider_identity,
    )
    if result["runtime_receipt"] != expected_runtime_receipt:
        fail(
            "materialization.transcript-invalid",
            "runtime receipt differs from independent raw wire/frame/seal authority",
        )
    validate_runtime_seal_cross_binding(
        runtime_observation["sealed_transcript"],
        result,
    )
    expected_coverage = expected_task_coverage(task)
    if result["coverage"] != expected_coverage:
        fail(
            "materialization.coverage-incomplete",
            "transport or semantic coverage record plane differs",
        )
    components = result["side_channel_components"]
    context = task_semantic_context(task)
    expected_unresolved_digest = semantic_digest(
        "cxxlens.clang22-task-unresolved.v1",
        _canonical_projection_value(
            {
                "originating_task": context,
                "records": fixture_task_unresolved_records(result),
            }
        ),
    )
    expected_evidence_digest = semantic_digest(
        "cxxlens.clang22-task-evidence.v1",
        _canonical_projection_value(
            {
                "originating_task": context,
                "records": fixture_task_evidence_records(result),
            }
        ),
    )
    if (
        components["transport_coverage_set_digest"]
        != expected_coverage["transport_record_set_digest"]
        or components["semantic_coverage_set_digest"]
        != expected_coverage["semantic_record_set_digest"]
        or components["unresolved_set_digest"] != expected_unresolved_digest
        or components["evidence_set_digest"] != expected_evidence_digest
        or components["guarantee_profile_id"] != GUARANTEE_PROFILE_ID
        or components["guarantee_profile_digest"]
        != expected_guarantee_profile_digest()
    ):
        fail(
            "materialization.coverage-incomplete",
            "task coverage/unresolved/evidence/profile side-channel binding differs",
        )
    groups = {row["dependency_group_id"]: row for row in result["groups"]}
    if set(groups) != set(GROUP_DESCRIPTORS):
        fail("materialization.group-incomplete", "task report group set differs")
    for group, descriptors in GROUP_DESCRIPTORS.items():
        if (
            groups[group]["descriptor_ids"] != descriptors
            or groups[group]["atomic_output_group_id"] != "clang22-atomic"
            or not groups[group]["sealed"]
        ):
            fail("materialization.group-incomplete", f"group {group} is incomplete")
    bindings = {row["descriptor_id"]: row for row in request["registry"]["descriptors"]}
    batches = {row["descriptor_id"]: row for row in result["batches"]}
    if set(batches) != set(DESCRIPTOR_IDS):
        fail("materialization.group-incomplete", "task batch set differs")
    for descriptor in DESCRIPTOR_IDS:
        batch = batches[descriptor]
        binding = bindings[descriptor]
        expected = {
            "batch_id": binding["batch_id"],
            "runtime_descriptor_digest": binding["runtime_descriptor_digest"],
            "dependency_group_id": binding["dependency_group_id"],
            "atomic_output_group_id": "clang22-atomic",
        }
        if any(batch[key] != value for key, value in expected.items()) or not batch["sealed"]:
            fail("materialization.group-incomplete", f"batch {descriptor} binding differs")
        row_bindings = batch["row_bindings"]
        if (
            len(row_bindings) != batch["row_count"]
            or len(batch["provenance_edge_digests"]) != batch["row_count"]
            or bool(batch["ordered_chunk_digests"])
            != (batch["row_count"] > 0)
            or row_bindings
            != sorted(
                row_bindings,
                key=lambda row: (
                    row["row_digest"],
                    task_semantic_context_key(row["originating_task"]),
                ),
            )
            or batch["provenance_edge_digests"]
            != sorted(batch["provenance_edge_digests"])
        ):
            fail(
                "materialization.claim-invalid",
                f"batch {descriptor} row/provenance leaf census differs",
            )
        expected_context = task_semantic_context(task)
        for binding in row_bindings:
            if (
                binding["originating_task"] != expected_context
                or binding["final_relation_compile_unit_id"]
                != task["compile_unit_id"]
            ):
                fail(
                    "materialization.task-binding-mismatch",
                    f"batch {descriptor} row is attributed to another task",
                )
            if descriptor in DESCRIPTOR_IDS[:3]:
                if (
                    binding["primary_span_bundle_digest"] is not None
                    or binding["exact_equivalence"] is not None
                    or binding["limitation_digest"] is not None
                ):
                    fail(
                        "materialization.claim-invalid",
                        f"canonical batch {descriptor} retained observation-only leaves",
                    )
            else:
                exact = binding["exact_equivalence"]
                if not isinstance(exact, bool) or (
                    exact == (binding["limitation_digest"] is not None)
                ):
                    fail(
                        "materialization.claim-invalid",
                        f"observation batch {descriptor} exactness leaf differs",
                    )
                if (
                    descriptor == "frontend.clang22.type_observation.v2"
                    and binding["primary_span_bundle_digest"] is not None
                ):
                    fail(
                        "materialization.span-invalid",
                        "type observation retained a primary span bundle",
                    )
        if descriptor in DESCRIPTOR_IDS[3:]:
            validate_observation_equivalence_census(
                descriptor,
                batch["observation_equivalence_census"],
                row_bindings,
            )
        expected_batch = copy.deepcopy(batch)
        bind_batch_digests(task, expected_batch)
        for digest_field in (
            "ordered_chunk_set_digest",
            "claim_content_set_digest",
            "provenance_edge_set_digest",
            "batch_digest",
        ):
            if batch[digest_field] != expected_batch[digest_field]:
                fail(
                    "materialization.claim-invalid",
                    f"batch {descriptor} {digest_field} differs",
                )
    for group, descriptors in GROUP_DESCRIPTORS.items():
        if groups[group]["batch_set_digest"] != expected_group_batch_set_digest(
            task,
            groups[group],
            result["batches"],
        ):
            fail(
                "materialization.group-incomplete",
                f"group {group} batch-set digest differs",
            )
    if components["guarantee_fragment_digest"] != (
        expected_task_guarantee_fragment_digest(result)
    ):
        fail(
            "materialization.claim-invalid",
            "task guarantee fragment differs from sealed semantic evidence",
        )
    if result["side_channel_digest"] != expected_task_side_channel_digest(result):
        fail("materialization.report-invalid", "task side-channel digest differs")
    if result["task_result_digest"] != expected_task_result_digest(result):
        fail("materialization.report-invalid", "task result digest differs")


COMPACT_RAW_PHASE_CODES = {
    "input-limit": {"materialization.request-invalid"},
    "json-decode": {
        "materialization.request-invalid",
        "materialization.spool-failure",
    },
    "request-envelope": {"materialization.request-invalid"},
    "request-schema": {
        "materialization.request-invalid",
        "materialization.spool-failure",
    },
    "request-version": {"materialization.version-unsupported"},
    "request-binding": {
        "materialization.identity-mismatch",
        "materialization.catalog-census-mismatch",
        "materialization.task-binding-mismatch",
        "materialization.descriptor-binding-mismatch",
        "materialization.spool-failure",
    },
}
COMPACT_BOUND_PHASE_CODES = {
    "installation-binding": {"materialization.identity-mismatch"},
    "worker-launch": {"materialization.worker-failure"},
    "transcript": {
        "materialization.transcript-invalid",
        "materialization.group-incomplete",
        "materialization.worker-failure",
    },
    "materialization-validation": {
        "materialization.span-invalid",
        "materialization.claim-invalid",
        "materialization.coverage-incomplete",
    },
    "store-open": {"materialization.store-failure"},
    "store-stage": {
        "materialization.store-failure",
        "materialization.claim-invalid",
        "materialization.coverage-incomplete",
    },
    "report-construction": {"materialization.report-invalid"},
}

PREPUBLICATION_STORE_OPERATIONS = {
    "store-open": {"store_open"},
    "store-stage": {
        "head_current",
        "writer_begin",
        "partition_stage",
        "closure_stage",
        "writer_validate",
    },
}


def validate_compact_store_failure_cause(
    error: dict[str, Any],
    effects: dict[str, Any],
    store_failure_authority: dict[str, Any] | None,
) -> None:
    """Retain only an authentic first prepublication Store SDK failure."""

    cause = effects["store_failure_cause"]
    store_failure = (
        error["code"] == "materialization.store-failure"
        and error["phase"] in PREPUBLICATION_STORE_OPERATIONS
    )
    if not store_failure:
        if cause is not None or store_failure_authority is not None:
            fail(
                "materialization.report-invalid",
                "non-Store compact failure retained a Store failure cause",
            )
        return
    head_current_failure = cause is not None and cause.get("operation") == "head_current"
    expected_access_path = "current-selector" if head_current_failure else None
    if (
        cause is None
        or store_failure_authority is None
        or cause != store_failure_authority
        or cause["kind"] != "sdk_error"
        or cause["operation"]
        not in PREPUBLICATION_STORE_OPERATIONS[error["phase"]]
        or cause["access_path"] != expected_access_path
    ):
        fail(
            "materialization.store-failure",
            "compact Store failure cause phase/operation/access-path differs",
        )
    if head_current_failure:
        expected_head_observation = (
            "absent"
            if cause["code"] == "store.current-not-found"
            else "sdk-error"
        )
        if (
            effects["head_observation"] != expected_head_observation
            or effects["observed_head_publication"] is not None
        ):
            fail(
                "materialization.store-failure",
                "compact head_current cause does not match its typed head observation",
            )
    elif effects["head_observation"] == "sdk-error":
        fail(
            "materialization.store-failure",
            "compact sdk-error head observation lacks an exact head_current cause",
        )
    _validate_store_detail_observation(cause["detail"])


def validate_raw_input_observation(
    observation: dict[str, Any],
    request_bytes: bytes,
) -> None:
    expected = raw_input_observation(request_bytes)
    if observation != expected:
        fail(
            "materialization.report-invalid",
            "raw input observation differs from exact transport bytes",
        )


def _request_binding(request: dict[str, Any]) -> dict[str, str]:
    return {
        "materialization_request_id": request["materialization_request_id"],
        "request_digest": request["request_digest"],
        "semantic_request_digest": request["semantic_request_digest"],
    }


def validate_report(
    root: pathlib.Path,
    request: dict[str, Any] | None,
    report: dict[str, Any],
    *,
    request_bytes: bytes,
    runtime_raw_occurrences: dict[tuple[str, str, str], bytes] | None = None,
    store_failure_authority: dict[str, Any] | None = None,
    postpublish_failure_authority: dict[str, Any] | None = None,
) -> None:
    validate_schema(
        report,
        load(root / REPORT_SCHEMA),
        "materialization report",
        error_code="materialization.report-invalid",
    )
    validate_raw_input_observation(report["raw_input_observation"], request_bytes)
    if report["response_kind"] == "compact_failure":
        binding = report["binding"]
        effects = report["effects"]
        error = report["error"]
        validate_compact_store_failure_cause(
            error,
            effects,
            store_failure_authority,
        )
        expected_launch_attempt_count = int(
            error["phase"]
            in {
                "worker-launch",
                "transcript",
                "materialization-validation",
                "store-open",
                "store-stage",
                "report-construction",
            }
        )
        expected_launch_success_count = int(
            error["phase"]
            in {
                "transcript",
                "materialization-validation",
                "store-open",
                "store-stage",
                "report-construction",
            }
        )
        expected_draft_state = (
            "discarded"
            if error["phase"] in {"store-stage", "report-construction"}
            else "not-created"
        )
        if binding["state"] == "raw-input-only":
            allowed_codes = COMPACT_RAW_PHASE_CODES.get(error["phase"])
            if (
                request is not None
                or binding["request"] is not None
                or allowed_codes is None
                or error["code"] not in allowed_codes
                or effects["worker_launch_attempt_count"] != 0
                or effects["worker_launch_success_count"] != 0
                or effects["store_draft_state"] != "not-created"
                or effects["head_observation"] != "not-observed"
            ):
                fail(
                    "materialization.report-invalid",
                    "raw-input compact failure phase, binding, or effects differ",
                )
        else:
            if request is None:
                fail(
                    "materialization.report-invalid",
                    "request-bound compact failure lacks a validated request",
                )
            validate_request(root, request)
            allowed_codes = COMPACT_BOUND_PHASE_CODES.get(error["phase"])
            if (
                binding["request"] != _request_binding(request)
                or allowed_codes is None
                or error["code"] not in allowed_codes
            ):
                fail(
                    "materialization.report-invalid",
                    "request-bound compact failure phase/code/binding differs",
                )
            if error["phase"] in {
                "installation-binding",
                "worker-launch",
                "transcript",
                "materialization-validation",
                "store-open",
            } and effects["head_observation"] != "not-observed":
                fail(
                    "materialization.report-invalid",
                    "pre-Store compact failure claims a head observation",
                )
            head_current_failure = (
                effects["store_failure_cause"] is not None
                and effects["store_failure_cause"]["operation"] == "head_current"
            )
            if (
                error["phase"] in {"store-stage", "report-construction"}
                and not head_current_failure
                and effects["head_observation"] not in {"absent", "present"}
            ):
                fail(
                    "materialization.report-invalid",
                    "post-head compact failure lacks an absent or present head observation",
                )
            if error["phase"] in {"installation-binding", "worker-launch"} and effects[
                "store_draft_state"
            ] != "not-created":
                fail(
                    "materialization.report-invalid",
                    "pre-Store compact failure claims a Store draft",
                )
        if (
            effects["worker_launch_attempt_count"] != expected_launch_attempt_count
            or effects["worker_launch_success_count"] != expected_launch_success_count
            or effects["store_draft_state"] != expected_draft_state
            or effects["publication_attempted"]
            or effects["committed_transaction_count"] != 0
            or not effects["prior_history_retained"]
        ):
            fail(
                "materialization.report-invalid",
                "compact failure is not an exact zero-publication effect ledger",
            )
        return
    if request is None:
        fail("materialization.report-invalid", "detailed report lacks request bytes/binding")
    validate_request(root, request)
    if report["request"] != _request_binding(request):
        fail("materialization.report-invalid", "report request binding differs")
    if report["source"] != {
        "revision": request["tool"]["source_revision"],
        "tree": request["tool"]["source_tree"],
    }:
        fail("materialization.identity-mismatch", "report source revision/tree differs")
    if report["authority_digests"] != authority_bindings(root):
        fail("materialization.report-invalid", "materialization authority digests differ")
    if report["installation"]["requested"] != {
        "occurrence_manifest_digest": request["tool"]["occurrence_manifest_digest"]
    }:
        fail(
            "materialization.identity-mismatch",
            "requested installed occurrence binding differs",
        )
    validate_measured_occurrence(root, request, report["installation"]["measured"])
    expected_provider = {
        "tool_executable": request["tool"]["executable"],
        "tool_interface_version": request["tool"]["interface_version"],
        "worker_executable": request["worker"]["executable"],
        "provider_id": request["worker"]["provider_id"],
        "provider_version": request["worker"]["provider_version"],
        "semantic_contract_digest": request["worker"]["semantic_contract_digest"],
        "protocol_major": request["worker"]["protocol_major"],
        "protocol_minor": request["worker"]["protocol_minor"],
        "required_features": request["worker"]["required_features"],
        "sandbox_policy_digest": request["worker"]["sandbox_policy_digest"],
    }
    if report["provider"] != expected_provider:
        fail("materialization.identity-mismatch", "provider identity binding differs")
    expected_project = {
        key: request["project"][key]
        for key in (
            "project_id",
            "catalog_id",
            "catalog_digest",
            "logical_root",
            "catalog_environment_digest",
            "catalog_compile_unit_census_digest",
            "catalog_compile_units",
        )
    }
    if report["project"] != expected_project:
        fail("materialization.catalog-census-mismatch", "report project census differs")
    if report["registry"] != {
        "authority_registry_digest": request["registry"][
            "authority_registry_digest"
        ],
        "base_descriptors": request["registry"]["base_descriptors"],
        "descriptors": request["registry"]["descriptors"],
    }:
        fail("materialization.descriptor-binding-mismatch", "report registry binding differs")
    if (
        report["engine"] != request["engine"]
        or report["interpretation_policy"] != request["interpretation_policy"]
        or report["trust_policy"] != request["trust_policy"]
    ):
        fail(
            "materialization.identity-mismatch",
            "report engine or named policy binding differs",
        )
    publication = report["publication"]
    request_publication = request["publication"]
    if (
        publication["backend"] != request_publication["backend"]
        or publication["selector"] != request_publication["selector"]
        or publication["series_id"] != request_publication["series_id"]
        or publication["genesis"] != request_publication["genesis"]
        or publication["expected_parent_publication"]
        != request_publication["expected_parent_publication"]
    ):
        fail("materialization.stale-parent", "report publication request binding differs")
    effect_receipt = publication["sqlite_effect_root_receipt"]
    if publication["backend"] == "memory":
        if effect_receipt is not None:
            fail(
                "materialization.store-failure",
                "memory publication retained a SQLite effect-root receipt",
            )
    elif (
        effect_receipt is None
        or effect_receipt["contract"] != "rooted-vfs-v1"
        or effect_receipt["relative_path"] != request_publication["sqlite_path"]
        or effect_receipt["parent_resolution"]
        != "openat2-beneath-no-symlinks-no-magiclinks"
        or effect_receipt["leaf_resolution"]
        != "database-and-sidecars-rooted-no-follow"
    ):
        fail(
            "materialization.store-failure",
            "SQLite effect-root receipt differs from the authenticated request",
        )
    spans = report["span_validation"]
    if (
        spans["absent_bundle_count"]
        != spans["entity_absent_bundle_count"] + spans["call_absent_bundle_count"]
        or spans["absent_bundle_count"] != spans["absent_bundle_unresolved_count"]
        or spans["absent_bundle_count"]
        < spans["source_dependent_canonical_omission_count"]
        or spans["call_absent_bundle_count"]
        != spans["source_dependent_canonical_omission_count"]
        or spans["unique_bundle_count"] > spans["observed_bundle_count"]
        or spans["constructed_source_span_claim_count"] > spans["unique_bundle_count"]
    ):
        fail("materialization.span-invalid", "absent span accounting is not exact")
    unresolved = report["side_channels"]["unresolved"]
    category_counts = unresolved["category_counts"]
    if (
        unresolved["categories"] != sorted(category_counts)
        or sum(category_counts.values()) != unresolved["record_count"]
        or unresolved["blocking_count"] > unresolved["record_count"]
        or category_counts.get(PRIMARY_SPAN_ABSENCE_CATEGORY, 0)
        != spans["absent_bundle_unresolved_count"]
    ):
        fail(
            "materialization.coverage-incomplete",
            "typed unresolved category accounting is not exact",
        )
    if report["adoption"] != {
        **report["adoption"],
        "boundary": "sealed-materialization-result",
        "visibility": "tool-private-immutable",
        "state": "sealed",
        "partial_policy": "forbid",
        "all_tasks_mandatory": True,
        "all_groups_mandatory": True,
        "all_batches_mandatory": True,
    }:
        fail("materialization.claim-invalid", "passed report adoption boundary differs")
    if report["adoption"]["raw_frames"][
        "authority"
    ] != "diagnostic-only-non-authoritative" or report["adoption"]["raw_frames"]["retained"]:
        fail("materialization.claim-invalid", "raw frames became adoption authority")
    task_rows = request["tasks"]
    result_rows = report["task_results"]
    tasks = {task_execution_key(row): row for row in task_rows}
    results = {task_execution_key(row): row for row in result_rows}
    if (
        len(tasks) != len(task_rows)
        or len(results) != len(result_rows)
        or set(results) != set(tasks)
    ):
        fail("materialization.group-incomplete", "not every requested task has one result")
    runtime_occurrences = (
        fixture_runtime_raw_occurrences(root, request)
        if runtime_raw_occurrences is None
        else runtime_raw_occurrences
    )
    if set(runtime_occurrences) != set(tasks):
        fail(
            "materialization.transcript-invalid",
            "runtime-private raw occurrence task census differs",
        )
    for execution_key, task in tasks.items():
        _exact_task_result(
            root,
            request,
            task,
            results[execution_key],
            runtime_occurrences[execution_key],
            report_runtime_provider_identity(report),
        )
    if report["adoption"]["task_result_set_digest"] != expected_task_result_set_digest(
        results.values()
    ):
        fail("materialization.report-invalid", "task-result set digest differs")
    if report["adoption"]["raw_frames"][
        "frame_set_digest"
    ] != expected_raw_frame_set_digest(results.values()):
        fail("materialization.transcript-invalid", "raw-frame set digest differs")
    observation_rows = {
        (
            task_semantic_context_key(binding["originating_task"]),
            batch["descriptor_id"],
            binding["row_digest"],
        ): binding["primary_span_bundle_digest"]
        for result in results.values()
        for batch in result["batches"]
        if batch["descriptor_id"] in SPAN_OBSERVATION_DESCRIPTORS
        for binding in batch["row_bindings"]
        if binding["primary_span_bundle_digest"] is not None
    }
    span_rows, span_bindings = validated_span_rows(
        request,
        spans["validated_bundle_bindings"],
        observation_rows,
    )
    expected_span_digests = span_report_digests(span_bindings, span_rows)
    if any(spans[key] != value for key, value in expected_span_digests.items()):
        fail(
            "materialization.span-invalid",
            "span bundle/task/row digest binding differs",
        )
    if (
        spans["unique_bundle_count"] != len(span_rows)
        or spans["constructed_source_span_claim_count"] != len(span_rows)
        or spans["observed_bundle_count"] != len(span_bindings)
    ):
        fail(
            "materialization.span-invalid",
            "validated span row census differs from bundle bindings",
        )
    if report["adoption"]["raw_frames"]["frame_count"] != sum(
        result["runtime_receipt"]["frame_count"] for result in results.values()
    ):
        fail("materialization.transcript-invalid", "raw-frame/task transcript count differs")
    raw_occurrences = [
        (
            result["runtime_receipt"]["raw_frame_stream_bytes"],
            result["runtime_receipt"]["raw_frame_stream_digest"],
        )
        for result in results.values()
    ]
    if len(raw_occurrences) != len(set(raw_occurrences)):
        fail(
            "materialization.transcript-invalid",
            "distinct task executions reused one raw-frame occurrence receipt",
        )
    call_observation_rows = sum(
        next(
            batch["row_count"]
            for batch in result["batches"]
            if batch["descriptor_id"] == "frontend.clang22.call_observation.v2"
        )
        for result in results.values()
    )
    entity_observation_rows = sum(
        next(
            batch["row_count"]
            for batch in result["batches"]
            if batch["descriptor_id"] == "frontend.clang22.entity_observation.v2"
        )
        for result in results.values()
    )
    call_site_rows = sum(
        next(
            batch["row_count"]
            for batch in result["batches"]
            if batch["descriptor_id"] == "cc.call_site.v1"
        )
        for result in results.values()
    )
    if call_observation_rows != call_site_rows + spans[
        "source_dependent_canonical_omission_count"
    ]:
        fail("materialization.span-invalid", "source-dependent call omission differs")
    if (
        entity_observation_rows + call_observation_rows
        != spans["observed_bundle_count"] + spans["absent_bundle_count"]
        or spans["entity_absent_bundle_count"] > entity_observation_rows
        or spans["call_absent_bundle_count"] > call_observation_rows
    ):
        fail("materialization.span-invalid", "observation/span bundle census differs")
    if (
        spans["unique_bundle_count"] != spans["constructed_source_span_claim_count"]
        or spans["recomputed_id_mismatch_count"] != 0
        or spans["invalid_range_count"] != 0
        or spans["task_binding_mismatch_count"] != 0
        or not spans["hard_references_resolved"]
    ):
        fail("materialization.span-invalid", "span validation did not close exactly")
    base = report["base_claims"]
    expected_base = base_claim_report(
        root,
        request,
        report["side_channels"]["guarantee"]["digest"],
        span_bindings,
    )
    if base != expected_base:
        fail("materialization.claim-invalid", "base-claim census/binding differs")
    transport_coverage = report["side_channels"]["transport_coverage"]
    coverage = report["side_channels"]["coverage"]
    if transport_coverage != expected_coverage_summary(
        results.values(), "transport"
    ) or coverage != expected_coverage_summary(results.values(), "semantic"):
        fail(
            "materialization.coverage-incomplete",
            "global transport/semantic coverage summaries differ from actual records",
        )
    evidence = report["side_channels"]["evidence"]
    if (
        evidence["kinds"] != sorted(evidence["kind_counts"])
        or sum(evidence["kind_counts"].values()) != evidence["record_count"]
    ):
        fail("materialization.claim-invalid", "evidence kind accounting differs")
    for channel in (
        "transport_coverage",
        "coverage",
        "unresolved",
        "evidence",
    ):
        if report["side_channels"][channel][
            "digest"
        ] != expected_global_side_channel_digest(
            channel,
            report["side_channels"][channel],
            results.values(),
        ):
            fail(
                "materialization.report-invalid",
                f"global {channel} digest differs",
            )
    guarantee = report["side_channels"]["guarantee"]
    if unresolved["blocking_count"] != 0:
        fail("materialization.coverage-incomplete", "passed report has blocking unresolved")
    expected_descriptor_censuses = observation_descriptor_censuses(results.values())
    if (
        guarantee["profile_id"] != GUARANTEE_PROFILE_ID
        or guarantee["profile_digest"] != expected_guarantee_profile_digest()
        or guarantee["assumptions"] != GUARANTEE_ASSUMPTIONS
        or guarantee["verification_modalities"] != GUARANTEE_MODALITIES
    ):
        fail(
            "materialization.claim-invalid",
            "closed prepublication guarantee profile differs",
        )
    if guarantee["observation_descriptor_censuses"] != expected_descriptor_censuses:
        fail(
            "materialization.claim-invalid",
            "guarantee observation equivalence census differs",
        )
    if guarantee["digest"] != expected_guarantee_digest(
        guarantee,
        report["side_channels"],
        results.values(),
    ):
        fail("materialization.report-invalid", "guarantee digest differs")
    if guarantee["approximation"] == "exact" and (
        unresolved["record_count"] != 0
        or spans["absent_bundle_count"] != 0
        or coverage["state_counts"]["covered"] != coverage["record_count"]
        or transport_coverage["state_counts"]["covered"]
        != transport_coverage["record_count"]
        or any(
            census["non_exact_equivalence_count"] != 0
            for census in expected_descriptor_censuses
        )
    ):
        fail("materialization.coverage-incomplete", "exact guarantee preconditions differ")
    stages = {row["descriptor_id"]: row for row in report["claim_stages"]}
    if set(stages) != set(DESCRIPTOR_IDS):
        fail("materialization.claim-invalid", "claim-stage descriptor set differs")
    for descriptor, expected_stage in DESCRIPTOR_STAGE.items():
        expected_claim = expected_claim_stage(
            results.values(),
            descriptor,
            guarantee["digest"],
            report["store"],
        )
        if (
            stages[descriptor] != expected_claim
            or stages[descriptor]["stage"] != expected_stage
        ):
            fail("materialization.claim-invalid", f"claim stage differs for {descriptor}")
        batch_claim_count = sum(
            next(
                batch["row_count"]
                for batch in result["batches"]
                if batch["descriptor_id"] == descriptor
            )
            for result in results.values()
        )
        if stages[descriptor]["origin_association_count"] != batch_claim_count:
            fail("materialization.claim-invalid", f"claim/batch census differs for {descriptor}")
    canonical_count = sum(
        stages[descriptor]["origin_association_count"]
        for descriptor in DESCRIPTOR_IDS[:3]
    )
    provenance = report["provenance"]
    if (
        provenance["canonical_claim_count"] != canonical_count
        or provenance["canonical_claims_with_exact_input_edges"] != canonical_count
        or provenance["edge_count"] < canonical_count
        or provenance["orphan_count"] != 0
        or provenance["ambiguous_count"] != 0
        or provenance["edge_set_digest"]
        != expected_global_provenance_digest(provenance, report["claim_stages"])
    ):
        fail("materialization.claim-invalid", "canonical provenance edge accounting differs")
    expected_store = expected_store_binding(root, request, report)
    if report["store"] != expected_store:
        fail(
            "materialization.store-failure",
            "claim occurrence, partition, or snapshot identity DAG differs",
        )
    snapshot_id = expected_store["snapshot_manifest"]["snapshot_id"]
    if publication["candidate_snapshot_id"] != snapshot_id:
        fail("materialization.store-failure", "candidate snapshot identity differs")
    if publication["series_id"] != expected_series_id(publication["selector"]):
        fail("materialization.store-failure", "publication series identity differs")
    outcome = publication["outcome"]
    if outcome == "committed_verified":
        expected = {"store": expected_store}
        bind_committed_verified_publication(request, expected)
        if (
            report["result"] != "passed"
            or report["error"] is not None
            or publication != expected["publication"]
            or report["semantic_verification"]["status"] != "passed"
            or report["semantic_verification"]["reopen_attempt"] is not None
            or report["semantic_verification"]["failure"] is not None
        ):
            fail(
                "materialization.store-failure",
                "committed publication or three-path reopened Store differs",
            )
        validate_committed_verified_reopened_store(request, report)
        return
    if report["result"] != "failed":
        fail("materialization.store-failure", "non-committed outcome reports success")
    validate_failed_publication_outcome(
        request,
        report,
        expected_store,
        postpublish_failure_authority=postpublish_failure_authority,
    )


def _validate_reopened_handle_projection(
    projection: dict[str, Any],
    *,
    expected_series_id: str,
) -> None:
    _validate_publication_record(
        projection["publication_record"],
        expected_series_id=expected_series_id,
        subject="reopened handle",
    )
    if projection["snapshot_manifest_digest"] != content_digest(
        canonical_json(projection["snapshot_manifest"])
    ):
        fail(
            "materialization.store-failure",
            "reopened snapshot manifest digest differs",
        )
    semantic_fields = {
        key: projection[key]
        for key in (
            "backend",
            "snapshot_manifest",
            "snapshot_manifest_digest",
            "descriptors",
            "partition_binding_multiset_digest",
            "row_multiset_digest",
            "claim_annotation_multiset_digest",
            "coverage_multiset_digest",
            "unresolved_digest",
            "closure_digest",
            "cursor_projection_digest",
            "canonical_export_digest",
        )
    }
    if projection["semantic_projection_digest"] != _digest_projection(
        "cxxlens.clang22-reopened-semantic-projection.v1", semantic_fields
    ):
        fail(
            "materialization.store-failure",
            "reopened semantic projection digest differs",
        )
    if projection["handle_projection_digest"] != _digest_projection(
        "cxxlens.clang22-reopened-handle-projection.v1",
        {
            key: value
            for key, value in projection.items()
            if key != "handle_projection_digest"
        },
    ):
        fail(
            "materialization.store-failure",
            "reopened handle projection digest differs",
        )


def _validate_committed_unverified_attempt(
    request: dict[str, Any],
    report: dict[str, Any],
    *,
    postpublish_failure_authority: dict[str, Any] | None,
) -> None:
    publication = report["publication"]
    verification = report["semantic_verification"]
    attempt = verification["reopen_attempt"]
    failure_record = verification["failure"]
    if (
        attempt["backend"] != publication["backend"]
        or publication["store_failure"]["cause"] != failure_record["cause"]
        or publication["store_failure"]["diagnostic_digest"]
        != failure_record["diagnostic_digest"]
    ):
        fail(
            "materialization.store-failure",
            "committed verification failure binding differs",
        )
    record = publication["invocation_committed_record"]
    expected_lookups = (
        {"selector": _expected_recovery_selector(request)},
        {"publication_id": record["publication_id"]},
        {"snapshot_id": record["snapshot_id"]},
    )
    first_non_present: dict[str, Any] | None = None
    for receipt, expected_lookup in zip(attempt["handle_receipts"], expected_lookups):
        if receipt["lookup"] != expected_lookup:
            fail(
                "materialization.store-failure",
                "committed verification lookup input differs",
            )
        if receipt["status"] == "present":
            projection = receipt["projection"]
            _validate_reopened_handle_projection(
                projection,
                expected_series_id=(
                    projection["publication_record"]["series_id"]
                    if receipt["access_path"] == "open-snapshot"
                    else publication["series_id"]
                ),
            )
        elif receipt["status"] != "not_attempted" and first_non_present is None:
            first_non_present = receipt
    classify_postpublish_failure(
        attempt,
        failure_record,
        first_non_present,
        first_cause_authority=postpublish_failure_authority,
    )
    _validate_postpublish_projection_mismatch_binding(
        request,
        report,
        failure_record,
    )


POSTPUBLISH_ACCESS_PATHS = (
    "current-selector",
    "open-publication",
    "open-snapshot",
)
POSTPUBLISH_PATH_OPERATIONS = {
    "current-selector": "verify_current",
    "open-publication": "verify_open_publication",
    "open-snapshot": "verify_open_snapshot",
}
POSTPUBLISH_MISMATCH_BINDINGS = {
    "publication-binding": {
        "current-selector": {
            "publication-semantic-fields",
            "physical-generation-transition",
        },
        "open-publication": {
            "publication-semantic-fields",
            "physical-generation-transition",
        },
    },
    "snapshot-binding": {
        "current-selector": {"snapshot-manifest"},
        "open-publication": {"snapshot-manifest"},
        "open-snapshot": {
            "snapshot-manifest",
            "open-snapshot-return-binding",
        },
    },
    "descriptor-inventory": {
        path: {"descriptor-inventory"} for path in POSTPUBLISH_ACCESS_PATHS
    },
    "partition-bindings": {
        path: {"partition-bindings"} for path in POSTPUBLISH_ACCESS_PATHS
    },
    "rows": {path: {"rows"} for path in POSTPUBLISH_ACCESS_PATHS},
    "claim-annotations": {
        path: {"claim-annotations"} for path in POSTPUBLISH_ACCESS_PATHS
    },
    "coverage": {path: {"coverage"} for path in POSTPUBLISH_ACCESS_PATHS},
    "unresolved": {path: {"unresolved"} for path in POSTPUBLISH_ACCESS_PATHS},
    "closure": {path: {"closure"} for path in POSTPUBLISH_ACCESS_PATHS},
    "cursor-projection": {
        path: {"cursor-projection"} for path in POSTPUBLISH_ACCESS_PATHS
    },
    "canonical-export": {
        path: {"canonical-export"} for path in POSTPUBLISH_ACCESS_PATHS
    },
    "cross-path-equality": {None: {"cross-path-semantic-projection"}},
}

POSTPUBLISH_RETAINED_DIGEST_FIELDS = {
    "snapshot-manifest": "snapshot_manifest_digest",
    "partition-bindings": "partition_binding_multiset_digest",
    "rows": "row_multiset_digest",
    "claim-annotations": "claim_annotation_multiset_digest",
    "coverage": "coverage_multiset_digest",
    "unresolved": "unresolved_digest",
    "closure": "closure_digest",
    "cursor-projection": "cursor_projection_digest",
    "canonical-export": "canonical_export_digest",
}


def _postpublish_named_projection_digest(
    name: str,
    projection: dict[str, Any],
) -> str:
    """Recompute one named mismatch digest from a retained handle projection."""

    retained_field = POSTPUBLISH_RETAINED_DIGEST_FIELDS.get(name)
    if retained_field is not None:
        return projection[retained_field]
    record = projection["publication_record"]
    if name == "publication-semantic-fields":
        return _digest_projection(
            "cxxlens.clang22-reopened-publication-semantic-fields.v1",
            {
                field: record[field]
                for field in (
                    "publication_id",
                    "series_id",
                    "snapshot_id",
                    "sequence",
                    "parent_publication",
                    "state",
                    "corrupt",
                )
            },
        )
    if name == "physical-generation-transition":
        return _digest_projection(
            "cxxlens.clang22-reopened-physical-generation.v1",
            record["physical_generation"],
        )
    if name == "descriptor-inventory":
        return _digest_projection(
            "cxxlens.clang22-reopened-descriptor-inventory.v1",
            projection["descriptors"],
        )
    if name == "open-snapshot-return-binding":
        return _digest_projection(
            "cxxlens.clang22-reopened-open-snapshot-return.v1",
            record["snapshot_id"],
        )
    fail(
        "materialization.store-failure",
        f"post-publish named projection has no digest binding: {name}",
    )


def _validate_postpublish_projection_mismatch_binding(
    request: dict[str, Any],
    report: dict[str, Any],
    failure_record: dict[str, Any],
) -> None:
    """Bind mismatch digests to expected Store state and retained SDK receipts."""

    cause = failure_record["cause"]
    if cause["kind"] != "verification_mismatch":
        return
    attempt = report["semantic_verification"]["reopen_attempt"]
    receipts = attempt["handle_receipts"]
    if any(receipt["status"] != "present" for receipt in receipts):
        fail(
            "materialization.store-failure",
            "projection mismatch has a non-present retained handle",
        )
    invocation_record = report["publication"]["invocation_committed_record"]
    expected_projection, _ = _reopened_handle_projection(
        request,
        report["store"],
        invocation_record,
    )
    name = cause["projection"]
    if name == "cross-path-semantic-projection":
        expected_digest = expected_projection["semantic_projection_digest"]
        actual_digest = next(
            (
                receipt["projection"]["semantic_projection_digest"]
                for receipt in receipts
                if receipt["projection"]["semantic_projection_digest"]
                != expected_digest
            ),
            None,
        )
        if actual_digest is None:
            fail(
                "materialization.store-failure",
                "cross-path mismatch has no retained differing projection",
            )
    else:
        access_path = cause["access_path"]
        receipt = next(
            (
                candidate
                for candidate in receipts
                if candidate["access_path"] == access_path
            ),
            None,
        )
        if receipt is None:
            fail(
                "materialization.store-failure",
                "projection mismatch access path has no retained receipt",
            )
        expected_digest = _postpublish_named_projection_digest(
            name,
            expected_projection,
        )
        actual_digest = _postpublish_named_projection_digest(
            name,
            receipt["projection"],
        )
        if name == "physical-generation-transition":
            expected_generation = expected_projection["publication_record"][
                "physical_generation"
            ]
            actual_generation = receipt["projection"]["publication_record"][
                "physical_generation"
            ]
            if actual_generation >= expected_generation:
                fail(
                    "materialization.store-failure",
                    "physical generation mismatch describes an allowed transition",
                )
    if (
        expected_digest == actual_digest
        or cause["expected_digest"] != expected_digest
        or cause["actual_digest"] != actual_digest
    ):
        fail(
            "materialization.store-failure",
            "post-publish cause and retained projection mismatch digests differ",
        )


def _postpublish_first_cause_authority(
    failure_record: dict[str, Any],
) -> dict[str, Any]:
    """Project the source-private first observation retained by the report."""

    return {
        "stage": failure_record["stage"],
        "access_path": failure_record["access_path"],
        "cause": failure_record["cause"],
    }


def _validate_postpublish_first_cause_authority(
    failure_record: dict[str, Any],
    first_cause_authority: dict[str, Any] | None,
) -> None:
    if (
        first_cause_authority is None
        or first_cause_authority
        != _postpublish_first_cause_authority(failure_record)
    ):
        fail(
            "materialization.store-failure",
            "post-publish first cause differs from the source-private observation",
        )


def classify_postpublish_failure(
    attempt: dict[str, Any],
    failure_record: dict[str, Any],
    first_non_present: dict[str, Any] | None = None,
    *,
    first_cause_authority: dict[str, Any] | None,
) -> str:
    """Classify the exact first post-publish SDK or projection observation."""

    receipts = attempt.get("handle_receipts")
    if (
        not isinstance(receipts, list)
        or len(receipts) != len(POSTPUBLISH_ACCESS_PATHS)
        or tuple(receipt.get("access_path") for receipt in receipts)
        != POSTPUBLISH_ACCESS_PATHS
        or failure_record.get("code") != "materialization.store-failure"
    ):
        fail(
            "materialization.store-failure",
            "post-publish verification path census or stable code differs",
        )
    cause = failure_record["cause"]
    if attempt["close_reopen_status"] == "open_failed":
        if (
            any(receipt.get("status") != "not_attempted" for receipt in receipts)
            or first_non_present is not None
            or failure_record["stage"] != "close-reopen"
            or failure_record["access_path"] is not None
            or cause["kind"] != "sdk_error"
            or cause["operation"] != "store_reopen"
            or cause["access_path"] is not None
            or cause["code"] != attempt["open_error_code"]
            or cause["field"] != attempt["open_error_field"]
        ):
            fail(
                "materialization.store-failure",
                "close/reopen verification failure differs",
            )
        _validate_store_detail_observation(cause["detail"])
        _validate_postpublish_first_cause_authority(
            failure_record,
            first_cause_authority,
        )
        return "committed_unverified"
    if (
        attempt["close_reopen_status"] not in {"opened", "not_applicable"}
        or attempt["open_error_code"] is not None
        or attempt["open_error_field"] is not None
        or any(receipt.get("status") == "not_attempted" for receipt in receipts)
    ):
        fail(
            "materialization.store-failure",
            "post-publish reopen attempt state differs",
        )
    recomputed_first_non_present = next(
        (receipt for receipt in receipts if receipt.get("status") != "present"),
        None,
    )
    if (
        first_non_present is not None
        and first_non_present != recomputed_first_non_present
    ):
        fail(
            "materialization.store-failure",
            "caller-provided first reopened handle failure is not first",
        )
    first_non_present = recomputed_first_non_present
    if first_non_present is not None:
        access_path = first_non_present["access_path"]
        expected_operation = POSTPUBLISH_PATH_OPERATIONS[access_path]
        if (
            failure_record["stage"] != access_path
            or failure_record["access_path"] != access_path
            or cause["kind"] != "sdk_error"
            or cause["operation"] != expected_operation
            or cause["access_path"] != access_path
            or cause["code"] != first_non_present["sdk_code"]
            or cause["field"] != first_non_present["sdk_field"]
        ):
            fail(
                "materialization.store-failure",
                "first reopened handle failure differs",
            )
        _validate_store_detail_observation(cause["detail"])
        _validate_postpublish_first_cause_authority(
            failure_record,
            first_cause_authority,
        )
        return "committed_unverified"
    stage_bindings = POSTPUBLISH_MISMATCH_BINDINGS.get(failure_record["stage"], {})
    allowed_projections = stage_bindings.get(failure_record["access_path"], set())
    if (
        cause["kind"] != "verification_mismatch"
        or cause["operation"] != "verify_projection"
        or cause["access_path"] != failure_record["access_path"]
        or cause["projection"] not in allowed_projections
        or cause["expected_digest"] == cause["actual_digest"]
    ):
        fail(
            "materialization.store-failure",
            "all-present reopen failure lacks an exact projection mismatch",
        )
    _validate_postpublish_first_cause_authority(
        failure_record,
        first_cause_authority,
    )
    return "committed_unverified"


def _validate_store_detail_observation(detail: dict[str, Any]) -> None:
    if detail["kind"] == "stable":
        return
    diagnostic_bytes = detail["diagnostic"].encode("utf-8")
    if (
        detail["byte_count"] != len(diagnostic_bytes)
        or detail["digest"] != content_digest(diagnostic_bytes)
    ):
        fail(
            "materialization.store-failure",
            "opaque Store detail receipt differs from observed bytes",
        )


class WriterPublishInvariantBreach(MaterializationError):
    """A publish tuple that must terminate without an authoritative response."""

    def __init__(self, message: str) -> None:
        super().__init__("materialization.store-failure", message)


def _writer_publish_invariant_breach(message: str) -> NoReturn:
    raise WriterPublishInvariantBreach(message)


def classify_writer_publish_failure(
    request: dict[str, Any],
    publication: dict[str, Any],
) -> tuple[str, str]:
    """Classify the complete registered SQLite writer_publish tuple table."""

    cause = publication["store_failure"]["cause"]
    if (
        publication["backend"] != "sqlite"
        or cause["kind"] != "sdk_error"
        or cause["operation"] != "writer_publish"
        or cause["access_path"] is not None
    ):
        _writer_publish_invariant_breach(
            "writer_publish tuple is an invariant breach requiring exit two",
        )
    _validate_store_detail_observation(cause["detail"])
    empty_detail = {"kind": "stable", "value": ""}
    candidate_snapshot = publication["candidate_snapshot_id"]
    candidate_identity = publication["candidate_identity"]
    if not isinstance(candidate_identity, dict) or not isinstance(
        candidate_identity.get("publication_id"), str
    ):
        _writer_publish_invariant_breach(
            "writer_publish tuple lacks a constructed candidate identity and is an invariant breach",
        )
    candidate_publication = candidate_identity["publication_id"]
    exact = (cause["code"], cause["field"], cause["detail"])
    if exact == (
        "store.publication-conflict",
        request["publication"]["series_id"],
        empty_detail,
    ):
        return "stale_parent", "rejected_stale"
    if (
        cause["code"] == "store.counter-overflow"
        and cause["field"] in {"publication_sequence", "physical_generation"}
        and cause["detail"] == empty_detail
    ):
        return "counter_overflow", "rejected_store_failure"
    if exact == ("store.hash-collision", candidate_snapshot, empty_detail):
        return "hash_collision", "rejected_store_failure"
    if exact == ("store.snapshot-ambiguous", candidate_snapshot, empty_detail):
        return "persistence_corrupt", "rejected_store_failure"
    if (
        cause["code"] == "store.sqlite-failure"
        and cause["field"] == "database"
        and cause["detail"]["kind"] == "opaque"
    ):
        return "persistence_io", "publication_outcome_unknown"
    allowed_corruption = {
        "sqlite": {
            "backend",
            "column-count",
            "publication-row",
            "series-head-count",
            "series-head",
            "series-head-sequence",
        },
        candidate_publication: {
            "authority-record",
            "duplicate-publication-id",
            "parent",
            "parent-sequence",
        },
        request["publication"]["series_id"]: {
            "duplicate-sequence",
            "series-roots",
            "series-head-cas",
        },
    }
    if (
        cause["code"] == "store.corrupt"
        and cause["detail"]["kind"] == "stable"
        and cause["detail"]["value"]
        in allowed_corruption.get(cause["field"], set())
    ):
        return "persistence_corrupt", "rejected_store_failure"
    _writer_publish_invariant_breach(
        "writer_publish tuple is unlisted or an invariant breach requiring exit two",
    )


def writer_publish_invariant_breach_disposition(
    request: dict[str, Any],
    publication: dict[str, Any],
) -> dict[str, Any]:
    """Exercise the required process boundary for an invariant-breach tuple."""

    try:
        classify_writer_publish_failure(request, publication)
    except WriterPublishInvariantBreach as error:
        diagnostic_bytes = str(error).encode("utf-8")
        return {
            "schema": "cxxlens.clang22-materialization-execution-receipt.v1",
            "actual_exit_status": 2,
            "exact_stdout_byte_count": 0,
            "stdout_sha256": content_digest(b""),
            "parsed_response_count": 0,
            "stderr_sha256": content_digest(diagnostic_bytes),
        }
    raise ValueError("writer_publish tuple is classified and may use a typed response")


def _validate_writer_publish_failure_cause(
    request: dict[str, Any],
    publication: dict[str, Any],
) -> None:
    category, outcome = classify_writer_publish_failure(request, publication)
    if (
        publication["store_failure"]["category"] != category
        or publication["outcome"] != outcome
    ):
        fail(
            "materialization.store-failure",
            "writer_publish tuple category/outcome mapping differs",
        )


def validate_failed_publication_outcome(
    request: dict[str, Any],
    report: dict[str, Any],
    expected_store: dict[str, Any],
    *,
    postpublish_failure_authority: dict[str, Any] | None,
) -> None:
    publication = report["publication"]
    outcome = publication["outcome"]
    if (
        publication["backend"] != request["publication"]["backend"]
        or publication["selector"] != request["publication"]["selector"]
        or publication["series_id"] != request["publication"]["series_id"]
        or publication["genesis"] != request["publication"]["genesis"]
        or publication["expected_parent_publication"]
        != request["publication"]["expected_parent_publication"]
        or publication["candidate_snapshot_id"]
        != expected_store["snapshot_manifest"]["snapshot_id"]
        or publication["partial_policy"] != "forbid"
        or not publication["publication_attempted"]
    ):
        fail("materialization.store-failure", "failed publication request binding differs")
    _validate_candidate_publication_identity(request, publication, expected_store)
    if publication["head_observation"] == "present":
        observed = publication["observed_parent_record"]
        _validate_publication_record(
            observed,
            expected_series_id=publication["series_id"],
            subject="observed parent",
        )
        if observed["publication_id"] != publication["observed_parent_publication"]:
            fail("materialization.store-failure", "observed parent binding differs")
    semantic = report["semantic_verification"]
    expected_category: str
    if outcome == "rejected_stale":
        expected_category = "stale_parent"
        expected_semantic_status = "not_published"
    elif outcome == "rejected_store_failure":
        expected_category = publication["store_failure"]["category"]
        if expected_category not in {
            "counter_overflow",
            "hash_collision",
            "persistence_corrupt",
        }:
            fail(
                "materialization.store-failure",
                "definitive zero-commit Store category differs",
            )
        expected_semantic_status = "not_published"
    elif outcome == "publication_outcome_unknown":
        expected_category = "persistence_io"
        expected_semantic_status = "publication_unknown"
    elif outcome == "committed_unverified":
        expected_category = "post_commit_verification"
        expected_semantic_status = "committed_unverified"
    else:
        fail("materialization.store-failure", "failed publication outcome differs")
    if (
        publication["store_failure"]["category"] != expected_category
        or semantic["status"] != expected_semantic_status
        or semantic["reopened_store"] is not None
    ):
        fail("materialization.store-failure", "failed Store outcome binding differs")
    if outcome in {
        "rejected_stale",
        "rejected_store_failure",
        "publication_outcome_unknown",
    }:
        _validate_writer_publish_failure_cause(request, publication)
    if outcome in {"rejected_stale", "rejected_store_failure"}:
        if (
            publication["invocation_commit_state"] != "not_committed"
            or publication["committed_transaction_count"] != 0
            or publication["invocation_committed_record"] is not None
            or semantic["reopen_attempt"] is not None
            or semantic["failure"] is not None
        ):
            fail(
                "materialization.store-failure",
                "rejected publication zero-commit evidence differs",
            )
    elif outcome == "publication_outcome_unknown":
        if (
            publication["candidate_identity_state"] != "constructed"
            or publication["invocation_commit_state"] != "unknown"
            or publication["committed_transaction_count"] is not None
            or publication["invocation_committed_record"] is not None
            or semantic["reopen_attempt"] is not None
            or semantic["failure"] is not None
        ):
            fail(
                "materialization.store-failure",
                "phase-opaque publication outcome is not unknown",
            )
    else:
        committed = publication["invocation_committed_record"]
        _validate_publication_record(
            committed,
            expected_series_id=publication["series_id"],
            subject="invocation-committed",
        )
        if (
            publication["invocation_commit_state"] != "committed"
            or publication["committed_transaction_count"] != 1
            or _publication_identity_projection(committed)
            != publication["candidate_identity"]
            or semantic["reopen_attempt"] is None
            or semantic["failure"] is None
        ):
            fail(
                "materialization.store-failure",
                "committed-unverified invocation evidence differs",
            )
        _validate_committed_unverified_attempt(
            request,
            report,
            postpublish_failure_authority=postpublish_failure_authority,
        )
    if publication["backend"] == "sqlite":
        _validate_publication_recovery_receipt(request, publication)
        if publication["sqlite_reopen_status"] != publication["recovery_receipt"][
            "reopen_status"
        ]:
            fail("materialization.store-failure", "SQLite reopen status differs")
        candidate_status = publication["recovery_receipt"]["candidate"]["status"]
        if publication["candidate_identity_state"] == "not_constructed":
            expected_visibility = "not_applicable"
        elif candidate_status == "not_found":
            expected_visibility = "absent"
        elif candidate_status in {"error", "not_attempted"}:
            expected_visibility = "unknown"
        elif candidate_status == "present":
            expected_visibility = (
                "present_unknown_attribution"
                if outcome == "publication_outcome_unknown"
                else (
                    "present_by_invocation"
                    if outcome == "committed_unverified"
                    else "present_external"
                )
            )
        else:
            expected_visibility = "not_applicable"
        if publication["candidate_visibility"] != expected_visibility:
            fail(
                "materialization.store-failure",
                "candidate recovery visibility differs",
            )
    elif publication["recovery_receipt"] is not None:
        fail("materialization.store-failure", "memory outcome has SQLite recovery receipt")


def _semantic_matrix_projection(request: dict[str, Any]) -> dict[str, Any]:
    return {
        "worker": {
            key: request["worker"][key]
            for key in (
                "provider_id",
                "provider_version",
                "semantic_contract_digest",
                "protocol_major",
                "protocol_minor",
                "required_features",
                "sandbox_policy_digest",
            )
        },
        "project": request["project"],
        "registry": request["registry"],
        "engine": request["engine"],
        "interpretation_policy": request["interpretation_policy"],
        "trust_policy": request["trust_policy"],
        "selector": request["publication"]["selector"],
        "series_id": request["publication"]["series_id"],
        "tasks": [
            {
                key: task[key]
                for key in (
                    "provider_task_id",
                    "task_input_digest",
                    "selected_catalog_compile_unit_id",
                    "compile_unit_id",
                    "build_variant_id",
                    "toolchain_context_id",
                    "toolchain_digest",
                    "normalized_invocation_digest",
                    "environment_digest",
                    "condition_universe_id",
                    "condition_id",
                    "interpretation_domain",
                    "source",
                    "effective_argv",
                    "requested_descriptor_ids",
                    "dependency_groups",
                )
            }
            for task in request["tasks"]
        ],
    }


def validate_qualification_matrix(
    root: pathlib.Path,
    entries: list[
        tuple[dict[str, Any], dict[str, Any]]
        | tuple[dict[str, Any], dict[str, Any], bytes]
    ],
) -> None:
    expected = {
        (configuration, backend)
        for configuration in ("static", "shared")
        for backend in ("memory", "sqlite")
    }
    actual: list[tuple[str, str]] = []
    projections: list[dict[str, Any]] = []
    snapshot_ids: set[str] = set()
    publication_ids: set[str] = set()
    exports: set[str] = set()
    queries: set[str] = set()
    base_claim_sets: set[bytes] = set()
    claim_stage_sets: set[bytes] = set()
    global_provenance_sets: set[bytes] = set()
    normalized_entries: list[tuple[dict[str, Any], dict[str, Any], bytes]] = []
    for entry in entries:
        if len(entry) == 2:
            request, report = entry
            request_bytes = canonical_json(request)
        elif len(entry) == 3:
            request, report, request_bytes = entry
            if not isinstance(request_bytes, bytes):
                fail(
                    "materialization.report-invalid",
                    "qualification exact request artifact is not bytes",
                )
        else:
            fail("materialization.report-invalid", "qualification entry arity differs")
        normalized_entries.append((request, report, request_bytes))
        validate_report(
            root,
            request,
            report,
            request_bytes=request_bytes,
        )
        if report["result"] != "passed":
            fail("materialization.report-invalid", "qualification matrix contains failure")
        guarantee = report["side_channels"]["guarantee"]
        if guarantee["approximation"] != "exact" or any(
            census["non_exact_equivalence_count"] != 0
            for census in guarantee["observation_descriptor_censuses"]
        ):
            fail(
                "materialization.coverage-incomplete",
                "qualification requires exact observation equivalence",
            )
        actual.append(
            (
                request["tool"]["package_configuration"],
                request["publication"]["backend"],
            )
        )
        projections.append(_semantic_matrix_projection(request))
        snapshot_ids.add(report["store"]["snapshot_manifest"]["snapshot_id"])
        publication_ids.add(
            report["publication"]["invocation_committed_record"]["publication_id"]
        )
        reopened = report["semantic_verification"]["reopened_store"]
        exports.add(reopened["canonical_export_digest"])
        queries.add(reopened["cursor_projection"]["digest"])
        base_claim_sets.add(canonical_json(report["base_claims"]))
        claim_stage_sets.add(canonical_json(report["claim_stages"]))
        global_provenance_sets.add(canonical_json(report["provenance"]))
    if set(actual) != expected or len(actual) != len(expected):
        fail("materialization.report-invalid", f"installed matrix differs: {actual}")
    if any(projection != projections[0] for projection in projections[1:]):
        fail("materialization.report-invalid", "matrix project/provider semantics differ")
    for configuration in ("static", "shared"):
        semantic_keys = {
            request["semantic_request_digest"]
            for request, _, _ in normalized_entries
            if request["tool"]["package_configuration"] == configuration
        }
        if len(semantic_keys) != 1:
            fail(
                "materialization.report-invalid",
                f"{configuration} memory/SQLite semantic request digests differ",
            )
    if (
        len(snapshot_ids) != 1
        or len(publication_ids) != 1
        or len(exports) != 1
        or len(queries) != 1
        or len(base_claim_sets) != 1
        or len(claim_stage_sets) != 1
        or len(global_provenance_sets) != 1
    ):
        fail("materialization.report-invalid", "backend/configuration semantic parity differs")


def validate_report_lifecycle_authority_text(
    design_text: str,
    adr_text: str,
    contract_text: str,
) -> None:
    normalized_design = " ".join(design_text.split())
    normalized_adr = " ".join(adr_text.split())
    normalized_contract = " ".join(contract_text.split())
    for forbidden in FORBIDDEN_REPORT_LIFECYCLE_TEXT:
        if (
            forbidden in normalized_design
            or forbidden in normalized_adr
            or forbidden in normalized_contract
        ):
            fail(
                "materialization.report-invalid",
                f"legacy report lifecycle text was reintroduced: {forbidden}",
            )
    for required in (
        "DF-0194",
        "bounded two-phase",
        "short/partial write",
        "OS-level all-or-nothing atomicity",
    ):
        if required not in normalized_adr:
            fail(
                "materialization.report-invalid",
                f"ADR two-phase report lifecycle marker is missing: {required}",
            )
    for required in (
        "DF-0194",
        "bounded two-phase",
        "publish attempt 後",
        "request/report v2 shape、identity、public Store API は変更しない",
    ):
        if required not in normalized_design:
            fail(
                "materialization.report-invalid",
                f"integrated design two-phase report lifecycle marker is missing: {required}",
            )


def validate_base64_authority_text(design_text: str, adr_text: str) -> None:
    """Fail closed when the accepted design/ADR loses DF-0199 canonicality markers."""

    normalized_design = " ".join(design_text.split())
    normalized_adr = " ".join(adr_text.split())
    required_markers = (
        "DF-0199",
        "zero discarded padding bits",
        "non-zero discarded padding bits",
        "sealed source bytes",
        "version `2.1.0`",
        "qualified",
    )
    for label, value in (("integrated design", normalized_design), ("ADR", normalized_adr)):
        for required in required_markers:
            if required not in value:
                fail(
                    "materialization.task-binding-mismatch",
                    f"{label} canonical Base64 marker is missing: {required}",
                )


def validate_occurrence_snapshot_authority_text(
    design_text: str,
    adr_text: str,
) -> None:
    """Bind the accepted occurrence authority to one immutable snapshot per role."""

    required = {
        "integrated design": (
            "exactly one の private memfd snapshot",
            "retained sealed snapshot の独立 read-only handle",
            "artifact bytes を再 copy しない",
            "measurement 後の rename または in-place mutation",
        ),
        "ADR": (
            "F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL",
            "exactly one の immutable snapshot",
            "同一 role の再 copy",
            "measurement 完了後の path replacement",
        ),
    }
    documents = {
        "integrated design": " ".join(design_text.split()),
        "ADR": " ".join(adr_text.split()),
    }
    for label, markers in required.items():
        for marker in markers:
            if marker not in documents[label]:
                fail(
                    "materialization.occurrence-invalid",
                    f"{label} immutable occurrence snapshot marker is missing: {marker}",
                )


def validate_private_spool_authority_text(
    design_text: str,
    adr_text: str,
) -> None:
    """Bind every materialization private spool to verified Linux memfd sealing."""

    required = {
        "integrated design": (
            "`memfd_create(..., MFD_CLOEXEC | MFD_ALLOW_SEALING)`",
            "pathname、`mkstemp`、unlink 済み mutable inode への fallback は禁止",
            "`F_ADD_SEALS`",
            "`F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL`",
            "`F_GET_SEALS`",
            "`fstat` の actual size と append census",
            "sealed bytes の SHA-256 と append transcript の incremental SHA-256",
            "size/content binding の不一致を source-private no-response",
            "compile-time absence と missing-bit contradiction は no-response",
        ),
        "ADR": (
            "`memfd_create(..., MFD_CLOEXEC | MFD_ALLOW_SEALING)`",
            "pathname、`mkstemp`、unlink 後の mutable inode への fallback を禁止",
            "`F_ADD_SEALS`",
            "`F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL`",
            "`F_GET_SEALS`",
            "`fstat` の actual size と append census",
            "sealed bytes の SHA-256 と append transcript の incremental SHA-256",
            "drift は worker、Store、file effect 前の source-private no-response",
            "compile-time absence と missing-bit contradiction は no-response",
        ),
    }
    documents = {
        "integrated design": " ".join(design_text.split()),
        "ADR": " ".join(adr_text.split()),
    }
    for label, markers in required.items():
        for marker in markers:
            if marker not in documents[label]:
                fail(
                    "materialization.request-invalid",
                    f"{label} private-spool sealing marker is missing: {marker}",
                )


def validate_materialization_io_taxonomy_header(header: str) -> None:
    """Bind stable spool failure to the exact operation-authentic kind matrix."""

    normalized = " ".join(header.split())
    begin = normalized.find(
        "[[nodiscard]] constexpr bool "
        "is_materialization_actual_io_or_hash_failure("
    )
    end = normalized.find("template <class Value>", begin)
    expected = " ".join(
        """
        [[nodiscard]] constexpr bool
        is_materialization_actual_io_or_hash_failure(
            const materialization_io_failure& failure) noexcept
        {
            switch (failure.operation)
            {
                case materialization_io_operation::input_read:
                    return failure.kind == materialization_io_failure_kind::read;
                case materialization_io_operation::spool_write:
                    return failure.kind == materialization_io_failure_kind::write ||
                        failure.kind == materialization_io_failure_kind::spool;
                case materialization_io_operation::spool_seal:
                case materialization_io_operation::spool_create:
                case materialization_io_operation::spool_rewind:
                    return failure.kind == materialization_io_failure_kind::spool;
                case materialization_io_operation::spool_read:
                    return failure.kind == materialization_io_failure_kind::read ||
                        failure.kind == materialization_io_failure_kind::spool;
                case materialization_io_operation::digest_update:
                case materialization_io_operation::digest_finalize:
                    return failure.kind == materialization_io_failure_kind::hash;
                case materialization_io_operation::configuration:
                case materialization_io_operation::buffer_allocation:
                    return false;
            }
            return false;
        }
        """.split()
    ).replace("failure( const", "failure(const")
    if begin < 0 or end < 0 or normalized[begin:end].strip() != expected:
        fail(
            "materialization.request-invalid",
            "materialization I/O kind-by-operation authenticity matrix differs",
        )


def validate_private_spool_implementation_text(source: str) -> None:
    """Reject mutable fallbacks or a logical seal not bound to exact sealed bytes."""

    for forbidden in ("mkstemp", "unlink", "O_TMPFILE", "mark_memfd("):
        if forbidden in source:
            fail(
                "materialization.request-invalid",
                f"private-spool mutable fallback was reintroduced: {forbidden}",
            )

    normalized = " ".join(source.replace("\\\n", " ").split())
    required = (
        "defined(__linux__) && defined(SYS_memfd_create) && defined(MFD_CLOEXEC)",
        "defined(MFD_ALLOW_SEALING) && defined(F_ADD_SEALS) && defined(F_GET_SEALS)",
        "F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL",
        "SYS_memfd_create, \"cxxlens-materialization\", MFD_CLOEXEC | MFD_ALLOW_SEALING",
        "::fcntl(descriptor_, F_ADD_SEALS, required_memfd_seals)",
        "const auto observed_seals = ::fcntl(descriptor_, F_GET_SEALS);",
        "(observed_seals & required_memfd_seals) != required_memfd_seals",
        "auto updated = expected_digest_->update(bytes);",
        "::fstat(descriptor_, &status)",
        "static_cast<std::uint64_t>(status.st_size) != size_",
        "auto verified = verify_sealed_bytes();",
        "if (!verified) { poisoned_ = true; return verified.error(); }",
        "auto expected = expected_digest_->finish();",
        "auto observed_digest = make_materialization_sha256_accumulator();",
        "ssize_t count{}; do { count = ::pread(",
        "descriptor_, buffer.data(), requested, static_cast<off_t>(offset));",
        "auto observed = observed_digest->finish();",
        "if (*observed != *expected)",
        "expected_digest_.reset();",
        "sealed_ = true;",
    )
    for marker in required:
        if marker not in normalized:
            fail(
                "materialization.request-invalid",
                f"private-spool fail-closed implementation marker is missing: {marker}",
            )

    add_position = normalized.find(
        "::fcntl(descriptor_, F_ADD_SEALS, required_memfd_seals)"
    )
    get_position = normalized.find(
        "const auto observed_seals = ::fcntl(descriptor_, F_GET_SEALS);"
    )
    verify_position = normalized.find(
        "(observed_seals & required_memfd_seals) != required_memfd_seals"
    )
    size_position = normalized.find("::fstat(descriptor_, &status)")
    content_call_position = normalized.find("auto verified = verify_sealed_bytes();")
    poison_position = normalized.find(
        "if (!verified) { poisoned_ = true; return verified.error(); }"
    )
    publish_position = normalized.find("sealed_ = true;")
    if not (
        normalized.count("sealed_ = true;") == 1
        and 0
        <= add_position
        < get_position
        < verify_position
        < size_position
        < content_call_position
        < poison_position
        < publish_position
    ):
        fail(
            "materialization.request-invalid",
            "private-spool add/get/size/content/logical-seal order differs",
        )

    expected_position = normalized.find("auto expected = expected_digest_->finish();")
    observed_create_position = normalized.find(
        "auto observed_digest = make_materialization_sha256_accumulator();"
    )
    sealed_read_position = normalized.find(
        "ssize_t count{}; do { count = ::pread("
    )
    observed_finish_position = normalized.find(
        "auto observed = observed_digest->finish();"
    )
    content_compare_position = normalized.find("if (*observed != *expected)")
    if not (
        0
        <= expected_position
        < observed_create_position
        < sealed_read_position
        < observed_finish_position
        < content_compare_position
    ):
        fail(
            "materialization.request-invalid",
            "private-spool sealed-byte transcript verification order differs",
        )

    factory_failure = (
        "if (descriptor < 0) return materialization_io_failure{"
        "materialization_io_failure_kind::spool, "
        "materialization_io_operation::spool_create};"
    )
    unsupported_failure = (
        "#else return materialization_io_failure{"
        "materialization_io_failure_kind::invalid_configuration, "
        "materialization_io_operation::spool_create}; #endif"
    )
    if factory_failure not in normalized or unsupported_failure not in normalized:
        fail(
            "materialization.request-invalid",
            "private-spool factory does not fail closed without a safe memfd",
        )

    successful_contradictions = (
        (
            "if (count == 0 || static_cast<std::size_t>(count) > bytes.size() - offset) "
            "{ poisoned_ = true; return materialization_io_failure{ "
            "materialization_io_failure_kind::invalid_configuration, "
            "materialization_io_operation::spool_write}; }"
        ),
        (
            "if ((observed_seals & required_memfd_seals) != required_memfd_seals) "
            "{ poisoned_ = true; return materialization_io_failure{ "
            "materialization_io_failure_kind::invalid_configuration, "
            "materialization_io_operation::spool_seal}; }"
        ),
        (
            "if (status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) != size_) "
            "{ poisoned_ = true; return materialization_io_failure{ "
            "materialization_io_failure_kind::invalid_configuration, "
            "materialization_io_operation::spool_seal}; }"
        ),
        (
            "if (count == 0 || static_cast<std::size_t>(count) > available) "
            "return materialization_io_failure{ "
            "materialization_io_failure_kind::invalid_configuration, "
            "materialization_io_operation::spool_read};"
        ),
        (
            "if (*received == 0U || *received > destination.size()) "
            "return failure(materialization_io_failure_kind::invalid_configuration, "
            "materialization_io_operation::spool_read);"
        ),
        (
            "if (*received > requested) return failure("
            "materialization_io_failure_kind::invalid_configuration, "
            "materialization_io_operation::input_read);"
        ),
        (
            "if (!exact_sha256(*finished)) return failure("
            "materialization_io_failure_kind::invalid_configuration, "
            "materialization_io_operation::digest_finalize);"
        ),
    )
    for marker in successful_contradictions:
        if marker not in normalized:
            fail(
                "materialization.request-invalid",
                "successful private-spool contradiction taxonomy differs: " + marker,
            )


def validate_v2_1_admission_authority_text(
    design_text: str,
    adr_text: str,
) -> None:
    """Bind selected-schema ceilings, compact equality, and spool phases."""

    documents = {
        "integrated design": (
            " ".join(design_text.split()),
            (
                "generic array count と non-envelope string length は raw 1 GiB reachability",
                "resident duplicate-name ledger を持つ object member は 4096",
                "4097 件目の後も strict lexical scan",
                "`trust_policy.task_sandbox_requirements`",
                "selected schema が `maxItems: 4096`",
                "`request-schema` / `materialization.request-invalid`",
                "bounded string-view tuple",
                "O(N log N)",
                "43 / 6 bytes",
                "fixed 64-byte private-spool record",
                "digest-only equality",
                "`materialization.spool-failure`",
                "raw observation を確立できない最初の capture spool failure は exit 2 / no response",
                "lexical raw/task-index は `json-decode`",
                "selected schema の global/task/source/index/uniqueItems は `request-schema`",
                "full schema 後の source/task.v3/ identity/task-index/execution uniqueness は `request-binding`",
                "source-private no-response signal",
                "`materialization.io-failure` / `materialization.resource-limit` / `materialization.internal-failure`",
                "semantic JSON token replay",
                "insignificant whitespace",
                "minimal JSON escaping",
                "integer spelling",
                "`tasks=[]`",
                "`source.content_base64=\"\"`",
                "`10,420,985`",
                "`8,463,179`",
                "`56,687,879`",
                "`58,645,685`",
                "`sha256:241fc96ae3a249e5a8851baa95e585460ad29378cb20d11cfcda33a69eaa9270`",
                "`sha256:ff9baf9982f909d8a4f51c46f53637af6980a7d06728dfa65794ffc1eebf816d`",
                "accepted SQLite Option A authority; report-schema activation applied, qualification pending",
                "request 2.1.0 shape は不変",
                "13/19-file occurrence inventory",
                "`task_sandbox_requirements maxItems: 4096`",
                "sha256:f321e25f72bf8c6312dfe1e36fe6b6573239db697c2cfabd60e2c0546f9ee98b",
                "operation-authentic kind×operation matrix",
                "全 mismatched pair は no-response",
                "root member の missing/extra",
                "collision reverse read",
                "successful write-drop",
                "raw-spelling inflation",
                "1 GiB raw transport reachability",
            ),
        ),
        "ADR-0096": (
            " ".join(adr_text.split()),
            (
                "generic array count と non-envelope value-string length",
                "object member は duplicate-name ledger が resident であるため 4096",
                "4097 件目を観測しても残りの strict lexical scan",
                "`trust_policy.task_sandbox_requirements`",
                "selected schema がそれぞれ `maxItems: 4096`",
                "`request-schema` / `materialization.request-invalid`",
                "bounded string-view projection",
                "O(N log N)",
                "43 と 6",
                "fixed 64-byte record",
                "digest match だけを JSON Schema equality authority にしない",
                "`materialization.spool-failure`",
                "complete raw-input observation を保持できない最初の raw spool create/capture failure は exit 2 / no authoritative response",
                "lexical raw/task-index は `json-decode`",
                "selected-schema global/task/source/index/ uniqueItems は `request-schema`",
                "full schema 後の source/task.v3/identity/task-index/execution uniqueness は `request-binding`",
                "source-private no-response signal",
                "`materialization.io-failure` / `materialization.resource-limit` / `materialization.internal-failure`",
                "semantic JSON token replay",
                "insignificant whitespace",
                "minimal JSON escaping",
                "integer spelling",
                "`tasks=[]`",
                "`source.content_base64=\"\"`",
                "`10,420,985`",
                "`8,463,179`",
                "`56,687,879`",
                "`58,645,685`",
                "`sha256:241fc96ae3a249e5a8851baa95e585460ad29378cb20d11cfcda33a69eaa9270`",
                "`sha256:ff9baf9982f909d8a4f51c46f53637af6980a7d06728dfa65794ffc1eebf816d`",
                "accepted Option A; report-schema activation applied, qualification pending",
                "request 2.1.0 shape は不変",
                "13/19-file occurrence inventory",
                "sandbox array bound",
                "sha256:f321e25f72bf8c6312dfe1e36fe6b6573239db697c2cfabd60e2c0546f9ee98b",
                "operation-authentic kind×operation matrix",
                "全 mismatched pair は no-response",
                "root member missing/extra",
                "collision reverse read",
                "successful write-drop",
                "raw-spelling inflation",
                "1 GiB raw request reachability",
            ),
        ),
    }
    for label, (document, markers) in documents.items():
        for marker in markers:
            if marker not in document:
                fail(
                    "materialization.request-invalid",
                    f"{label} v2.1 admission authority marker is missing: {marker}",
                )


def validate_v2_1_admission_implementation_text(
    stream_header: str,
    stream_source: str,
    admission_source: str,
    admission_error_header: str,
    identity_source: str,
    task_spool_source: str,
    request_driver: str,
) -> None:
    """Bind selected-schema limits and exact compact uniqueness to their authority."""

    normalized_header = " ".join(stream_header.split())
    normalized_stream = " ".join(stream_source.split())
    normalized_admission = " ".join(admission_source.split())
    normalized_error_header = " ".join(admission_error_header.split())
    normalized_identity = " ".join(identity_source.split())
    normalized_task_spool = " ".join(task_spool_source.split())
    normalized_driver = " ".join(request_driver.split())
    required_header = (
        (
            "maximum_materialization_request_schema_capture_bytes = "
            "materialization_request_schema_v2.size() + 1U"
        ),
        (
            "maximum_materialization_request_version_capture_bytes = "
            "materialization_request_version_v2_1.size() + 1U"
        ),
        (
            "maximum_materialization_global_request_window_bytes = "
            "64U * 1024U * 1024U"
        ),
        (
            "maximum_materialization_task_metadata_window_bytes = "
            "64U * 1024U * 1024U"
        ),
        "maximum_members_per_object{4096U}",
        "maximum_elements_per_array{static_cast<std::size_t>(maximum_raw_request_bytes)}",
        "maximum_string_utf8_bytes{static_cast<std::size_t>(maximum_raw_request_bytes)}",
    )
    for marker in required_header:
        if marker not in normalized_header:
            fail(
                "materialization.request-invalid",
                f"v2.1 pass-one lexical/resource marker is missing: {marker}",
            )

    required_stream = (
        "tasks_exceeded_selected_maximum_ = true;",
        'scan_error("request-schema", "tasks-maxItems", cursor_.position())',
        "limits.max_array_elements = maximum_window_bytes;",
        "limits.max_total_values = maximum_window_bytes;",
        'task_index_io_error("json-decode", "create", 0U, spool.error())',
        'task_index_io_error("json-decode", "append", value_offset, written.error())',
        'task_index_io_error("request-schema", "seal", 0U, sealed.error())',
        '"task-index-private", "read", record_offset + filled, read.error()',
        "constexpr materialization_request_scan_limits authority_limits{};",
        "limits.maximum_depth != authority_limits.maximum_depth",
        "limits.maximum_members_per_object != authority_limits.maximum_members_per_object",
        "limits.maximum_elements_per_array != authority_limits.maximum_elements_per_array",
        "limits.maximum_string_utf8_bytes != authority_limits.maximum_string_utf8_bytes",
        'const auto capture_limit = name == "schema"',
        "maximum_materialization_request_schema_capture_bytes",
        "maximum_materialization_request_version_capture_bytes",
        "output->size() >= maximum_capture_bytes",
        "class semantic_json_replayer",
        "append_semantic_replayed_range",
        "append_code_point(code_point)",
        "if (decimal.all_zero) return append('0');",
        "maximum_output_bytes_",
        'replay.append("[]");',
        'replay.append("\\"\\"");',
        'replay_io_error(phase_, "read", position_, read.error())',
        "exact_materialization_task_json_equal",
        'collision_metadata_io_error("append", appended.error())',
        'collision_metadata_io_error("seal", sealed.error())',
        'collision_metadata_io_error("read", read.error())',
        'selected_shape_error(phase, "root-member-census", 0U)',
        'selected_shape_error(phase, "root-member-required", 0U)',
        'selected_shape_error(phase, "source-content-required", task.value_offset)',
        'selected_shape_error( phase, "source-content-string", *task.source_content_offset)',
        'if (phase == "request-schema") return scan_error('
        '"request-schema", std::move(reason), offset); return '
        "materialization_admission_no_response();",
        "metadata_comparison.size_bytes() != canonical_left.size()",
        "while (verified_left < canonical_left.size())",
        "if (!std::ranges::equal(std::span{comparison}.first(*read), expected))",
    )
    for marker in required_stream:
        if marker not in normalized_stream:
            fail(
                "materialization.request-invalid",
                f"v2.1 streaming admission marker is missing: {marker}",
            )

    left_replay = normalized_stream.find("canonical_left = canonical_json(left_metadata->root())")
    left_seal = normalized_stream.find("metadata_comparison.seal()", left_replay)
    reverse_size = normalized_stream.find(
        "metadata_comparison.size_bytes() != canonical_left.size()", left_seal
    )
    reverse_read = normalized_stream.find(
        "while (verified_left < canonical_left.size())", reverse_size
    )
    reverse_content = normalized_stream.find(
        "if (!std::ranges::equal(std::span{comparison}.first(*read), expected))",
        reverse_read,
    )
    right_replay = normalized_stream.find(
        "auto right_metadata = replay_materialization_task_metadata", reverse_content
    )
    if not (
        0
        <= left_replay
        < left_seal
        < reverse_size
        < reverse_read
        < reverse_content
        < right_replay
    ):
        fail(
            "materialization.request-invalid",
            "collision metadata left-authority reverse closure order differs",
        )
    for forbidden in ("append_replayed_range(", "replay_shape_error("):
        if forbidden in normalized_stream:
            fail(
                "materialization.request-invalid",
                f"raw-spelling replay shortcut was reintroduced: {forbidden}",
            )

    trust_begin = normalized_admission.find("validate_trust_schema(")
    trust_end = normalized_admission.find("validate_publication_schema(", trust_begin)
    if trust_begin < 0 or trust_end < 0:
        fail("materialization.request-invalid", "v2.1 trust schema validator is missing")
    trust_validator = normalized_admission[trust_begin:trust_end]
    if (
        '"task_sandbox_requirements"' not in trust_validator
        or "1U, 4096U, false" not in trust_validator
        or (
            "std::vector<std::pair<std::string_view, std::string_view>> "
            "exact_requirements"
        )
        not in trust_validator
        or "std::ranges::sort(exact_requirements)" not in trust_validator
        or "std::ranges::adjacent_find(exact_requirements)" not in trust_validator
        or "std::numeric_limits<std::size_t>::max()" in trust_validator
    ):
        fail(
            "materialization.request-invalid",
            "bounded trust requirement exact projection differs",
        )

    catalog_begin = normalized_admission.find("validate_project_schema(")
    catalog_end = normalized_admission.find("validate_registry_schema(", catalog_begin)
    if catalog_begin < 0 or catalog_end < 0:
        fail("materialization.request-invalid", "v2.1 project schema validator is missing")
    catalog_validator = normalized_admission[catalog_begin:catalog_end]
    required_catalog_projection = (
        '"catalog_compile_units", "project.catalog_compile_units", 1U, 4096U, false',
        (
            "using catalog_entry_key = std::tuple<std::string_view, std::string_view, "
            "std::string_view, std::string_view>"
        ),
        "exact_catalog_entries",
        "std::ranges::sort(exact_catalog_entries)",
        "std::ranges::adjacent_find(exact_catalog_entries)",
    )
    for marker in required_catalog_projection:
        if marker not in catalog_validator:
            fail(
                "materialization.request-invalid",
                f"catalog exact projection marker is missing: {marker}",
            )

    required_admission = (
        "compact_unique_record_bytes = 64U",
        "std::array<compact_unique_record, maximum_materialization_tasks>",
        "seal_and_validate_compact_unique_records",
        "materialization.spool-failure",
        "task_unique_index",
        "task_collision_metadata",
        "execution_unique_index",
        "exact_materialization_task_json_equal(",
        'task_index_phase_error(std::move(span.error()), "request-schema")',
        'task_index_phase_error(std::move(span.error()), "request-binding")',
        "materialization_admission_no_response()",
    )
    for marker in required_admission:
        if marker not in normalized_admission:
            fail(
                "materialization.request-invalid",
                f"v2.1 compact uniqueness marker is missing: {marker}",
            )
    for forbidden in (
        "seal_and_validate_unique_records",
        "append_replay_bytes",
        "materialization.internal-failure",
    ):
        if forbidden in normalized_admission:
            fail(
                "materialization.request-invalid",
                f"v2.1 unbounded or uncontracted admission path was reintroduced: {forbidden}",
            )

    typed_mapper_requirements = {
        "stream": (
            normalized_stream,
            (
                "return materialization_admission_io_failure(",
                'failure, "request-schema", "task-collision-metadata:"',
                '"raw-replay:" + std::string{operation}',
            ),
        ),
        "v2.1": (
            normalized_admission,
            (
                "auxiliary_create_failure(",
                "auxiliary_io_failure(",
                "return materialization_admission_io_failure(",
                "materialization_v2_1_auxiliary_spool_purpose::task_input_digest",
                "auto digest = auxiliary_spools.make_digest(purpose);",
                "if (!has_content_digest_grammar(*finished)) return sdk::unexpected("
                "materialization_admission_no_response());",
                "if (!*unique_records) return sdk::unexpected("
                "materialization_admission_no_response());",
                "if (!*comparison) return sdk::unexpected("
                "materialization_admission_no_response());",
                "if (!*execution_keys) return sdk::unexpected("
                "materialization_admission_no_response());",
            ),
        ),
        "identity": (
            normalized_identity,
            (
                "class production_identity_digest_factory final",
                "materialization_request_identity_digest_factory& digest_factory",
                "return materialization_admission_io_failure(",
                "if (!exact_sha256(*digest)) return sdk::unexpected("
                "materialization_admission_no_response());",
            ),
        ),
        "task spool": (
            normalized_task_spool,
            (
                "return materialization_admission_io_failure(",
                "if (!exact_sha256(*output)) return sdk::unexpected("
                "materialization_admission_no_response());",
            ),
        ),
    }
    for label, (source, markers) in typed_mapper_requirements.items():
        if "failure.kind" in source:
            fail(
                "materialization.request-invalid",
                f"{label} bypasses the central typed I/O classifier",
            )
        for marker in markers:
            if marker not in source:
                fail(
                    "materialization.request-invalid",
                    f"{label} typed I/O classifier marker is missing: {marker}",
                )

    if (
        'materialization_admission_no_response_code = "no-response"'
        not in normalized_error_header
        or (
            "if (!is_materialization_actual_io_or_hash_failure(failure)) "
            "return materialization_admission_no_response();"
        )
        not in normalized_error_header
        or (
            "return materialization_admission_spool_failure("
            "std::move(phase), std::move(detail));"
        )
        not in normalized_error_header
        or "if (is_materialization_admission_no_response(error)) return error;"
        not in normalized_error_header
        or (
            'if (error.code != "materialization.spool-failure") return '
            "materialization_admission_no_response();"
        )
        not in normalized_error_header
    ):
        fail(
            "materialization.request-invalid",
            "source-private no-response/spool normalization marker differs",
        )

    admission_texts = {
        "stream": normalized_stream,
        "v2.1": normalized_admission,
        "identity": normalized_identity,
        "task spool": normalized_task_spool,
        "admission error": normalized_error_header,
        "request driver": normalized_driver,
    }
    for label, source in admission_texts.items():
        for forbidden_code in EXPECTED_ADMISSION_FAILURE[
            "forbidden_admission_codes"
        ]:
            if forbidden_code in source:
                fail(
                    "materialization.request-invalid",
                    f"{label} reintroduced forbidden admission code {forbidden_code}",
                )

    if (
        "catch (const std::bad_alloc&) { return sdk::unexpected("
        "materialization_admission_no_response()); }"
        not in normalized_stream
        or "catch (const std::bad_alloc&) { return sdk::unexpected("
        "materialization_admission_no_response()); }"
        not in normalized_admission
        or normalized_task_spool.count(
            "catch (const std::bad_alloc&) { poisoned_ = true; return "
            "sdk::unexpected(materialization_admission_no_response()); }"
        )
        < 6
        or 'spool_error("source", "allocation")' in normalized_task_spool
        or 'spool_error("task.v3", "allocation")' in normalized_task_spool
    ):
        fail(
            "materialization.request-invalid",
            "phase-opaque allocation failure escaped no-response taxonomy",
        )

    required_driver = (
        "return is_materialization_admission_no_response(error) ? 2 : fail(error);",
        "if (!spool) return 2;",
        "if (!observed) return 2;",
        (
            'if (!observed->complete) return fail({"materialization.request-invalid", '
            '"input-limit", "maximum-bytes"});'
        ),
    )
    for marker in required_driver:
        if marker not in normalized_driver:
            fail(
                "materialization.request-invalid",
                f"request driver no-response/input-limit marker is missing: {marker}",
            )


def validate_sqlite_effect_root_authority_text(
    design_text: str,
    adr_text: str,
) -> None:
    """Bind rooted VFS lifetime, destruction order, and closed file authority."""

    markers = (
        "named non-default VFS",
        "type-erased backend lifetime token",
        "backend lifetime bridge は installed public header に callable signature を追加せず",
        "source-private default-visibility access class",
        "catalog 対象の public C++ API または supported external ABI ではない",
        "`snapshot_store` の bridge-specific な唯一の friend type",
        "exactly one static `open_sqlite` member",
        "method 単体の visibility override を禁止",
        "required static/shared build-test",
        "`sqlite3_open_v2` が返した raw connection",
        "stack RAII guard",
        "SQLite connection close、VFS unregister、captured root dirfd close、SQLite library release",
        "最後の main-database close",
        "named `xOpen`",
        "anonymous `xOpen`",
        "`xShmMap` / `xShmUnmap`",
        "`xAccess` / `xDelete`",
        "suffix-only path、unknown suffix",
        "`/cxxlens-rooted-vfs-v1/` synthetic name",
        "leading `:` / `file:` reserved name",
        "`xDlOpen` / `xDlSym` / `xDlClose` / `xDlError`",
        "`xSetSystemCall` / `xGetSystemCall` / `xNextSystemCall`",
        "named `SQLITE_OPEN_DELETEONCLOSE`",
        "anonymous pathname-unreachable private memfd",
        "pending reservation、file create、noexcept active commit",
        "`sqlite3_file.pMethods = nullptr`",
        "C ABI callback は C++ exception を外へ漏らさず",
        "authority lease",
        "最後の close は新規 lease を原子的に拒否",
        "既に取得済みの lease は effect 完了後に drain",
        "`xRead`、`xWrite`、`xTruncate`",
        "`xClose` だけは無条件に owned resource を解放",
        "transactional rollback を一律には主張せず",
        "pMethods null / flags zero",
        "FIFO、device、directory",
        "parent directory は captured root dirfd 自身の duplicate、または captured root dirfd から `openat2` beneath で取得時に認証",
        "retained authenticated parent-directory capability 相対",
        "concurrent ancestor rename / mount relocation",
        "current namespace の captured-root path 下から到達可能であることは主張しない",
        "`SQLITE_FCNTL_HAS_MOVED`",
        "missing、rename、replacement、 再認証 failure を moved",
    )
    documents = {
        "integrated design": " ".join(design_text.split()),
        "ADR": " ".join(adr_text.split()),
    }
    for label, value in documents.items():
        for marker in markers:
            if marker not in value:
                fail(
                    "materialization.sqlite-effect-root-invalid",
                    f"{label} rooted VFS authority marker is missing: {marker}",
                )


def validate_sqlite_effect_root_implementation_text(
    rooted_vfs_source: str,
    store_source: str,
    sqlite_lifecycle_header: str,
    sqlite_lifecycle_source: str,
) -> None:
    """Bind language-level teardown order and Store ownership to exact source structure."""

    def function_body(source: str, signature: str, search_from: int = 0) -> str:
        start = source.find(signature, search_from)
        if start < 0:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"rooted VFS implementation function is missing: {signature}",
            )
        parameter_opening = source.find("(", start)
        opening = -1
        if parameter_opening >= 0:
            parameter_depth = 0
            for index in range(parameter_opening, len(source)):
                if source[index] == "(":
                    parameter_depth += 1
                elif source[index] == ")":
                    parameter_depth -= 1
                    if parameter_depth == 0:
                        opening = source.find("{", index + 1)
                        break
        if opening < 0:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"rooted VFS implementation body is missing: {signature}",
            )
        depth = 0
        for index in range(opening, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return source[start : index + 1]
        fail(
            "materialization.sqlite-effect-root-invalid",
            f"rooted VFS implementation body is unterminated: {signature}",
        )

    def require_order(body: str, markers: tuple[str, ...], label: str) -> None:
        position = -1
        for marker in markers:
            observed = body.find(marker, position + 1)
            if observed < 0:
                fail(
                    "materialization.sqlite-effect-root-invalid",
                    f"{label} marker is missing or out of order: {marker}",
                )
            position = observed

    def normalized_body_contents(body: str) -> str:
        opening = body.find("{")
        closing = body.rfind("}")
        if opening < 0 or closing <= opening:
            fail(
                "materialization.sqlite-effect-root-invalid",
                "rooted VFS implementation body boundary is missing",
            )
        return "".join(body[opening + 1 : closing].split())

    def case_body(body: str, marker: str, next_marker: str, label: str) -> str:
        start = body.find(marker)
        end = body.find(next_marker, start + len(marker)) if start >= 0 else -1
        if start < 0 or end < 0:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"{label} case boundary is missing",
            )
        return body[start:end]

    def braced_block(
        source: str, marker: str, search_from: int, label: str
    ) -> tuple[str, int]:
        start = source.find(marker, search_from)
        opening = source.find("{", start) if start >= 0 else -1
        if start < 0 or opening < 0:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"{label} block is missing: {marker}",
            )
        depth = 0
        for index in range(opening, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return source[start : index + 1], index + 1
        fail(
            "materialization.sqlite-effect-root-invalid",
            f"{label} block is unterminated: {marker}",
        )

    def require_catch_contract(
        body: str,
        expected_bodies: tuple[tuple[str, str], ...],
        label: str,
    ) -> None:
        opening = body.find("{")
        if opening < 0:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"{label} typed exception cases differ",
            )
        depth = 0
        top_level_catch_count = 0
        for index in range(opening, len(body)):
            if body[index] == "{":
                depth += 1
            elif body[index] == "}":
                depth -= 1
            elif depth == 1 and body.startswith("catch (", index):
                top_level_catch_count += 1
        if top_level_catch_count != len(expected_bodies):
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"{label} typed exception cases differ",
            )
        cursor = 0
        for marker, expected in expected_bodies:
            block, cursor = braced_block(body, marker, cursor, label)
            if normalized_body_contents(block) != expected:
                fail(
                    "materialization.sqlite-effect-root-invalid",
                    f"{label} typed exception result differs: {marker}",
                )

    library_member = "sqlite_library_handle sqlite_library;"
    root_member = "materialization_owned_fd root;"
    if library_member not in rooted_vfs_source or root_member not in rooted_vfs_source:
        fail(
            "materialization.sqlite-effect-root-invalid",
            "rooted VFS lifetime owner members are missing",
        )
    if rooted_vfs_source.index(library_member) > rooted_vfs_source.index(root_member):
        fail(
            "materialization.sqlite-effect-root-invalid",
            "captured root is not destroyed before the SQLite library owner",
        )
    for marker in (
        "~sqlite_library_handle()",
        "(void)::dlclose(value);",
        "~rooted_sqlite_vfs()",
        "api.unregister_vfs(&wrapper)",
    ):
        if marker not in rooted_vfs_source:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"rooted VFS teardown marker is missing: {marker}",
            )
    destructor_start = rooted_vfs_source.index("~rooted_sqlite_vfs()")
    destructor_end = rooted_vfs_source.find("\n\t\t};", destructor_start)
    if destructor_end < 0:
        fail(
            "materialization.sqlite-effect-root-invalid",
            "rooted VFS destructor boundary is missing",
        )
    if "::dlclose" in rooted_vfs_source[destructor_start:destructor_end]:
        fail(
            "materialization.sqlite-effect-root-invalid",
            "rooted VFS releases SQLite before captured-root member destruction",
        )

    backend_member = "std::shared_ptr<void> backend_lifetime;"
    database_member = "std::unique_ptr<sqlite_database> database;"
    store_implementation, _ = braced_block(
        store_source,
        "struct snapshot_store::implementation",
        0,
        "Store implementation",
    )
    if (
        backend_member not in store_implementation
        or database_member not in store_implementation
    ):
        fail(
            "materialization.sqlite-effect-root-invalid",
            "Store backend/database lifetime members are missing",
        )
    if store_implementation.index(backend_member) > store_implementation.index(
        database_member
    ):
        fail(
            "materialization.sqlite-effect-root-invalid",
            "Store backend token would be destroyed before its SQLite connection",
        )

    if "bool delete_on_close" in rooted_vfs_source or "file->delete_on_close" in rooted_vfs_source:
        fail(
            "materialization.sqlite-effect-root-invalid",
            "rooted VFS retained a named delayed-delete capability",
        )

    io_table = rooted_vfs_source.index("const sqlite3_io_methods rooted_io_methods")
    io_callbacks = (
        "rooted_close",
        "rooted_read",
        "rooted_write",
        "rooted_truncate",
        "rooted_sync",
        "rooted_file_size",
        "rooted_lock",
        "rooted_unlock",
        "rooted_reserved",
        "rooted_control",
        "rooted_sector",
        "rooted_characteristics",
        "rooted_shm_map",
        "rooted_shm_lock",
        "rooted_shm_barrier",
        "rooted_shm_unmap",
        "rooted_fetch",
        "rooted_unfetch",
    )
    io_bodies: dict[str, str] = {}
    for callback in io_callbacks:
        body = function_body(rooted_vfs_source, f"{callback}(", io_table)
        io_bodies[callback] = body
        if "noexcept" not in body[: body.index("{")]:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"SQLite IO callback is not a no-throw C ABI boundary: {callback}",
            )

    vfs_callbacks = (
        "rooted_vfs_open",
        "rooted_vfs_remove",
        "rooted_vfs_access",
        "rooted_vfs_full_path",
        "rooted_dl_open",
        "rooted_dl_error",
        "rooted_dl_sym",
        "rooted_dl_close",
        "rooted_randomness",
        "rooted_sleep",
        "rooted_current_time",
        "rooted_last_error",
        "rooted_current_time_int64",
        "rooted_set_system_call",
        "rooted_get_system_call",
        "rooted_next_system_call",
    )
    vfs_bodies: dict[str, str] = {}
    for callback in vfs_callbacks:
        body = function_body(rooted_vfs_source, f"{callback}(")
        vfs_bodies[callback] = body
        if "noexcept" not in body[: body.index("{")]:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"SQLite VFS callback is not a no-throw C ABI boundary: {callback}",
            )

    for callback, body in (*io_bodies.items(), *vfs_bodies.items()):
        if any(marker in body for marker in ("std::terminate(", "throw;", "throw ")):
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"SQLite callback terminates or rethrows across its C ABI boundary: {callback}",
            )

    for callback in (
        "rooted_dl_open",
        "rooted_dl_error",
        "rooted_dl_sym",
        "rooted_dl_close",
        "rooted_set_system_call",
        "rooted_get_system_call",
        "rooted_next_system_call",
    ):
        if "delegate" in vfs_bodies[callback]:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"rooted VFS delegated a closed loader/syscall capability: {callback}",
            )
    closed_callback_bodies = {
        "rooted_dl_open": "returnnullptr;",
        "rooted_dl_error": "if(output!=nullptr&&size>0)output[0]='\\0';",
        "rooted_dl_sym": "returnnullptr;",
        "rooted_dl_close": "",
        "rooted_set_system_call": "returnsqlite_not_found;",
        "rooted_get_system_call": "returnnullptr;",
        "rooted_next_system_call": "returnnullptr;",
    }
    for callback, expected in closed_callback_bodies.items():
        if normalized_body_contents(vfs_bodies[callback]) != expected:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"rooted VFS closed callback body differs: {callback}",
            )

    advisory_callback_bodies = {
        "rooted_sector": "return4096;",
        "rooted_characteristics": "return0;",
        "rooted_shm_barrier": (
            "std::atomic_thread_fence(std::memory_order_seq_cst);"
        ),
        "rooted_fetch": (
            "(void)base;if(output==nullptr)returnsqlite_io_error;"
            "*output=nullptr;returnsqlite_ok;"
        ),
        "rooted_unfetch": "returnsqlite_ok;",
    }
    for callback, expected in advisory_callback_bodies.items():
        if normalized_body_contents(io_bodies[callback]) != expected:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"rooted VFS no-effect advisory callback body differs: {callback}",
            )

    sqlite_flags_body = function_body(rooted_vfs_source, "sqlite_flags_to_open(")
    require_order(
        sqlite_flags_body,
        (
            "O_RDWR : O_RDONLY) | O_NONBLOCK",
            "output |= O_CREAT;",
            "output |= O_EXCL;",
            "return output;",
        ),
        "rooted named xOpen nonblocking flags",
    )

    rooted_parent_body = function_body(rooted_vfs_source, "rooted_parent(")
    require_order(
        rooted_parent_body,
        (
            "const auto separator = relative.rfind('/');",
            "leaf = relative;",
            "::fcntl(owner.root.get(), F_DUPFD_CLOEXEC, 0)",
            "leaf = relative.substr(separator + 1U);",
            "open_materialization_beneath(",
            "owner.root.get()",
            "relative.substr(0U, separator)",
            "O_RDONLY | O_DIRECTORY",
        ),
        "rooted parent capability acquisition beneath the captured root",
    )
    rooted_open_body = function_body(rooted_vfs_source, "rooted_open(")
    require_order(
        rooted_open_body,
        (
            "rooted_parent(owner, relative, leaf)",
            "if (!parent)",
            "open_materialization_beneath(",
            "parent->get()",
            "leaf",
            "flags",
            "creation_mode",
        ),
        "rooted open parent capability and leaf effect order",
    )

    database_path_reservation_body = function_body(
        rooted_vfs_source, "reserve_database_path("
    )
    require_catch_contract(
        database_path_reservation_body,
        (
            ("catch (const std::bad_alloc&)", "returnfalse;"),
            ("catch (const std::length_error&)", "returnfalse;"),
        ),
        "rooted database-path reservation",
    )

    file_effect_authority_body = function_body(
        rooted_vfs_source, "acquire_file_effect_authority("
    )
    require_catch_contract(
        file_effect_authority_body,
        (
            (
                "catch (const std::bad_alloc&)",
                "return{std::nullopt,sqlite_no_memory};",
            ),
            (
                "catch (const std::length_error&)",
                "return{std::nullopt,sqlite_no_memory};",
            ),
            ("catch (...)", "return{std::nullopt,sqlite_io_error};"),
        ),
        "rooted file-effect authority",
    )

    allocation_callback_catches = {
        "rooted_vfs_open": (
            (
                "catch (const std::bad_alloc&)",
                "if(database_path_reserved)"
                "cancel_database_path_reservation(*owner,relative);"
                "returnsqlite_no_memory;",
            ),
            (
                "catch (const std::length_error&)",
                "if(database_path_reserved)"
                "cancel_database_path_reservation(*owner,relative);"
                "returnsqlite_no_memory;",
            ),
            (
                "catch (...)",
                "if(database_path_reserved)"
                "cancel_database_path_reservation(*owner,relative);"
                "returnsqlite_cannot_open;",
            ),
        ),
        "rooted_vfs_remove": (
            ("catch (const std::bad_alloc&)", "returnsqlite_no_memory;"),
            ("catch (const std::length_error&)", "returnsqlite_no_memory;"),
            ("catch (...)", "returnsqlite_io_error;"),
        ),
        "rooted_vfs_access": (
            ("catch (const std::bad_alloc&)", "returnsqlite_no_memory;"),
            ("catch (const std::length_error&)", "returnsqlite_no_memory;"),
            ("catch (...)", "returnsqlite_io_error;"),
        ),
        "rooted_vfs_full_path": (
            ("catch (const std::bad_alloc&)", "returnsqlite_no_memory;"),
            ("catch (const std::length_error&)", "returnsqlite_no_memory;"),
            ("catch (...)", "returnsqlite_cannot_open;"),
        ),
        "rooted_shm_map": (
            ("catch (const std::bad_alloc&)", "returnsqlite_no_memory;"),
            ("catch (const std::length_error&)", "returnsqlite_no_memory;"),
            ("catch (...)", "returnsqlite_io_error;"),
        ),
        "rooted_shm_unmap": (
            ("catch (const std::bad_alloc&)", "returnsqlite_no_memory;"),
            ("catch (const std::length_error&)", "returnsqlite_no_memory;"),
            ("catch (...)", "returnsqlite_io_error;"),
        ),
    }
    for callback, expected in allocation_callback_catches.items():
        callback_body = vfs_bodies.get(callback) or io_bodies[callback]
        require_catch_contract(callback_body, expected, callback)

    open_body = vfs_bodies["rooted_vfs_open"]
    require_order(
        open_body,
        (
            "if (output_flags != nullptr)",
            "*output_flags = 0;",
            "if (output == nullptr)",
            "output->methods = nullptr;",
            "sqlite_open_delete_on_close",
            "reserve_database_path",
            "rooted_open(",
            "rooted_regular_file(opened->get())",
            "new (output) rooted_sqlite_file{}",
            "commit_database_path_reservation",
            "authenticated.release()",
            "*output_flags = flags;",
            "file->base.methods = &rooted_io_methods;",
        ),
        "rooted VFS xOpen preeffect/commit/publication order",
    )
    rooted_open_position = open_body.find("rooted_open(")
    commit_position = open_body.find(
        "commit_database_path_reservation", rooted_open_position
    )
    if rooted_open_position < 0 or commit_position < 0:
        fail(
            "materialization.sqlite-effect-root-invalid",
            "rooted VFS xOpen create/commit boundary is missing",
        )
    post_create = " ".join(
        open_body[rooted_open_position:commit_position].split()
    )
    for fallible_marker in (
        "std::string ",
        "std::vector<",
        "std::make_",
        ".reserve(",
        ".append(",
        ".assign(",
        ".emplace(",
        ".push_back(",
        ".resize(",
        "new std::",
        "operator new",
    ):
        if fallible_marker in post_create:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"rooted VFS xOpen retained fallible work after create: {fallible_marker}",
            )
    if open_body.count("cancel_database_path_reservation") < 4 or "catch (...)" not in open_body:
        fail(
            "materialization.sqlite-effect-root-invalid",
            "rooted VFS xOpen does not cancel every fallible pending reservation",
        )
    close_body = io_bodies["rooted_close"]
    if "rooted_remove" in close_body or "acquire_file_effect_authority" in close_body:
        fail(
            "materialization.sqlite-effect-root-invalid",
            "rooted xClose retained a path effect or required revoked authority",
        )
    for marker in (
        "release_shm_resources(*file)",
        "::close(file->authenticated_descriptor)",
        "file->~rooted_sqlite_file()",
    ):
        if marker not in close_body:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"rooted xClose cleanup marker is missing: {marker}",
            )

    acquire_body = function_body(rooted_vfs_source, "acquire_database_path_authority(")
    require_order(
        acquire_body,
        ("active_open_count == 0U", "++existing->authority_lease_count"),
        "rooted database authority acquisition",
    )
    for signature in (
        "deauthenticate_database_path(",
        "release_database_path_authority(",
    ):
        body = " ".join(function_body(rooted_vfs_source, signature).split())
        if not all(
            marker in body
            for marker in (
                "existing->active_open_count == 0U",
                "existing->pending_open_count == 0U",
                "existing->authority_lease_count == 0U",
                "owner.authenticated_database_paths.erase(existing)",
            )
        ):
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"rooted database authority drain is incomplete: {signature}",
            )

    effect_callbacks = {
        "rooted_read": "::pread(",
        "rooted_write": "::pwrite(",
        "rooted_truncate": "::ftruncate(",
        "rooted_sync": "::fdatasync(",
        "rooted_file_size": "::fstat(",
        "rooted_lock": "rooted_set_lock(",
        "rooted_unlock": "rooted_set_lock(",
        "rooted_reserved": "::fcntl(",
        "rooted_shm_lock": "::fcntl(",
    }
    for callback, effect in effect_callbacks.items():
        require_order(
            io_bodies[callback],
            ("acquire_file_effect_authority(*file)", effect),
            f"opened sidecar effect authority for {callback}",
        )
    control_body = io_bodies["rooted_control"]
    file_control_cases = (
        (
            "case sqlite_file_control_lock_state:",
            "case sqlite_file_control_last_errno:",
            "*static_cast<int*>(value) = file->lock_level;",
            "lock-state",
        ),
        (
            "case sqlite_file_control_last_errno:",
            "case sqlite_file_control_size_hint:",
            "*static_cast<int*>(value) = file->last_errno;",
            "last-errno",
        ),
        (
            "case sqlite_file_control_size_hint:",
            "case sqlite_file_control_powersafe_overwrite:",
            "const auto requested = *static_cast<const long long*>(value);",
            "size-hint",
        ),
        (
            "case sqlite_file_control_powersafe_overwrite:",
            "case sqlite_file_control_has_moved:",
            "auto& requested = *static_cast<int*>(value);",
            "powersafe-overwrite",
        ),
    )
    pre_authority_effect_markers = (
        "::ftruncate(",
        "::fcntl(",
        "::fstat(",
        "rooted_file_size(",
        "rooted_truncate(",
        "rooted_set_lock(",
        "file->lock_level",
        "file->last_errno",
        "file->powersafe_overwrite",
        "*static_cast",
    )
    for marker, next_marker, effect, label in file_control_cases:
        block = case_body(control_body, marker, next_marker, f"rooted file-control {label}")
        authority_marker = "acquire_file_effect_authority(*file)"
        authority_position = block.find(authority_marker)
        if authority_position < 0:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"opened sidecar file-control authority is missing: {label}",
            )
        before_authority = block[:authority_position]
        if any(effect_marker in before_authority for effect_marker in pre_authority_effect_markers):
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"opened sidecar file-control effect precedes authority: {label}",
            )
        require_order(
            block,
            (
                authority_marker,
                "authority.failure != sqlite_ok",
                "return authority.failure;",
                effect,
            ),
            f"opened sidecar file-control authority for {label}",
        )

    has_moved_body = case_body(
        control_body,
        "case sqlite_file_control_has_moved:",
        "default:",
        "rooted HAS_MOVED",
    )
    require_order(
        has_moved_body,
        (
            "moved = 1;",
            "try",
            "acquire_named_path_authority",
            "if (!authority)",
            "return sqlite_ok;",
            "rooted_open(",
            "::fstat(file->authenticated_descriptor",
            "::fstat(reopened->get()",
            "retained.st_dev == observed.st_dev",
            "retained.st_ino == observed.st_ino",
            "moved = 0;",
            "catch (...)",
            "return sqlite_ok;",
        ),
        "rooted HAS_MOVED conservative identity order",
    )
    no_authority = has_moved_body.find("if (!authority)")
    no_authority_return = has_moved_body.find("return sqlite_ok;", no_authority)
    if (
        no_authority < 0
        or no_authority_return < 0
        or "moved = 0;" in has_moved_body[no_authority:no_authority_return]
        or has_moved_body.count("moved = 0;") != 1
    ):
        fail(
            "materialization.sqlite-effect-root-invalid",
            "rooted HAS_MOVED reauthentication failure is not moved-one",
        )

    require_order(
        vfs_bodies["rooted_vfs_remove"],
        ("acquire_named_path_authority", "rooted_remove("),
        "rooted xDelete authority lease",
    )
    access_body = vfs_bodies["rooted_vfs_access"]
    if access_body.count("O_NONBLOCK") != 2:
        fail(
            "materialization.sqlite-effect-root-invalid",
            "rooted xAccess named branches are not nonblocking",
        )
    require_order(
        access_body,
        (
            "*output = 0;",
            "acquire_named_path_authority",
            "rooted_open(",
            "rooted_regular_file(opened->get())",
        ),
        "rooted xAccess authority and output order",
    )
    remove_body = function_body(rooted_vfs_source, "rooted_remove(")
    require_order(
        remove_body,
        (
            "rooted_parent(owner, relative, leaf)",
            "open_materialization_beneath(",
            "parent->get()",
            "leaf",
            "identity_open_flags",
            "::fstat(authenticated->get()",
            "::fstatat(parent->get()",
            "::unlinkat(parent->get()",
        ),
        "rooted xDelete parent capability and leaf effect order",
    )
    fstat_position = remove_body.find("::fstat(")
    fstatat_position = remove_body.find("::fstatat(", fstat_position)
    unlink_position = remove_body.find("::unlinkat(", fstatat_position)
    if min(fstat_position, fstatat_position, unlink_position) < 0 or not (
        fstat_position < fstatat_position < unlink_position
    ) or remove_body.count("S_ISREG") < 2:
        fail(
            "materialization.sqlite-effect-root-invalid",
            "rooted xDelete lacks its immediate regular-file observation",
        )

    for callback, output_marker in (
        ("rooted_vfs_full_path", "output[0] = '\\0';"),
        ("rooted_shm_map", "*output = nullptr;"),
        ("rooted_fetch", "*output = nullptr;"),
    ):
        if output_marker not in (
            vfs_bodies.get(callback) or io_bodies.get(callback) or ""
        ):
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"rooted callback failure output is not initialized: {callback}",
            )
    for callback in ("rooted_vfs_open", "rooted_vfs_remove", "rooted_vfs_access", "rooted_vfs_full_path"):
        if "catch (...)" not in vfs_bodies[callback]:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"allocation-capable VFS callback lacks catch-all containment: {callback}",
            )
    for callback in ("rooted_shm_map", "rooted_shm_unmap"):
        if "catch (...)" not in io_bodies[callback]:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"allocation-capable IO callback lacks catch-all containment: {callback}",
            )

    relative_body = function_body(rooted_vfs_source, "rooted_relative_name(")
    require_order(
        relative_body,
        ("!value.starts_with(rooted_name_prefix)", "value.remove_prefix(rooted_name_prefix.size())"),
        "rooted synthetic filename namespace",
    )
    if 'value.front() == \':\'' not in rooted_vfs_source or 'value.starts_with("file:")' not in rooted_vfs_source:
        fail(
            "materialization.sqlite-effect-root-invalid",
            "reserved SQLite filename rejection is missing",
        )

    opener_body = function_body(
        rooted_vfs_source, "materialization_rooted_store_opener::open_sqlite("
    )
    require_order(
        opener_body,
        (
            "rooted_sqlite_path.reserve",
            "prepared_exact_path.emplace",
            "prepared_receipt.emplace",
            "snapshot_store_backend_lifetime_access::open_sqlite(",
            "static_assert",
            "state_->exact_sqlite_path =",
            "state_->receipt =",
        ),
        "rooted Store opener preallocation and no-throw publication",
    )
    bridge_start = opener_body.index("snapshot_store_backend_lifetime_access::open_sqlite(")
    bridge_end = opener_body.index(");", bridge_start)
    if "rooted_sqlite_path" not in opener_body[bridge_start:bridge_end] or (
        "exact_path," in opener_body[bridge_start:bridge_end]
    ):
        fail(
            "materialization.sqlite-effect-root-invalid",
            "rooted Store opener passed an untrusted raw SQLite filename",
        )

    load_body = function_body(store_source, "load_sqlite()")
    require_order(
        load_body,
        ("sqlite_library_guard library_guard", "std::make_shared<sqlite_api>()", "library_guard.release()"),
        "SQLite library RAII ownership",
    )
    database_open_body = function_body(store_source, "open_database(")
    require_order(
        database_open_body,
        (
            "sqlite_connection_lifecycle connection{nullptr, api->close, std::move(pins)};",
            "auto** database_slot = connection.open_handle_out_parameter();",
            "if (database_slot == nullptr)",
            "const auto open_result",
            "if (open_result != 0)",
            "connection.get() != nullptr",
            "std::make_unique<sqlite_database>(std::move(api), std::move(connection))",
        ),
        "SQLite raw connection RAII ownership",
    )
    if database_open_body.count("api->open(") != 1:
        fail(
            "materialization.sqlite-effect-root-invalid",
            "SQLite raw connection open is not adopted exactly once",
        )

    sqlite_database_body, _ = braced_block(
        store_source, "class sqlite_database", 0, "SQLite database owner"
    )
    require_order(
        sqlite_database_body,
        (
            "sqlite_database(std::shared_ptr<sqlite_api> api, "
            "sqlite_connection_lifecycle connection)",
            "connection_{std::move(connection)}",
            "sqlite_connection_lifecycle connection_;",
        ),
        "SQLite lifecycle ownership transfer",
    )

    lifecycle_header_markers = (
        "~sqlite_connection_lifecycle() noexcept;",
        "sqlite_connection_lifecycle(sqlite_connection_lifecycle&& other) noexcept;",
        "sqlite_connection_lifecycle& operator=(sqlite_connection_lifecycle&& other) noexcept;",
        "sqlite_connection_lifecycle(const sqlite_connection_lifecycle&) = delete;",
        "sqlite_connection_lifecycle& operator=(const sqlite_connection_lifecycle&) = delete;",
        "void** open_handle_out_parameter() noexcept;",
        "sqlite_connection_close_outcome close_exactly_once() noexcept;",
    )
    for marker in lifecycle_header_markers:
        if marker not in sqlite_lifecycle_header:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"SQLite connection lifecycle contract marker is missing: {marker}",
            )

    open_slot_body = function_body(
        sqlite_lifecycle_source,
        "sqlite_connection_lifecycle::open_handle_out_parameter()",
    )
    if normalized_body_contents(open_slot_body) != (
        "returnstate_!=nullptr&&state_->connection==nullptr?"
        "&state_->connection:nullptr;"
    ):
        fail(
            "materialization.sqlite-effect-root-invalid",
            "SQLite open result slot is not adopted by the empty lifecycle owner",
        )

    close_body = function_body(
        sqlite_lifecycle_source,
        "sqlite_connection_lifecycle::close_exactly_once()",
    )
    require_order(
        close_body,
        (
            "auto owned = std::move(state_);",
            "if (owned == nullptr || owned->connection == nullptr)",
            "if (owned->close_v2 == nullptr)",
            "try",
            "const auto code = owned->close_v2(owned->connection);",
            "if (code == sqlite_ok)",
            "catch (...)",
        ),
        "SQLite exact-once close lifecycle",
    )
    if (
        close_body.count("std::move(state_)") != 1
        or close_body.count("owned->close_v2(owned->connection)") != 1
    ):
        fail(
            "materialization.sqlite-effect-root-invalid",
            "SQLite lifecycle does not consume ownership and close at most once",
        )

    cleanup_body = function_body(
        sqlite_lifecycle_source,
        "sqlite_connection_lifecycle::cleanup_noexcept()",
    )
    require_order(
        cleanup_body,
        ("if (state_ != nullptr)", "(void)close_exactly_once();"),
        "SQLite noexcept lifecycle cleanup",
    )
    destructor_body = function_body(
        sqlite_lifecycle_source,
        "sqlite_connection_lifecycle::~sqlite_connection_lifecycle()",
    )
    if normalized_body_contents(destructor_body) != "cleanup_noexcept();":
        fail(
            "materialization.sqlite-effect-root-invalid",
            "SQLite lifecycle destructor does not use exact-once cleanup",
        )

    move_assignment_body = function_body(
        sqlite_lifecycle_source,
        "sqlite_connection_lifecycle::operator=(sqlite_connection_lifecycle&& other)",
    )
    require_order(
        move_assignment_body,
        ("cleanup_noexcept();", "state_ = std::move(other.state_);"),
        "SQLite lifecycle move assignment",
    )


def validate_store_backend_lifetime_bridge_text(
    public_store_header: str,
    internal_store_header: str,
    rooted_vfs_source: str,
    store_source: str,
) -> None:
    """Keep the rooted SQLite lifetime bridge out of the installed callable surface."""

    def compound_body(source: str, signature: str, label: str) -> str:
        start = source.find(signature)
        if start < 0:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"{label} declaration is missing",
            )
        opening = source.find("{", start)
        if opening < 0:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"{label} body is missing",
            )
        depth = 0
        for index in range(opening, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return source[opening : index + 1]
        fail(
            "materialization.sqlite-effect-root-invalid",
            f"{label} body is unterminated",
        )

    legacy_callable = "open_sqlite_snapshot_store_with_backend_lifetime_internal"
    if any(
        legacy_callable in document
        for document in (
            public_store_header,
            internal_store_header,
            rooted_vfs_source,
            store_source,
        )
    ):
        fail(
            "materialization.sqlite-effect-root-invalid",
            "Store backend lifetime bridge leaked a namespace callable",
        )
    friend_marker = "friend struct snapshot_store_backend_lifetime_access;"
    snapshot_store_body = compound_body(
        public_store_header, "class snapshot_store\n", "installed snapshot_store"
    )
    friend_type_declarations = [
        " ".join(declaration.split())
        for declaration in re.findall(
            r"\bfriend\b[^;]*;", snapshot_store_body, flags=re.DOTALL
        )
        if "(" not in declaration
    ]
    if (
        snapshot_store_body.count(friend_marker) != 1
        or friend_type_declarations != [friend_marker]
        or "backend_lifetime" in public_store_header.replace(friend_marker, "")
    ):
        fail(
            "materialization.sqlite-effect-root-invalid",
            "installed Store header violates the exact bridge-specific friend boundary",
        )
    for marker in (
        '__attribute__((visibility("default"))) snapshot_store_backend_lifetime_access',
        "struct snapshot_store_backend_lifetime_access",
        "open_sqlite(const std::string& database_path",
        "sqlite_backend_runtime_binding runtime",
        "std::shared_ptr<void> backend_lifetime",
        "std::shared_ptr<sqlite_backend_observation_capability> observation",
    ):
        if marker not in internal_store_header:
            fail(
                "materialization.sqlite-effect-root-invalid",
                f"source-private Store bridge marker is missing: {marker}",
            )
    bridge_body = compound_body(
        internal_store_header,
        "struct snapshot_store_backend_lifetime_access\n#endif",
        "source-private Store bridge",
    )
    exact_bridge_body = (
        "{ [[nodiscard]] static result<snapshot_store> "
        "open_sqlite(const std::string& database_path, relation_engine engine, "
        "const std::string& vfs_name, sqlite_backend_runtime_binding runtime, "
        "std::shared_ptr<void> backend_lifetime, "
        "std::shared_ptr<sqlite_backend_observation_capability> observation); }"
    )
    if " ".join(bridge_body.split()) != exact_bridge_body:
        fail(
            "materialization.sqlite-effect-root-invalid",
            "source-private Store bridge violates exact-one-member or visibility authority",
        )
    definition = "snapshot_store_backend_lifetime_access::open_sqlite("
    call = "sdk::snapshot_store_backend_lifetime_access::open_sqlite("
    internal_call = "return snapshot_store_backend_lifetime_access::open_sqlite("
    if (
        store_source.count(definition) != 3
        or store_source.count(internal_call) != 2
        or rooted_vfs_source.count(call) != 1
    ):
        fail(
            "materialization.sqlite-effect-root-invalid",
            "source-private Store bridge definition/call binding differs",
        )
    definition_position = store_source.rindex(definition)
    definition_opening = store_source.find("{", definition_position)
    definition_region_start = store_source.rfind("}", 0, definition_position) + 1
    exact_definition_signature = (
        "result<snapshot_store> "
        "snapshot_store_backend_lifetime_access::open_sqlite( "
        "const std::string& database_path, relation_engine engine, "
        "const std::string& vfs_name, sqlite_backend_runtime_binding runtime, "
        "std::shared_ptr<void> backend_lifetime, "
        "std::shared_ptr<sqlite_backend_observation_capability> observation)"
    )
    if (
        definition_opening < 0
        or " ".join(
            store_source[definition_region_start:definition_opening].split()
        )
        != exact_definition_signature
    ):
        fail(
            "materialization.sqlite-effect-root-invalid",
            "source-private Store bridge definition violates exact visibility authority",
        )


def validate_occurrence_build_provenance(
    root_cmake: str,
    occurrence_generator: str,
    source_verifier: str,
) -> None:
    """Bind occurrence source IDs to explicit input or a clean stable checkout."""

    required = {
        "root CMake": (
            "must be supplied together",
            "set(CXXLENS_SOURCE_PROVENANCE_MODE git-clean)",
            "cmake/VerifyClang22SourceProvenance.cmake",
        ),
        "occurrence generator": (
            'set(CXXLENS_PROVENANCE_MODE "@CXXLENS_SOURCE_PROVENANCE_MODE@")',
            'set(CXXLENS_PROVENANCE_EXPECTED_REVISION "@CXXLENS_SOURCE_REVISION@")',
            'set(CXXLENS_PROVENANCE_EXPECTED_TREE "@CXXLENS_SOURCE_TREE@")',
            'include("@CMAKE_CURRENT_BINARY_DIR@/VerifyClang22SourceProvenance.cmake")',
        ),
        "source provenance verifier": (
            'CXXLENS_PROVENANCE_MODE STREQUAL "explicit"',
            'CXXLENS_PROVENANCE_MODE STREQUAL "git-clean"',
            "status --porcelain=v1",
            "--untracked-files=all",
            "_cxxlens_git_observe(before)",
            "_cxxlens_git_observe(after)",
            "CXXLENS_PROVENANCE_EXPECTED_REVISION",
            "CXXLENS_PROVENANCE_EXPECTED_TREE",
            "dirty, stale, or unstable",
        ),
    }
    documents = {
        "root CMake": root_cmake,
        "occurrence generator": occurrence_generator,
        "source provenance verifier": source_verifier,
    }
    for label, markers in required.items():
        for marker in markers:
            if marker not in documents[label]:
                fail(
                    "materialization.occurrence-invalid",
                    f"{label} source-provenance guard is missing: {marker}",
                )


def validate_documents(root: pathlib.Path) -> dict[str, Any]:
    contract_text = (root / CONTRACT).read_text(encoding="utf-8")
    design_text = (root / INTEGRATED_DESIGN).read_text(encoding="utf-8")
    adr_text = (root / DECISION_ADR).read_text(encoding="utf-8")
    validate_report_lifecycle_authority_text(
        design_text,
        adr_text,
        contract_text,
    )
    validate_base64_authority_text(design_text, adr_text)
    validate_occurrence_snapshot_authority_text(design_text, adr_text)
    validate_private_spool_authority_text(design_text, adr_text)
    validate_materialization_io_taxonomy_header(
        (root / MATERIALIZATION_IO_HEADER).read_text(encoding="utf-8")
    )
    validate_private_spool_implementation_text(
        (root / MATERIALIZATION_IO_SOURCE).read_text(encoding="utf-8")
    )
    validate_v2_1_admission_authority_text(design_text, adr_text)
    validate_v2_1_admission_implementation_text(
        (root / MATERIALIZATION_REQUEST_STREAM_HEADER).read_text(encoding="utf-8"),
        (root / MATERIALIZATION_REQUEST_STREAM_SOURCE).read_text(encoding="utf-8"),
        (root / MATERIALIZATION_REQUEST_V2_1_SOURCE).read_text(encoding="utf-8"),
        (root / MATERIALIZATION_ADMISSION_ERROR_HEADER).read_text(encoding="utf-8"),
        (root / MATERIALIZATION_REQUEST_IDENTITY_SOURCE).read_text(encoding="utf-8"),
        (root / MATERIALIZATION_TASK_SPOOL_SOURCE).read_text(encoding="utf-8"),
        (root / MATERIALIZATION_REQUEST_DRIVER).read_text(encoding="utf-8"),
    )
    validate_sqlite_effect_root_authority_text(design_text, adr_text)
    validate_sqlite_effect_root_implementation_text(
        (root / ROOTED_VFS_SOURCE).read_text(encoding="utf-8"),
        (root / STORE_SOURCE).read_text(encoding="utf-8"),
        (root / SQLITE_CONNECTION_LIFECYCLE_INTERNAL).read_text(encoding="utf-8"),
        (root / SQLITE_CONNECTION_LIFECYCLE_SOURCE).read_text(encoding="utf-8"),
    )
    validate_store_backend_lifetime_bridge_text(
        (root / STORE_HEADER).read_text(encoding="utf-8"),
        (root / STORE_BACKEND_LIFETIME_INTERNAL).read_text(encoding="utf-8"),
        (root / ROOTED_VFS_SOURCE).read_text(encoding="utf-8"),
        (root / STORE_SOURCE).read_text(encoding="utf-8"),
    )
    validate_occurrence_build_provenance(
        (root / ROOT_CMAKE).read_text(encoding="utf-8"),
        (root / OCCURRENCE_GENERATOR_CMAKE).read_text(encoding="utf-8"),
        (root / SOURCE_PROVENANCE_CMAKE).read_text(encoding="utf-8"),
    )
    contract = load(root / CONTRACT)
    contract_schema = load(root / CONTRACT_SCHEMA)
    request_schema = load(root / REQUEST_SCHEMA)
    report_schema = load(root / REPORT_SCHEMA)
    occurrence_schema = load(root / OCCURRENCE_SCHEMA)
    snapshot_store_contract = load(root / SNAPSHOT_STORE)
    for label, schema in (
        ("contract", contract_schema),
        ("request", request_schema),
        ("report", report_schema),
        ("materializer occurrence manifest", occurrence_schema),
    ):
        try:
            jsonschema.Draft202012Validator.check_schema(schema)
        except jsonschema.SchemaError as error:
            fail(
                "materialization.request-invalid",
                f"materialization {label} schema: {error.message}",
            )
    validate_schema(contract, contract_schema, "materialization contract")
    validate_occurrence_manifest(
        root,
        fixture_occurrence_manifest(
            root,
            source_revision="1" * 40,
            source_tree="2" * 40,
            configuration="static",
            tool_digest="sha256:" + "1" * 64,
            worker_digest="sha256:" + "1" * 64,
        ),
    )
    validate_contract_exact(
        contract,
        request_schema,
        report_schema,
        contract_schema,
        snapshot_store_contract,
    )
    validate_df_0200_claim_batch_corpus(
        root,
        contract["claim_adoption"]["df_0200_resolution"][
            "d1_claim_batch_oracle"
        ]["qualification_corpus"],
    )
    validate_project_catalog_authority(root)
    validate_materialization_dependency_graph(
        contract,
        materialization_dependency_documents(root),
    )
    validate_span_registry_columns(
        load(root / REGISTRY),
        contract["span_adoption"]["registry_column_binding"],
    )
    report_errors = set(
        report_schema["$defs"]["error"]["properties"]["code"]["enum"]
    )
    if report_errors != STABLE_ERRORS:
        fail("materialization.report-invalid", "report schema error registry differs")
    entries: list[tuple[dict[str, Any], dict[str, Any]]] = []
    for configuration in ("static", "shared"):
        for backend in ("memory", "sqlite"):
            request = sample_request(root, configuration=configuration, backend=backend)
            validate_request(root, request)
            report = sample_report(root, request)
            validate_report(
                root,
                request,
                report,
                request_bytes=canonical_json(request),
            )
            entries.append((request, report))
    validate_qualification_matrix(root, entries)
    stale_request = sample_request(root, configuration="static", backend="sqlite")
    validate_report(
        root,
        stale_request,
        stale_parent_report(root, stale_request),
        request_bytes=canonical_json(stale_request),
    )
    return contract


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    arguments = parser.parse_args()
    try:
        validate_documents(arguments.root.resolve())
    except MaterializationError as error:
        print(f"Clang 22 materialization contract check failed: {error}", file=sys.stderr)
        return 1
    print("Clang 22 materialization contract check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
