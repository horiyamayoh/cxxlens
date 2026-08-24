#!/usr/bin/env python3
"""Executable snapshot identity/publication contract."""

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
DF_0200_SQLITE_CAPACITY_DECISION = {
    "selected_alternative": "A",
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
                "reopen-compaction-pin-and-backend-parity-direct-tests",
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
                "same-cap-for-memory-and-sqlite-direct-tests",
            ],
        },
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


def _require_mapping_fields(
    value: Any,
    fields: tuple[str, ...],
    *,
    code: str,
    label: str,
) -> dict[str, Any]:
    if not isinstance(value, dict):
        fail(code, f"{label} must be an object")
    missing = [field for field in fields if field not in value]
    if missing:
        fail(code, f"{label} is missing required fields: {', '.join(missing)}")
    return value


def _require_nonempty_list(value: Any, *, code: str, label: str) -> list[Any]:
    if not isinstance(value, list) or not value:
        fail(code, f"{label} must be a non-empty list")
    return value


def _require_text_fragment(
    value: Any,
    fragments: tuple[str, ...],
    *,
    code: str,
    label: str,
) -> None:
    if not isinstance(value, str) or not all(fragment in value for fragment in fragments):
        fail(code, f"{label} does not retain the required safety semantics")


def _require_safety_matrix(
    value: Any, *, code: str, label: str
) -> None:
    matrix = _require_mapping_fields(
        value, ("required", "positive"), code=code, label=label
    )
    _require_nonempty_list(matrix["required"], code=code, label=f"{label} required")
    _require_nonempty_list(matrix["positive"], code=code, label=f"{label} positive")


