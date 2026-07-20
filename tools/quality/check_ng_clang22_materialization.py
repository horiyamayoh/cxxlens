#!/usr/bin/env python3
"""Validate the installed Clang 22 materialization machine contract."""

from __future__ import annotations

import argparse
import base64
import copy
import datetime
import decimal
import functools
import hashlib
import json
import pathlib
import sys
import unicodedata
from typing import Any, Iterable

import jsonschema
import yaml


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
REGISTRY = pathlib.Path("schemas/cxxlens_ng_relation_registry.yaml")
PROJECT_CATALOG = pathlib.Path("schemas/cxxlens_ng_project_catalog_contract.yaml")
PORTABLE_PROVIDER_TASK = pathlib.Path(
    "schemas/cxxlens_ng_portable_provider_task_contract.yaml"
)
PROVIDER_PROTOCOL = pathlib.Path("schemas/cxxlens_ng_provider_protocol.yaml")
PROVIDER_RUNTIME = pathlib.Path("schemas/cxxlens_ng_provider_runtime_contract.yaml")
SNAPSHOT_STORE = pathlib.Path("schemas/cxxlens_ng_snapshot_store_contract.yaml")
DECISION_ADR = pathlib.Path(
    "docs/design/adr/0096-clang22-installed-materialization-boundary.md"
)
INTEGRATED_DESIGN = pathlib.Path(
    "docs/design/cxxlens_next_generation_integrated_design_ja.md"
)
GENERIC_DEPENDENCIES = [
    REGISTRY,
    PROJECT_CATALOG,
    PORTABLE_PROVIDER_TASK,
    PROVIDER_PROTOCOL,
    PROVIDER_RUNTIME,
    SNAPSHOT_STORE,
]
AUTHORITY_PATHS = [CONTRACT, CONTRACT_SCHEMA, REQUEST_SCHEMA, REPORT_SCHEMA, REGISTRY]
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
    "validation": "recompute-before-source-file-and-task-adoption",
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
    },
    "global_side_channels": {
        "domains": [
            "cxxlens.clang22-global-coverage.v1",
            "cxxlens.clang22-global-unresolved.v1",
            "cxxlens.clang22-global-evidence.v1",
        ],
        "task_order": "semantic-task-key-byte-order",
        "physical_provider_execution": "excluded",
    },
    "guarantee": {
        "domain": "cxxlens.clang22-materialization-guarantee.v1",
        "inputs": [
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
        },
        "exact_compare": [
            "selector",
            "snapshot-manifest",
            "eight-field-partition-bindings",
            "rows",
            "claim-annotation-multiset",
            "coverage-multiset",
            "unresolved",
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


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def content_digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


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
        value.encode("utf-8")
    except UnicodeEncodeError:
        return False
    return all(
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

    if not isinstance(working_directory, str) or not working_directory:
        fail("materialization.request-invalid", "working directory is invalid")
    if (
        not isinstance(effective_argv, list)
        or not effective_argv
        or any(not isinstance(argument, str) or not argument for argument in effective_argv)
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
    return _canonical_tuple(
        (
            _canonical_string("cxxlens.clang22.task.v3"),
            _canonical_projection_value(global_catalog),
            _canonical_string(task["selected_catalog_compile_unit_id"]),
            _canonical_string(task["compile_unit_id"]),
            _canonical_projection_value(per_tu_payload),
        )
    )


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


def validate_contract_exact(contract: dict[str, Any]) -> None:
    validate_materialization_contract_dependencies(contract)

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
        "json_lexical_policy": EXPECTED_JSON_LEXICAL_POLICY,
        "json_lexical_errors": {
            "request": "materialization.request-invalid",
            "report": "materialization.report-invalid",
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
            "maximum_decoded_source_bytes_per_task": 16777216,
            "maximum_aggregate_decoded_source_bytes": 536870912,
            "maximum_content_base64_characters_per_task": 22369624,
            "maximum_response_bytes": RAW_INPUT_BYTE_LIMIT,
            "raw_request_storage": "bounded-chunk-read-and-private-spool",
            "json_processing": "streaming-strict-utf8-duplicate-aware-no-one-gib-dom",
            "source_decoding": "streaming-base64-to-bounded-private-spool",
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
        "machine_contract": "2.0.0",
        "request": "2.0.0",
        "report": "2.0.0",
        "provider_task_input_codec": "cxxlens.clang22.task.v3",
        "observation_native_codec": "cxxlens.clang22.observation-native.v2",
        "provider_protocol": "1.0.0",
        "unknown_member": "reject",
        "missing_required_member": "reject",
        "adjacent_version_fallback": "forbidden",
        "migration": "v1-unimplemented-unqualified-superseded-no-implicit-upgrade",
    }:
        fail("materialization.version-unsupported", "version/fallback policy is not exact")
    compact_report_construction = contract["report"]["response_union"][
        "compact_failure"
    ].get("report_construction_phase")
    if (
        compact_report_construction
        != "prepublication-zero-effect-only-before-publish-call"
    ):
        fail(
            "materialization.report-invalid",
            "compact report-construction boundary differs",
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
    if contract["report"].get("digest_chain") != EXPECTED_REPORT_DIGEST_CHAIN:
        fail("materialization.report-invalid", "report digest chain differs")
    adoption = contract["claim_adoption"]
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
        "exact_preconditions": [
            "zero-non-exact-in-each-observation-descriptor-census",
            "complete-balanced-coverage",
            "no-blocking-unresolved",
            "no-absent-primary-span-bundle",
            "exact-task-census",
            "exact-six-batches",
            "full-span-validation",
            "complete-provenance",
            "successful-publication",
        ],
        "non_exact": (
            "preserve-typed-approximation-assumptions-modalities-and-unresolved"
        ),
        "inference_from-success-or-prose": "forbidden",
    }:
        fail("materialization.coverage-incomplete", "guarantee preconditions differ")
    if contract["side_channels"]["guarantee"] != {
        "record_type": "typed-guarantee",
        "fields": [
            "approximation",
            "scope",
            "assumptions",
            "verification_modalities",
            "observation_descriptor_censuses",
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
    if contract["errors"]["diagnostic_prose_control_flow"] != "forbidden":
        fail("materialization.report-invalid", "diagnostic prose became control authority")
    if contract["lifetime"]["raw_frames"] != "diagnostic-only-and-destroyed-before-process-exit":
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
    request = {
        "schema": "cxxlens.clang22-materialization-request.v2",
        "request_version": "2.0.0",
        "materialization_request_id": "pending",
        "request_digest": semantic_digest("cxxlens.fixture.v1", "pending-request"),
        "semantic_request_digest": semantic_digest("cxxlens.fixture.v1", "pending-semantic"),
        "tool": {
            "executable": "cxxlens-clang22-materialize",
            "interface_version": "2.0.0",
            "distribution_version": "1.0.0",
            "source_revision": "1" * 40,
            "source_tree": "2" * 40,
            "installed_executable_digest": "sha256:" + digit * 64,
            "package_configuration": configuration,
            "prefix_manifest_digest": "sha256:" + digit * 64,
            "relocated_prefix_digest": "sha256:" + digit * 64,
        },
        "worker": {
            "executable": "cxxlens-clang-worker-22",
            "provider_id": "cxxlens.clang22.reference",
            "provider_version": "1.0.0",
            "installed_binary_digest": "sha256:" + digit * 64,
            "semantic_contract_digest": "sha256:" + "a" * 64,
            "protocol_major": 1,
            "protocol_minor": 0,
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
        source = base64.b64decode(task["source"]["content_base64"], validate=True)
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
    validate_schema(request, load(root / REQUEST_SCHEMA), "materialization request")
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
        try:
            source = base64.b64decode(task["source"]["content_base64"], validate=True)
        except ValueError as error:
            fail("materialization.request-invalid", f"invalid source base64: {error}")
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
    canonical_form = detached_row_canonical_form(relation, row)
    row_digest = content_digest(canonical_form.encode("utf-8"))
    cache_key = (descriptor_id, row_digest)
    previous = _FIXTURE_WORKER_ROW_CANONICAL.setdefault(cache_key, canonical_form)
    if previous != canonical_form:
        fail("materialization.claim-invalid", "fixture row digest collision")
    return row_digest, canonical_form


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
    side_components = {
        component: semantic_digest(
            f"cxxlens.clang22-task-{component.replace('_set_digest', '').replace('_fragment_digest', '')}.v1",
            _canonical_projection_value(
                {
                    "originating_task": context,
                    "component": component,
                }
            ),
        )
        for component in (
            "coverage_set_digest",
            "unresolved_set_digest",
            "evidence_set_digest",
            "guarantee_fragment_digest",
        )
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
        "transcript": {
            "frame_count": 32,
            "transcript_digest": "sha256:" + "9" * 64,
            "semantic_digest": semantic_digest(
                "cxxlens.provider-transcript.v1",
                _canonical_projection_value(
                    {
                        field: task[field]
                        for field in TASK_EXECUTION_KEY_FIELDS
                    }
                ),
            ),
            "raw_frame_digest": "sha256:" + "a" * 64,
        },
        "groups": groups,
        "batches": batches,
        "side_channel_components": side_components,
        "side_channel_digest": "pending",
        "task_result_digest": "pending",
    }
    result["side_channel_digest"] = expected_task_side_channel_digest(result)
    result["task_result_digest"] = expected_task_result_digest(result)
    return result


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
            "transcript": result["transcript"],
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
                "frame_count": result["transcript"]["frame_count"],
                "raw_frame_digest": result["transcript"]["raw_frame_digest"],
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
        "coverage": "coverage_set_digest",
        "unresolved": "unresolved_set_digest",
        "evidence": "evidence_set_digest",
    }[channel]
    projection = {key: value for key, value in summary.items() if key != "digest"}
    projection["task_components"] = _task_component_rows(results, component)
    return _digest_projection(
        f"cxxlens.clang22-global-{channel}.v1",
        projection,
    )


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
        for channel in ("coverage", "unresolved", "evidence")
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
        result["side_channel_digest"] = expected_task_side_channel_digest(result)
        result["task_result_digest"] = expected_task_result_digest(result)
    report["adoption"]["task_result_set_digest"] = expected_task_result_set_digest(
        results
    )
    report["adoption"]["raw_frames"]["frame_count"] = sum(
        result["transcript"]["frame_count"] for result in results
    )
    report["adoption"]["raw_frames"][
        "frame_set_digest"
    ] = expected_raw_frame_set_digest(results)
    for channel in ("coverage", "unresolved", "evidence"):
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
    coverage = {
        "record_type": "typed-coverage-unit",
        "record_count": 3 * task_count,
        "state_counts": {
            "covered": 3 * task_count,
            "excluded": 0,
            "not_applicable": 0,
            "failed": 0,
            "unresolved": 0,
            "unsupported": 0,
            "stale": 0,
            "truncated": 0,
        },
        "balance": "exact",
        "digest": "pending",
    }
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
        "coverage": coverage,
        "unresolved": unresolved,
        "evidence": evidence,
    }
    for channel in ("coverage", "unresolved", "evidence"):
        side_channels[channel]["digest"] = expected_global_side_channel_digest(
            channel,
            side_channels[channel],
            task_results,
        )
    guarantee = {
        "record_type": "typed-guarantee",
        "approximation": "exact",
        "scope": "project:fixture",
        "assumptions": [],
        "verification_modalities": [
            "clang22-parse",
            "query-parity",
            "store-reopen",
        ],
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
        "report_version": "2.0.0",
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
            "configuration": configuration,
            "platform": f"linux-x86_64-{configuration}",
            "prefix_manifest_digest": request["tool"]["prefix_manifest_digest"],
            "relocated_prefix_digest": request["tool"]["relocated_prefix_digest"],
            "tool_digest": request["tool"]["installed_executable_digest"],
            "worker_digest": request["worker"]["installed_binary_digest"],
            "relocated": True,
            "runtime_dependency_policy": "self-contained-prefix",
            "tool_worker_same_prefix": True,
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
                "frame_count": 32 * task_count,
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
                "code": "store.publication-conflict",
                "field": "publication",
            },
            "diagnostic_digest": content_digest(b"fixture stale parent"),
        },
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
    return {
        "schema": "cxxlens.clang22-materialization-report.v2",
        "report_version": "2.0.0",
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
            "head_observation": "not-observed",
            "observed_head_publication": None,
            "publication_attempted": False,
            "committed_transaction_count": 0,
            "prior_history_retained": True,
        },
        "error": {
            "code": code,
            "phase": phase,
            "subject": subject,
            "diagnostic": "fixture failure",
        },
    }


def _exact_task_result(
    request: dict[str, Any],
    task: dict[str, Any],
    result: dict[str, Any],
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
    if result["side_channel_digest"] != expected_task_side_channel_digest(result):
        fail("materialization.report-invalid", "task side-channel digest differs")
    if result["task_result_digest"] != expected_task_result_digest(result):
        fail("materialization.report-invalid", "task result digest differs")


COMPACT_RAW_PHASE_CODES = {
    "input-limit": {"materialization.request-invalid"},
    "json-decode": {"materialization.request-invalid"},
    "request-envelope": {"materialization.request-invalid"},
    "request-schema": {"materialization.request-invalid"},
    "request-version": {"materialization.version-unsupported"},
    "request-binding": {
        "materialization.identity-mismatch",
        "materialization.catalog-census-mismatch",
        "materialization.task-binding-mismatch",
        "materialization.descriptor-binding-mismatch",
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
            } and effects["head_observation"] != "not-observed":
                fail(
                    "materialization.report-invalid",
                    "pre-Store compact failure claims a head observation",
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
    expected_installation = {
        "configuration": request["tool"]["package_configuration"],
        "prefix_manifest_digest": request["tool"]["prefix_manifest_digest"],
        "relocated_prefix_digest": request["tool"]["relocated_prefix_digest"],
        "tool_digest": request["tool"]["installed_executable_digest"],
        "worker_digest": request["worker"]["installed_binary_digest"],
    }
    if any(report["installation"][key] != value for key, value in expected_installation.items()):
        fail("materialization.identity-mismatch", "installed tool/worker binding differs")
    configuration = request["tool"]["package_configuration"]
    if not report["installation"]["platform"].endswith("-" + configuration):
        fail("materialization.report-invalid", "platform/configuration binding differs")
    expected_provider = {
        "tool_executable": request["tool"]["executable"],
        "tool_interface_version": request["tool"]["interface_version"],
        "worker_executable": request["worker"]["executable"],
        "provider_id": request["worker"]["provider_id"],
        "provider_version": request["worker"]["provider_version"],
        "semantic_contract_digest": request["worker"]["semantic_contract_digest"],
        "protocol_major": request["worker"]["protocol_major"],
        "protocol_minor": request["worker"]["protocol_minor"],
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
    for execution_key, task in tasks.items():
        _exact_task_result(request, task, results[execution_key])
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
        result["transcript"]["frame_count"] for result in results.values()
    ):
        fail("materialization.transcript-invalid", "raw-frame/task transcript count differs")
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
    coverage = report["side_channels"]["coverage"]
    if sum(coverage["state_counts"].values()) != coverage["record_count"] or coverage[
        "balance"
    ] != "exact":
        fail("materialization.coverage-incomplete", "coverage balance differs")
    if coverage["record_count"] != 3 * len(request["tasks"]):
        fail("materialization.coverage-incomplete", "task coverage census differs")
    evidence = report["side_channels"]["evidence"]
    if (
        evidence["kinds"] != sorted(evidence["kind_counts"])
        or sum(evidence["kind_counts"].values()) != evidence["record_count"]
    ):
        fail("materialization.claim-invalid", "evidence kind accounting differs")
    for channel in ("coverage", "unresolved", "evidence"):
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
            _validate_reopened_handle_projection(
                receipt["projection"],
                expected_series_id=publication["series_id"],
            )
        elif receipt["status"] != "not_attempted" and first_non_present is None:
            first_non_present = receipt
    if attempt["close_reopen_status"] == "open_failed":
        if (
            failure_record["stage"] != "close-reopen"
            or failure_record["access_path"] is not None
            or failure_record["cause"]
            != {
                "kind": "sdk_error",
                "code": attempt["open_error_code"],
                "field": attempt["open_error_field"],
            }
        ):
            fail(
                "materialization.store-failure",
                "close/reopen verification failure differs",
            )
    elif first_non_present is not None:
        if (
            failure_record["stage"] != first_non_present["access_path"]
            or failure_record["access_path"] != first_non_present["access_path"]
            or failure_record["cause"]
            != {
                "kind": "sdk_error",
                "code": first_non_present["sdk_code"],
                "field": first_non_present["sdk_field"],
            }
        ):
            fail(
                "materialization.store-failure",
                "first reopened handle failure differs",
            )
    elif failure_record["cause"]["kind"] != "verification_mismatch":
        fail(
            "materialization.store-failure",
            "all-present reopen failure lacks an exact projection mismatch",
        )


def validate_failed_publication_outcome(
    request: dict[str, Any],
    report: dict[str, Any],
    expected_store: dict[str, Any],
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
            "identity_validation",
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
            or publication["store_failure"]["cause"]
            != {
                "kind": "sdk_error",
                "code": "store.sqlite-failure",
                "field": publication["store_failure"]["cause"]["field"],
            }
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
        _validate_committed_unverified_attempt(request, report)
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


def validate_documents(root: pathlib.Path) -> dict[str, Any]:
    contract_text = (root / CONTRACT).read_text(encoding="utf-8")
    validate_report_lifecycle_authority_text(
        (root / INTEGRATED_DESIGN).read_text(encoding="utf-8"),
        (root / DECISION_ADR).read_text(encoding="utf-8"),
        contract_text,
    )
    contract = load(root / CONTRACT)
    contract_schema = load(root / CONTRACT_SCHEMA)
    request_schema = load(root / REQUEST_SCHEMA)
    report_schema = load(root / REPORT_SCHEMA)
    for label, schema in (
        ("contract", contract_schema),
        ("request", request_schema),
        ("report", report_schema),
    ):
        try:
            jsonschema.Draft202012Validator.check_schema(schema)
        except jsonschema.SchemaError as error:
            fail(
                "materialization.request-invalid",
                f"materialization {label} schema: {error.message}",
            )
    validate_schema(contract, contract_schema, "materialization contract")
    validate_contract_exact(contract)
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
