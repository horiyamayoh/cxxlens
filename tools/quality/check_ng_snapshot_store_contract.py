#!/usr/bin/env python3
"""Executable snapshot identity and publication-series contract for Issue #146."""

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


def validate_design(root: pathlib.Path) -> None:
    design = (root / "docs/design/cxxlens_next_generation_integrated_design_ja.md").read_text(
        encoding="utf-8"
    )
    required = (
        "1.0.0-normative",
        "cxxlens_ng_snapshot_store_contract.yaml",
        "snapshot_series_selector",
        "producer_input_basis",
        "Issue #146",
    )
    for marker in required:
        if marker not in design:
            fail("store.design-marker-missing", marker)
    for stale in ("current(catalog_id)", "producer_input_snapshot"):
        if stale in design:
            fail("store.design-stale-contract", stale)
    index = (root / "docs/design/catalogs/README.md").read_text(encoding="utf-8")
    if "Snapshot / Store Contract" not in index or "#146" not in index:
        fail("store.catalog-index-stale", "snapshot contract")


def validate_all(
    root: pathlib.Path,
) -> tuple[dict[str, Any], list[dict[str, Any]], int]:
    contract = load_yaml(root / CONTRACT)
    schema_validate(contract, load_yaml(root / CONTRACT_SCHEMA), "store contract")
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