def validate_writer_mapping_lease_shape(lease: dict[str, Any]) -> None:
    """Validate the safety shape without pinning serialized document contents.

    The lease and its amendments are product safety descriptions.  Their
    identities, state sets, and fail-closed transitions are checked here; a
    digest of the complete YAML object is deliberately not an authority.
    """

    code = "store.sqlite-shm-writer-lease-proposal-invalid"
    _require_mapping_fields(
        lease,
        (
            "id",
            "writer_native_attachment_amendment_proposal",
            "reader_native_attachment_amendment_proposal",
            "reader_late_close_cleanup_amendment_proposal",
            "writer_gate_outcome_evidence_amendment_proposal",
            "public_surface",
            "admitted_native_projection",
            "two_stage_writer_authority",
            "process_global_registry",
            "identity_receipt",
            "reader_lifetime",
            "generation_and_races",
            "reader_pre_post_receipt",
            "fail_closed_matrix",
        ),
        code=code,
        label="same-process writer mapping lease",
    )
    if lease["id"] != "cxxlens.sqlite.same-process-writer-shm-mapping-lease.v1":
        fail(code, "lease identity is not the product identity")
    if "status" in lease:
        fail(code, "operational status is not part of the product lease")

    writer = _require_mapping_fields(
        lease["writer_native_attachment_amendment_proposal"],
        (
            "id",
            "attachment_identity",
            "generation_fresh_reader_page_set",
            "nonlast_sole_page_reader_predelegate_blocker",
            "nonlast_remaining_page_reader_predelegate",
            "nonlast_sole_page_same_thread",
            "nonlast_sole_page_other_thread",
            "established_reader_handoff_during_writer_cleanup",
            "retired_attachment_evidence",
            "same_attachment_admission",
            "repeated_same_page",
            "cross_attachment_grouping",
            "map_before_gate_group",
            "gate_completion_total_order",
            "successful_gate_postcondition",
            "fail_closed_matrix",
        ),
        code=code,
        label="writer attachment amendment",
    )
    if writer["id"] != "cxxlens.sqlite.writer-shm-native-attachment.v1":
        fail(code, "writer attachment identity differs")
    _require_text_fragment(
        writer["attachment_identity"],
        ("nonreusable", "open-epoch", "callback"),
        code=code,
        label="writer attachment identity",
    )
    _require_text_fragment(
        writer["generation_fresh_reader_page_set"],
        ("union-of-page-support", "atomically"),
        code=code,
        label="writer page support",
    )
    _require_text_fragment(
        writer["nonlast_sole_page_reader_predelegate_blocker"],
        ("predelegation", "zero-support"),
        code=code,
        label="writer sole-page blocker",
    )
    _require_text_fragment(
        writer["nonlast_remaining_page_reader_predelegate"],
        ("retains-exact-support", "not-a-cleanup-blocker"),
        code=code,
        label="writer remaining-page rule",
    )
    _require_text_fragment(
        writer["nonlast_sole_page_same_thread"],
        ("never-waits", "quarantines"),
        code=code,
        label="writer same-thread rule",
    )
    _require_text_fragment(
        writer["nonlast_sole_page_other_thread"],
        ("bounded-ordered-wait", "quarantines"),
        code=code,
        label="writer other-thread rule",
    )
    _require_text_fragment(
        writer["established_reader_handoff_during_writer_cleanup"],
        ("never-blocks", "never-counts"),
        code=code,
        label="writer handoff rule",
    )
    _require_text_fragment(
        writer["retired_attachment_evidence"],
        ("immutable", "never"),
        code=code,
        label="writer retired evidence rule",
    )
    _require_text_fragment(
        writer["cross_attachment_grouping"],
        ("forbidden",),
        code=code,
        label="writer grouping rule",
    )
    _require_text_fragment(
        writer["gate_completion_total_order"],
        ("serialized", "before"),
        code=code,
        label="writer gate ordering",
    )
    _require_text_fragment(
        writer["successful_gate_postcondition"],
        ("every-member", "no-same-attachment-pending"),
        code=code,
        label="writer gate postcondition",
    )
    writer_matrix = _require_mapping_fields(
        writer["fail_closed_matrix"],
        ("required", "positive"),
        code=code,
        label="writer fail-closed matrix",
    )
    required = _require_nonempty_list(
        writer_matrix["required"], code=code, label="writer required failures"
    )
    positive = _require_nonempty_list(
        writer_matrix["positive"], code=code, label="writer positive cases"
    )
    if not any("predelegation" in str(item) for item in required):
        fail(code, "writer fail-closed matrix omits predelegation safety")
    if not any("unmap" in str(item) for item in positive):
        fail(code, "writer fail-closed matrix omits native cleanup positive")

    reader = _require_mapping_fields(
        lease["reader_native_attachment_amendment_proposal"],
        (
            "id",
            "attachment_identity",
            "ownership",
            "map_attempt",
            "group_state",
            "ordering",
            "eager_transaction_lifetime",
            "cleanup_dispatch",
            "close_lifecycle",
            "writer_generation_boundary",
            "outward_projection",
            "fail_closed_matrix",
            "public_api",
        ),
        code=code,
        label="reader attachment amendment",
    )
    if reader["id"] != "cxxlens.sqlite.reader-shm-native-attachment.v1":
        fail(code, "reader attachment identity differs")
    identity = _require_mapping_fields(
        reader["attachment_identity"],
        ("binding", "cleanup_only_observation_owner"),
        code=code,
        label="reader attachment identity",
    )
    binding = _require_nonempty_list(identity["binding"], code=code, label="reader identity binding")
    if not any("checked-observed-SHM" in str(item) for item in binding):
        fail(code, "reader identity omits observed SHM object binding")
    _require_text_fragment(
        identity["cleanup_only_observation_owner"],
        ("noncopyable", "promote-once"),
        code=code,
        label="reader observation owner",
    )
    ownership = _require_mapping_fields(
        reader["ownership"],
        ("custody_kind_enum", "custody_state_enum"),
        code=code,
        label="reader ownership",
    )
    for name in ("map_attempt", "use_session", "connection_close", "logical_ack"):
        if name not in ownership["custody_kind_enum"]:
            fail(code, f"reader custody enum omits {name}")
    if ownership["custody_state_enum"] != [
        "live",
        "consumed_with_exact_terminal_receipt",
        "transferred_to_exact_successor",
        "transferred_to_durable_tombstone",
    ]:
        fail(code, "reader custody state is not closed")
    group_state = _require_mapping_fields(
        reader["group_state"],
        ("reservation_phase_enum", "reservation_transition_graph"),
        code=code,
        label="reader group state",
    )
    for phase in (
        "reserved",
        "predecessor_route_active",
        "unpublished_cleanup_admitted",
        "unpublished_cleanup_confirmed",
        "terminal_quarantined",
    ):
        if phase not in group_state["reservation_phase_enum"]:
            fail(code, f"reader group state omits {phase}")
        if phase not in group_state["reservation_transition_graph"]:
            fail(code, f"reader transition graph omits {phase}")
    eager = _require_mapping_fields(
        reader["eager_transaction_lifetime"],
        (
            "owner_set",
            "pointer_coverage_relation",
            "pointer_publication_commit",
            "session_start_issuer",
            "pre_mint_route_partition",
            "cached_pointer_session_admission",
        ),
        code=code,
        label="reader eager lifetime",
    )
    _require_text_fragment(
        eager["owner_set"],
        ("exactly-one", "move-only"),
        code=code,
        label="reader owner set",
    )
    _require_text_fragment(
        eager["pointer_coverage_relation"],
        ("exactly-one", "pointer"),
        code=code,
        label="reader pointer coverage",
    )
    _require_text_fragment(
        eager["pointer_publication_commit"],
        ("atomically", "pointer"),
        code=code,
        label="reader pointer publication",
    )
    _require_text_fragment(
        eager["pre_mint_route_partition"],
        ("exactly-one", "reject"),
        code=code,
        label="reader pre-mint route",
    )
    _require_text_fragment(
        eager["cached_pointer_session_admission"],
        ("Wal-apWiData-without-xShmMap",),
        code=code,
        label="reader cached pointer admission",
    )
    cleanup = _require_mapping_fields(
        reader["cleanup_dispatch"],
        ("failure_exact_mapped_existing_group", "logical_ack_phase_enum"),
        code=code,
        label="reader cleanup dispatch",
    )
    _require_text_fragment(
        cleanup["failure_exact_mapped_existing_group"],
        ("active-use", "zero-owner"),
        code=code,
        label="reader existing-group cleanup",
    )
    if cleanup["logical_ack_phase_enum"] != [
        "not_applicable",
        "awaiting_sqlite_ack",
        "consumed_by_exact_unmap",
        "consumed_by_close",
    ]:
        fail(code, "reader logical acknowledgement states are not closed")
    if "close_cut" not in reader["ordering"]:
        fail(code, "reader close cut is missing")
    if "family-exclusion-custody-count" not in reader["writer_generation_boundary"]["successor"]:
        fail(code, "reader successor exclusion is missing")
    if reader["public_api"] != "unchanged":
        fail(code, "reader proposal changes the public API")
    outward = _require_mapping_fields(
        reader["outward_projection"],
        ("exact_determinate_no_change", "unmap_result"),
        code=code,
        label="reader outward projection",
    )
    _require_text_fragment(
        outward["exact_determinate_no_change"],
        ("non-OK", "null"),
        code=code,
        label="reader no-change projection",
    )
    _require_text_fragment(
        outward["unmap_result"],
        ("SQLITE_IOERR", "additional-call"),
        code=code,
        label="reader unmap projection",
    )
    reader_matrix = _require_mapping_fields(
        reader["fail_closed_matrix"],
        ("required", "positive"),
        code=code,
        label="reader fail-closed matrix",
    )
    if not _require_nonempty_list(reader_matrix["required"], code=code, label="reader required failures"):
        fail(code, "reader required failures are empty")

    late = _require_mapping_fields(
        lease["reader_late_close_cleanup_amendment_proposal"],
        (
            "id",
            "outer_unwind_authority",
            "close_terminal_provenance",
            "drain_subledger",
            "acknowledgement",
            "quarantine",
            "scope_boundary",
            "fail_closed_matrix",
        ),
        code=code,
        label="late-close cleanup amendment",
    )
    if late["id"] != "cxxlens.sqlite.reader-late-close-cleanup.v1":
        fail(code, "late-close amendment identity differs")
    drain = _require_mapping_fields(
        late["drain_subledger"],
        ("retained_pins", "state_enum", "transition_graph"),
        code=code,
        label="late-close drain subledger",
    )
    required_pins = {
        "proposal-candidate",
        "reader-map-predelegate",
        "runtime-vfs-registration-and-callback-cohort",
        "connection-open-epoch",
        "file-family-and-mapping-generation",
    }
    if not required_pins.issubset(set(drain["retained_pins"])):
        fail(code, "late-close drain does not retain all callback pins")
    required_states = {
        "cleanup_confirmed_awaiting_sqlite_ack",
        "terminal_quarantined",
    }
    if not required_states.issubset(set(drain["state_enum"])):
        fail(code, "late-close drain state is incomplete")
    transitions = drain["transition_graph"].get("cleanup_confirmed_awaiting_sqlite_ack", [])
    if not {"consumed_by_exact_outer_unmap", "terminal_quarantined"}.issubset(set(transitions)):
        fail(code, "late-close acknowledgement transition is incomplete")
    close_provenance = _require_mapping_fields(
        late["close_terminal_provenance"],
        ("kind_enum", "tuple_fields"),
        code=code,
        label="late-close provenance",
    )
    if not close_provenance["kind_enum"] or "native_xClose_completed" in close_provenance["kind_enum"]:
        fail(code, "late-close provenance has an invalid terminal kind")
    if "native-xClose-call-count-zero" not in close_provenance["tuple_fields"]:
        fail(code, "late-close provenance omits zero native close")
    acknowledgement = _require_mapping_fields(
        late["acknowledgement"],
        ("consumption", "wrong_outer_owner", "exact_outer_owner_indeterminate", "confirmed_after_close_replay"),
        code=code,
        label="late-close acknowledgement",
    )
    _require_text_fragment(
        acknowledgement["consumption"],
        ("only-exact", "once", "zero-native"),
        code=code,
        label="late-close acknowledgement consumption",
    )
    _require_text_fragment(
        acknowledgement["wrong_outer_owner"],
        ("wrong", "IOERR", "preserves"),
        code=code,
        label="late-close wrong owner",
    )
    _require_text_fragment(
        acknowledgement["exact_outer_owner_indeterminate"],
        ("terminal_quarantined", "zero-native"),
        code=code,
        label="late-close indeterminate owner",
    )
    _require_text_fragment(
        acknowledgement["confirmed_after_close_replay"],
        ("distinct", "zero-native"),
        code=code,
        label="late-close replay",
    )
    if "no-close-retry" not in late["quarantine"]["forbidden_authority"]:
        fail(code, "late-close quarantine permits forbidden retry")
    if late["scope_boundary"]["public_api"] != "unchanged":
        fail(code, "late-close amendment changes the public API")
    late_matrix = _require_mapping_fields(
        late["fail_closed_matrix"],
        ("required", "positive"),
        code=code,
        label="late-close fail-closed matrix",
    )
    _require_nonempty_list(late_matrix["required"], code=code, label="late-close required failures")

    gate = _require_mapping_fields(
        lease["writer_gate_outcome_evidence_amendment_proposal"],
        (
            "id",
            "gate_profile",
            "gate_attempt_owner",
            "closed_outcome_union",
            "native_attachment_binding",
            "registry_cut",
            "native_state_resolution",
            "native_effect_dispatch_matrix",
            "empty_and_mixed_group",
            "composite_cleanup_lineage",
            "reader_boundary",
            "fail_closed_matrix",
        ),
        code=code,
        label="writer gate outcome amendment",
    )
    if gate["id"] != "cxxlens.sqlite.writer-gate-outcome-evidence.v1":
        fail(code, "writer gate identity differs")
    profile = _require_mapping_fields(
        gate["gate_profile"],
        ("canonical_policy_profile_digest", "ordered_stage_enum", "stage_value_projections", "terminal_evidence_locus"),
        code=code,
        label="writer gate profile",
    )
    _require_text_fragment(
        profile["canonical_policy_profile_digest"],
        ("acceleration-key-only", "never-equality"),
        code=code,
        label="writer gate policy digest",
    )
    required_stages = [
        "writer-readwrite-mode",
        "runtime-version-and-locator",
        "runtime-vfs-file-family-and-open-epoch",
        "synchronous-full-and-wal-mode",
        "current-v3-format-schema-head-counter-authority",
        "store-writer-open-before-publication-effect",
    ]
    if profile["ordered_stage_enum"] != required_stages:
        fail(code, "writer gate stages are not ordered and closed")
    for stage in required_stages:
        projection = profile["stage_value_projections"].get(stage)
        if not isinstance(projection, dict) or not {
            "exact_value_projection",
            "exact_effect_projection",
            "success",
        }.issubset(projection):
            fail(code, f"writer gate stage projection is incomplete: {stage}")
    if "arbitrary-caller-locus" in profile["terminal_evidence_locus"].get("closed_kind_enum", []):
        fail(code, "writer gate permits an arbitrary terminal locus")
    _require_text_fragment(
        gate["closed_outcome_union"]["exclusivity"],
        ("double-issue", "replay"),
        code=code,
        label="writer gate outcome exclusivity",
    )
    lifecycle = gate["native_attachment_binding"].get("reservation_lifecycle")
    if lifecycle != ["reserved", "claimed_inflight", "consumed_to_present", "revoked", "quarantined"]:
        fail(code, "writer gate reservation lifecycle is not closed")
    _require_text_fragment(
        gate["composite_cleanup_lineage"]["preinvoke_consumption"],
        ("consume", "never-reissue"),
        code=code,
        label="writer gate cleanup ownership",
    )
    if gate["composite_cleanup_lineage"]["closed_obligation_union"] != [
        "no_mapping_close_only",
        "mapped_unmap_then_close",
    ]:
        fail(code, "writer gate cleanup obligations are not closed")
    _require_text_fragment(
        gate["reader_boundary"]["grouping"],
        ("excludes-reader",),
        code=code,
        label="writer gate reader boundary",
    )
    _require_text_fragment(
        gate["reader_boundary"]["transitive_authorization"],
        ("forbidden",),
        code=code,
        label="writer gate transitive boundary",
    )
    gate_matrix = _require_mapping_fields(
        gate["fail_closed_matrix"],
        ("required", "positive"),
        code=code,
        label="writer gate fail-closed matrix",
    )
    _require_nonempty_list(gate_matrix["required"], code=code, label="writer gate required failures")
    try:
        schema = load_yaml(ROOT / CONTRACT_SCHEMA)
        expected = schema["properties"]["format_compatibility"]["properties"][
            "sqlite_source_shm_readonly_capability"
        ]["const"]["shm_map_state_machine"][
            "same_process_writer_mapping_lease_proposal"
        ]
    except (KeyError, TypeError, OSError, yaml.YAMLError) as error:
        fail(code, f"lease semantic schema is missing: {error}")
    if lease != expected:
        fail(code, "nested lease safety semantics differ from the declared schema")


def validate_df_0200_ingress_shape(ingress: dict[str, Any]) -> None:
    """Validate DF-0200 safety semantics without copying serialized bytes."""

    code = "store.materialization-ingress-contract-invalid"
    _require_mapping_fields(
        ingress,
        (
            "resolution_id", "activation", "caller_scope", "bridge", "public_api",
            "public_writer_states", "public_stage_validate_publish_semantics",
            "source", "counter_model", "publication", "memory_backend",
            "sqlite_backend", "sqlite_capacity_decision", "compatibility",
        ),
        code=code,
        label="DF-0200 ingress",
    )
    allowed_fields = {
        "resolution_id", "activation", "caller_scope", "bridge", "public_api",
        "public_writer_states", "public_stage_validate_publish_semantics",
        "source", "counter_model", "publication", "memory_backend",
        "sqlite_backend", "sqlite_capacity_decision", "compatibility",
    }
    if set(ingress) != allowed_fields:
        fail(code, "DF-0200 ingress has unknown or missing fields")
    if ingress["resolution_id"] != "cxxlens.df-0200.incremental-claim-store.v1" or ingress["public_api"] != "unchanged":
        fail(code, "DF-0200 identity or public API changed")
    source = _require_mapping_fields(
        ingress["source"],
        ("ownership", "codec", "events", "public-stage-mixing", "materializer-receipt-is-validation-authority", "external_completeness_authority", "store-validation"),
        code=code,
        label="DF-0200 source",
    )
    codec = _require_mapping_fields(
        source["codec"],
        ("id", "event_kind_codes", "authority_binding", "validation", "unknown_missing_reordered_or_truncated"),
        code=code,
        label="DF-0200 codec",
    )
    if codec["id"] != "cxxlens.df-0200.partition-event-stream.v1" or codec["event_kind_codes"] != {"partition-begin": 1, "claim-occurrence": 2, "detached-row": 3, "claim-annotation": 4, "coverage": 5, "unresolved": 6, "partition-end": 7}:
        fail(code, "DF-0200 event codec identity differs")
    binding = _require_mapping_fields(codec["authority_binding"], ("required_sections", "store_checker", "materialization_checker"), code=code, label="DF-0200 codec binding")
    required_sections = {"canonical_tuple_profile", "field_catalog", "stream_header", "frame", "event_projections", "event_container", "digest_framing", "digest_domains", "canonical_order", "stream_trailer", "rejection"}
    if not required_sections.issubset(set(_require_nonempty_list(binding["required_sections"], code=code, label="DF-0200 codec sections"))):
        fail(code, "DF-0200 codec sections are incomplete")
    _require_text_fragment(binding["store_checker"], ("self-contained", "no-reverse-load"), code=code, label="DF-0200 store checker")
    _require_text_fragment(binding["materialization_checker"], ("recompute", "exact-match"), code=code, label="DF-0200 materializer checker")
    _require_text_fragment(codec["validation"], ("every-full-byte-frame",), code=code, label="DF-0200 codec validation")
    _require_text_fragment(codec["unknown_missing_reordered_or_truncated"], ("reject-entire-candidate",), code=code, label="DF-0200 rejection")
    if source["events"] != ["partition-begin", "claim-occurrence", "detached-row", "claim-annotation", "coverage", "unresolved", "partition-end"] or source["public-stage-mixing"] != "forbidden" or source["materializer-receipt-is-validation-authority"] is not False:
        fail(code, "DF-0200 event or receipt boundary differs")
    external = _require_mapping_fields(source["external_completeness_authority"], ("validated_request", "sealed_execution_journal_and_task_receipts", "pre_encoder_receipt_oracle", "required_global_censuses_and_digests", "required_manifests", "required_receipts", "store_comparison", "comparison", "whole_partition_drop", "correlated_omission_rejection"), code=code, label="DF-0200 external authority")
    _require_text_fragment(external["whole_partition_drop"], ("reject", "self-consistent"), code=code, label="DF-0200 omission rule")
    for item in ("task", "partition", "event", "claim", "row", "coverage", "unresolved"):
        if item not in external["required_global_censuses_and_digests"]:
            fail(code, f"DF-0200 global census omits {item}")
    for item in ("segment-manifest", "run-manifest", "merge-manifest"):
        if item not in external["required_manifests"]:
            fail(code, f"DF-0200 manifest set omits {item}")
    for item in ("byte-receipt", "record-receipt", "seal-receipt"):
        if item not in external["required_receipts"]:
            fail(code, f"DF-0200 receipt set omits {item}")
    oracle = _require_mapping_fields(external["pre_encoder_receipt_oracle"], ("owner", "canonicalization", "receipt_seal", "execution_journal_receipt_set"), code=code, label="DF-0200 receipt oracle")
    _require_text_fragment(oracle["owner"], ("installed-tool-private",), code=code, label="DF-0200 receipt owner")
    _require_text_fragment(oracle["canonicalization"], ("independent", "sort"), code=code, label="DF-0200 receipt canonicalization")
    receipt = _require_mapping_fields(oracle["receipt_seal"], ("projection", "output"), code=code, label="DF-0200 receipt seal")
    for field in ("successful-seal", "provider-stdout-sha256", "task-event-count-and-digest", "task-coverage-count-and-digest", "task-unresolved-count-and-digest", "selected-request-entry-binding-digest"):
        if field not in receipt["projection"]:
            fail(code, f"DF-0200 receipt seal omits {field}")
    if receipt["output"] != "semantic-v2-sha256-string":
        fail(code, "DF-0200 receipt seal is not semantic-digest bound")
    journal = _require_mapping_fields(oracle["execution_journal_receipt_set"], ("projection", "output"), code=code, label="DF-0200 journal receipt")
    if "ordered-pre-encoder-task-receipt-seal-digests" not in journal["projection"] or journal["output"] != "semantic-v2-sha256-string":
        fail(code, "DF-0200 journal receipt set is incomplete")
    validation = _require_mapping_fields(source["store-validation"], ("independence", "external-authority-inputs", "self-reported-stream-census-authority", "required-recomputation"), code=code, label="DF-0200 store validation")
    if "independent" not in validation["independence"] or validation["self-reported-stream-census-authority"] != "forbidden":
        fail(code, "DF-0200 store validation is not independent")
    for item in ("canonical-claim-and-row-identity", "full-byte-event-codec-framing-order-checksum-and-seal", "external-request-journal-task-and-global-census-digest-closure", "canonical-v5-encode-decode-byte-identity"):
        if item not in validation["required-recomputation"]:
            fail(code, f"DF-0200 recomputation omits {item}")
    counters = _require_mapping_fields(ingress["counter_model"], ("semantic_version_components", "canonical_v5_collection_counts", "collection_overflow_failure"), code=code, label="DF-0200 counters")
    if counters["semantic_version_components"] != {"encoding": "u32", "maximum": (1 << 32) - 1} or counters["canonical_v5_collection_counts"] != {"encoding": "u64be", "maximum": (1 << 64) - 1, "aggregate_before_narrowing": "checked-u128"}:
        fail(code, "DF-0200 counters are not widened before narrowing")
    overflow = _require_mapping_fields(counters["collection_overflow_failure"], ("operation", "code", "field"), code=code, label="DF-0200 overflow")
    if overflow["operation"] != "partition_stage" or overflow["code"] != "store.counter-overflow":
        fail(code, "DF-0200 overflow route differs")
    publication = _require_mapping_fields(ingress["publication"], ("candidate", "publish-attempts", "partial-visibility", "stale-or-failed-publish-preserves-head"), code=code, label="DF-0200 publication")
    if publication != {"candidate": "exactly-one-unpublished", "publish-attempts": "exactly-one", "partial-visibility": "forbidden", "stale-or-failed-publish-preserves-head": True}:
        fail(code, "DF-0200 publication lifecycle is not bounded")
    compatibility = _require_mapping_fields(ingress["compatibility"], ("snapshot_payload_v5_schema_and_semantic_projection", "sqlite_physical_format", "snapshot_and_publication_identity", "public_cursor_lifetime_and-success-results", "additive_public_result"), code=code, label="DF-0200 compatibility")
    if compatibility["snapshot_payload_v5_schema_and_semantic_projection"] != "unchanged-with-authorized-physical-generation-transition" or compatibility["sqlite_physical_format"] != "cxxlens.sqlite-semantic-store.v3-3.0.0" or compatibility["snapshot_and_publication_identity"] != "unchanged" or compatibility["public_cursor_lifetime_and-success-results"] != "unchanged" or compatibility["additive_public_result"] != "store.migration-required":
        fail(code, "DF-0200 compatibility semantics differ")
    decision = _require_mapping_fields(ingress["sqlite_capacity_decision"], ("selected_alternative", "confirmed_blocker", "required_parity", "weakening_parity", "alternatives"), code=code, label="SQLite capacity decision")
    alternatives = _require_mapping_fields(decision["alternatives"], ("A", "B"), code=code, label="SQLite capacity alternatives")
    selected = _require_mapping_fields(alternatives["A"], ("decision", "disposition", "preserves", "requires"), code=code, label="SQLite capacity A")
    rejected = _require_mapping_fields(alternatives["B"], ("decision", "disposition", "preserves", "requires"), code=code, label="SQLite capacity B")
    if decision["selected_alternative"] != "A" or decision["weakening_parity"] != "forbidden" or selected["disposition"] != "selected" or not str(selected["decision"]).startswith("sqlite-physical-v3-") or rejected["disposition"] != "rejected-not-selected":
        fail(code, "SQLite capacity decision semantics differ")
    try:
        schema = load_yaml(ROOT / CONTRACT_SCHEMA)
        expected = schema["$defs"]["df_0200_materialization_ingress"]["const"]
    except (KeyError, TypeError, OSError, yaml.YAMLError) as error:
        fail(code, f"DF-0200 semantic schema is missing: {error}")
    if ingress != expected:
        fail(code, "DF-0200 nested safety semantics differ from the declared schema")


def validate_contract_shape(contract: dict[str, Any]) -> None:
    try:
        writer_mapping_lease = contract["format_compatibility"]["sqlite_source_shm_readonly_capability"]["shm_map_state_machine"]["same_process_writer_mapping_lease_proposal"]
    except (KeyError, TypeError) as error:
        fail("store.sqlite-shm-writer-lease-proposal-invalid", f"required proposal field is missing: {error}")
    validate_writer_mapping_lease_shape(writer_mapping_lease)
    try:
        ingress = contract["df_0200_materialization_ingress"]
    except (KeyError, TypeError) as error:
        fail("store.materialization-ingress-contract-invalid", f"DF-0200 ingress is missing: {error}")
    validate_df_0200_ingress_shape(ingress)
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
            "accepted_empty_normalization_source_anchor": [
                "canonical-locator",
                "exact-pinned-vfs-identity",
                "pinned-sqlite-runtime-identity-and-version",
                "exact-normalization-effect-grammar-profile-receipt",
                "accepted-empty-private-recovery-stable-source-receipt",
                "preinit-exact-empty-wal-header-anchor",
                (
                    "pre-coordination-zero-wal-branch-absent-create-or-preexisting-"
                    "bound-size-zero-open"
                ),
                (
                    "actual-normalizer-main-open-file-instance-identity-and-"
                    "directory-entry-binding"
                ),
                (
                    "retained-authenticated-parent-directory-capability-and-"
                    "continuous-namespace-epoch"
                ),
                "pre-coordination-main-size-and-sha256-and-sidecar-census",
                (
                    "pre-coordination-decoded-main-page-size-and-file-offset-zero-"
                    "database-page-one-sha256-and-exact-header-byte-range-zero-"
                    "through-one-hundred-exclusive"
                ),
                (
                    "immutable-held-pre-main-exact-byte-snapshot-with-length-and-"
                    "streaming-byte-receipt"
                ),
                "exact-empty-logical-projection",
                "normalization-id",
                (
                    "deterministic-expected-post-whole-main-exact-byte-projection-"
                    "plus-size-and-sha256-derived-before-effect-from-the-immutable-"
                    "pre-main-byte-snapshot-by-streaming-copy-with-only-the-"
                    "authorized-page-one-field-patch-plus-the-complete-expected-"
                    "rollback-empty-logical-projection-and-sidecar-census"
                ),
            ],
            "accepted_empty_normalization": [
                "exact-accepted-empty-normalization-source-anchor-receipt",
                "exact-normalization-effect-grammar-profile-receipt",
                (
                    "exact-accepted-empty-normalization-coordination-sequence-two-"
                    "receipt-with-denied-sequence-one-prerequisite-and-armed-after-"
                    "exclusive-true"
                ),
                (
                    "later-repeated-exclusive-xLock-source-anchor-and-same-main-"
                    "entry-recheck"
                ),
                (
                    "coordination-zero-wal-open-flags-file-instance-identity-"
                    "directory-entry-size-and-sha256"
                ),
                "planned-normalization-candidate-id",
            ],
            "accepted_empty_normalization_completed_edge": [
                "exact-accepted-empty-normalization-pre-effect-full-receipt",
                "exact-normalization-effect-grammar-profile-receipt",
                (
                    "exact-accepted-empty-normalization-full-sequence-three-receipt-"
                    "with-coordination-sequence-two-prerequisite-and-armed-after-"
                    "exclusive-true"
                ),
                "exact-normalization-bounded-effect-transcript-receipt",
                "exact-coordination-wal-delete-retained-authenticated-parent-fsync-receipt",
                "exact-journal-creation-retained-authenticated-parent-fsync-receipt",
                "confirmed-single-delete-normalization-transition-result",
                "exact-terminal-journal-delete-retained-authenticated-parent-fsync-receipt",
                "exactly-one-confirmed-connection-close",
                (
                    "same-main-and-entry-plus-streaming-exact-post-main-byte-"
                    "equality-to-the-sealed-deterministic-expected-projection-with-"
                    "size-and-sha256-only-acceleration-plus-complete-post-structural-"
                    "logical-validation-and-sidecar-census"
                ),
                "sealed-normalization-edge-id",
            ],
            "fresh_empty_anchor_is_not_post_format_authority_state": True,
        },
        "post_format_candidate_extension": (
            "only-after-candidate-id-and-exact-candidate-projection-are-complete"
        ),
        "fresh_initialization_receipt_seal": "before-journal-arming",
        "accepted_empty_normalization_source_anchor": (
            "candidate-fields-computed-while-effects-are-denied-before-installing-"
            "the-pending-coordination-request-and-immutable-receipt-sealed-inside-"
            "the-first-exclusive-callback-before-coordination-publication-or-any-"
            "persistent-effect"
        ),
        "accepted_empty_normalization_source_anchor_profile": (
            "terminal_reclassification.sealed_receipt_profiles."
            "accepted_empty_normalization_source_anchor"
        ),
        "accepted_empty_normalization_source_anchor_seal": (
            "pending-coordination-request-may-be-installed-while-effects-remain-"
            "denied-then-after-first-successful-underlying-exclusive-xLock-and-"
            "local-zero-initialized-HAS_MOVED-exact-SQLITE_OK-output-zero-source-"
            "recheck-seal-the-immutable-source-anchor-immediately-before-publishing-"
            "coordination-sequence-two-and-before-zero-wal-or-other-persistent-effect"
        ),
        "accepted_empty_normalization_receipt_seal": (
            "in-the-later-repeated-second-pending-full-arm-exclusive-xLock-callback-"
            "after-prior-exact-coordination-sequence-two-receipt-and-exact-zero-wal-"
            "observation-and-before-full-arm-or-header-effect"
        ),
        "accepted_empty_normalization_receipt_extension": (
            "exact-source-anchor-receipt-plus-exact-coordination-sequence-two-"
            "receipt-plus-the-later-repeated-exclusive-lock-recheck-plus-"
            "coordination-zero-wal-observation-plus-planned-normalization-candidate-id"
        ),
        "accepted_empty_normalization_candidate_identity": (
            "planned-normalization-candidate-id-in-the-pre-effect-full-receipt-never-"
            "proves-transition-close-poststate-physical-edge-or-success"
        ),
        "accepted_empty_normalization_completed_edge_profile": (
            "terminal_reclassification.sealed_receipt_profiles."
            "accepted_empty_normalization_completed_edge"
        ),
        "accepted_empty_normalization_completed_edge_seal": (
            "only-after-the-exact-normalization-bounded-effect-transcript-and-"
            "coordination-wal-delete-journal-creation-and-terminal-journal-delete-"
            "retained-authenticated-parent-fsync-receipts-the-single-delete-"
            "normalization-transition-finalize-exactly-one-confirmed-close-and-post-"
            "close-total-reclassification-bind-the-planned-candidate-source-"
            "coordination-and-full-arm-receipts-to-the-exact-same-identity-poststate"
        ),
        "accepted_empty_normalization_operation_identity": (
            "accepted-empty-normalization-operation-plus-journal-normalization-"
            "boundary-plus-normalization-transition-phase-and-generic-connection-"
            "close-distinct-from-fresh-initialization"
        ),
        "accepted_empty_normalization_success": (
            "intermediate-only-no-store-and-only-a-sealed-same-identity-normalized-"
            "empty-operation-edge-continues-into-ordinary-fresh-initialization"
        ),
        "accepted_empty_normalization_public_success": (
            "forbidden-until-ordinary-fresh-initialization-independently-completes"
        ),
        "accepted_empty_normalization_receiptless_crash_profile_draft": {
            "sqlite_profile_path": (
                "transaction.fresh_v3_initialization.guards.filesystem."
                "precreate_census.preauthority_sidecar_candidate."
                "accepted_empty_original_normalization.receiptless_crash_profile_draft"
            ),
            "family_partition": [
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
            "sealed_receipt": [
                "canonical-locator-and-pinned-runtime-vfs",
                (
                    "exact-layer-discriminator-and-family-applicable-runtime-build-"
                    "vfs-device-filesystem-S-P-N-E-receipt-with-disposable-run-plan-"
                    "or-runtime-safety-receipt-never-cross-authorizing"
                ),
                "continuous-parent-namespace-epoch-and-object-entry-identities",
                "exact-family-tag-and-raw-whole-main-plus-sidecar-byte-receipts",
                (
                    "family-applicable-exact-pre-and-when-journal-preimages-exist-"
                    "deterministic-post-projections-plus-byte-exact-current-main"
                ),
                "private-validation-and-confirmed-close",
                "source-effect-phase-and-exact-bounded-effect-receipt",
                "post-effect-family-and-exact-next-route",
            ],
            "cold_operation_history_inference": (
                "forbidden-FZ-post-FI-and-FO-prove-only-an-independent-current-"
                "rollback-header-exact-empty-anchor-while-F0-FZ-pre-FP-and-FH-may-"
                "only-start-a-new-live-receipted-normalizer"
            ),
            "disposable_fixture_capability": (
                "harness-minted-nonforgeable-private-root-capability-run-ID-and-"
                "internal-test-only-entrypoint-with-user-and-canonical-locators-"
                "rejected-and-no-production-API-route"
            ),
            "profile_receipt_layering": (
                "disposable-binds-capability-run-plan-and-runtime-safety-receipt-"
                "without-authorizing-production-production-requires-direct-positive-"
                "negative-fault-determinism-and-resource-tests-and-the-product-safety-"
                "receipt"
            ),
            "public_success": (
                "forbidden-the-only-success-is-an-internal-receipt-bound-handoff-to-"
                "clean-normalization-or-ordinary-fresh-initialization"
            ),
        },
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
    """Check the schema binding and its safety-critical semantic shape."""

    def check_closed_object_nodes(value: Any, path: tuple[str, ...] = ()) -> None:
        if isinstance(value, dict):
            if (
                value.get("type") == "object"
                and isinstance(value.get("properties"), dict)
                and path != ("$defs", "open_object")
                and "const" not in value
                and value.get("additionalProperties") is True
            ):
                fail(
                    "store.materialization-ingress-contract-invalid",
                    "nested schema object permits undeclared fields: "
                    + "/".join(path),
                )
            for key, child in value.items():
                check_closed_object_nodes(child, path + (key,))
        elif isinstance(value, list):
            for index, child in enumerate(value):
                check_closed_object_nodes(child, path + (str(index),))

    check_closed_object_nodes(schema)

    try:
        required = schema["required"]
        binding = schema["properties"][
            "df_0200_materialization_ingress"
        ]
        semantic_const = schema["$defs"][
            "df_0200_materialization_ingress"
        ]["const"]
    except (KeyError, TypeError) as error:
        fail(
            "store.materialization-ingress-contract-invalid",
            f"DF-0200 schema binding is missing: {error}",
        )
    if (
        "df_0200_materialization_ingress" not in required
        or binding
        != {"$ref": "#/$defs/df_0200_materialization_ingress"}
    ):
        fail(
            "store.materialization-ingress-contract-invalid",
            "DF-0200 accepted materialization ingress schema binding differs",
        )
    if not isinstance(semantic_const, dict):
        fail(
            "store.materialization-ingress-contract-invalid",
            "DF-0200 schema semantic projection is not an object",
        )
    validate_df_0200_ingress_shape(semantic_const)


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
    if comparisons == 0:
        fail("store.perturbation-matrix-incomplete", str(comparisons))
    return contract, results, comparisons


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    args = parser.parse_args()
    contract, results, comparisons = validate_all(args.root.resolve())
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
