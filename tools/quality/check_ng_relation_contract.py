#!/usr/bin/env python3
"""Validate the NG0 exact relation, claim-envelope, and conformance contract."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import re
import sys
from collections import defaultdict
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
REGISTRY = pathlib.Path("schemas/cxxlens_ng_relation_registry.yaml")
REGISTRY_SCHEMA = pathlib.Path("schemas/cxxlens_ng_relation_registry.schema.yaml")
CLAIM_ENVELOPE_SCHEMA = pathlib.Path("schemas/cxxlens_ng_claim_envelope.schema.yaml")
VECTORS = pathlib.Path("schemas/cxxlens_ng_relation_conformance_vectors.yaml")
VECTORS_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_relation_conformance_vectors.schema.yaml"
)
REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_relation_conformance_report.schema.yaml"
)

REQUIRED_RELATIONS = {
    "build.project",
    "build.compile_unit",
    "build.variant",
    "build.toolchain_context",
    "source.file",
    "source.span",
    "source.origin",
    "cc.entity",
    "cc.declaration",
    "cc.type",
    "cc.type_component",
    "cc.call_site",
    "cc.call_direct_target",
    "core.provider_execution",
    "core.unresolved",
    "core.claim_conflict",
    "core.differential_disagreement",
    "company.lock.acquire",
    "frontend.clang22.entity_observation",
    "frontend.clang22.type_observation",
    "frontend.clang22.call_observation",
}

CLANG22_OBSERVATION_RELATIONS = {
    "frontend.clang22.entity_observation",
    "frontend.clang22.type_observation",
    "frontend.clang22.call_observation",
}

CLANG22_INSTALLED_OUTPUT_DESCRIPTORS = {
    "frontend.clang22.entity_observation.v2",
    "frontend.clang22.type_observation.v2",
    "frontend.clang22.call_observation.v2",
    "cc.entity.v1",
    "cc.call_site.v1",
    "cc.call_direct_target.v1",
}

CLANG22_PRIMARY_SPAN_SUFFIXES = (
    "source",
    "source_snapshot",
    "source_file",
    "source_begin",
    "source_end",
    "source_role",
    "source_read_only",
)

REQUIRED_VECTOR_IDS = {
    "exact-registry",
    "envelope-only-condition",
    "noncircular-domain-identity",
    "envelope-instance-valid",
    "envelope-containing-snapshot-forbidden",
    "hard-reference-resolved",
    "hard-reference-missing",
    "soft-reference-missing-accounted",
    "soft-reference-missing-unaccounted",
    "open-symbol-unknown-preserved",
    "closed-symbol-unknown-rejected",
    "canonical-digest-scalar",
    "malformed-digest-scalar",
    "noncanonical-semver-scalar",
    "empty-typed-id-scalar",
    "canonical-set-scalar",
    "noncanonical-set-order",
    "minor-optional-column-preserved",
    "minor-required-column-rejected",
    "static-dynamic-column-parity",
    "static-dynamic-column-mismatch",
    "external-relation-registration",
    "external-relation-central-switch",
    "exact-call-model",
    "stale-inline-direct-target",
    "call-site-partition-column",
    "entity-occurrence-anchor-in-identity",
    "clang22-observation-v2-family",
    "clang22-span-column-missing",
    "clang22-span-constraint-partial",
    "clang22-span-constraint-duplicate",
    "clang22-span-constraint-unknown-column",
    "clang22-span-reference-mismatch",
    "clang22-hard-reference-unknown",
    "dynamic-only-static-projection",
}


class RelationContractError(ValueError):
    """A stable NG relation-contract violation."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code


def fail(code: str, message: str) -> None:
    raise RelationContractError(code, message)


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail("relation.document-invalid", f"expected mapping: {path}")
    return value


def schema_validate(document: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail("relation.schema-invalid", f"{label}: {error.message}")


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value)).hexdigest()


def canonical_relation_projection(relation: dict[str, Any]) -> dict[str, Any]:
    """Canonicalize registry collections whose insertion order is non-semantic."""
    canonical = copy.deepcopy(relation)
    references = canonical.setdefault("references", [])
    references.sort(
        key=lambda reference: (
            tuple(reference["source_columns"]),
            str(reference["strength"]),
            str(reference["target_relation"]),
            tuple(reference["target_columns"]),
        )
    )
    canonical["merge"].setdefault("conflict_columns", []).sort()
    constraints = canonical.get("row_constraints")
    if isinstance(constraints, dict):
        groups = constraints.get("all_or_none", [])
        constraints["all_or_none"] = sorted(
            (sorted(group) for group in groups), key=lambda group: tuple(group)
        )
    return canonical


def registry_semantic_projection(registry: dict[str, Any]) -> dict[str, Any]:
    """Exclude migration/document metadata and relation registration order."""
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
            (canonical_relation_projection(row) for row in registry["relations"]),
            key=lambda row: row["name"],
        ),
    }


def rows_by(rows: list[dict[str, Any]], field: str, label: str) -> dict[str, dict[str, Any]]:
    grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        grouped[row[field]].append(row)
    duplicates = sorted(key for key, values in grouped.items() if len(values) != 1)
    if duplicates:
        fail("relation.duplicate-id", f"{label} duplicates: {duplicates}")
    return {key: values[0] for key, values in grouped.items()}


def assert_acyclic(graph: dict[str, set[str]]) -> None:
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(node: str) -> None:
        if node in visiting:
            fail("relation.hard-reference-cycle", f"hard reference cycle includes {node}")
        if node in visited:
            return
        visiting.add(node)
        for target in sorted(graph[node]):
            visit(target)
        visiting.remove(node)
        visited.add(node)

    for node in sorted(graph):
        visit(node)


def _column_maps(
    relation: dict[str, Any],
) -> tuple[dict[str, dict[str, Any]], dict[str, dict[str, Any]]]:
    return (
        rows_by(relation["columns"], "id", f"{relation['name']} column IDs"),
        rows_by(relation["columns"], "name", f"{relation['name']} column names"),
    )


def _validate_row_constraints(
    relation: dict[str, Any], columns: dict[str, dict[str, Any]]
) -> None:
    constraints = relation.get("row_constraints")
    if constraints is None:
        return
    groups = constraints.get("all_or_none", [])
    canonical_groups: set[tuple[str, ...]] = set()
    constrained_columns: set[str] = set()
    for group in groups:
        if len(group) != len(set(group)):
            fail(
                "relation.row-constraint-duplicate-column",
                f"{relation['name']} all-or-none group repeats a column",
            )
        canonical = tuple(sorted(group))
        if canonical in canonical_groups:
            fail(
                "relation.row-constraint-duplicate",
                f"{relation['name']} repeats an all-or-none group",
            )
        canonical_groups.add(canonical)
        unknown = sorted(set(group) - set(columns))
        if unknown:
            fail(
                "relation.row-constraint-column-unknown",
                f"{relation['name']} all-or-none columns are unknown: {unknown}",
            )
        overlap = sorted(set(group) & constrained_columns)
        if overlap:
            fail(
                "relation.row-constraint-overlap",
                f"{relation['name']} all-or-none groups overlap: {overlap}",
            )
        constrained_columns.update(group)
        non_optional = sorted(
            identifier
            for identifier in group
            if columns[identifier]["required"]
            or not columns[identifier]["type"].startswith("optional<")
        )
        if non_optional:
            fail(
                "relation.row-constraint-column-not-optional",
                f"{relation['name']} all-or-none columns are not optional: {non_optional}",
            )


def _validate_clang22_observation_relations(
    relations: dict[str, dict[str, Any]],
) -> None:
    dynamic = {
        name
        for name, relation in relations.items()
        if relation.get("api_surface") == "dynamic_only"
    }
    if dynamic != CLANG22_OBSERVATION_RELATIONS:
        fail(
            "relation.dynamic-surface-set-invalid",
            "dynamic-only relation set differs from the accepted Clang 22 observation family",
        )

    base_specs = {
        "observation": ("typed_id<clang22_observation_id>", True, "claim_key"),
        "compile_unit": ("typed_id<compile_unit_id>", True, "authoritative_payload"),
        "semantic_key": ("bytes", True, "authoritative_payload"),
        "payload_digest": ("digest", True, "authoritative_payload"),
        "exact_equivalence": ("bool", True, "authoritative_payload"),
        "limitation": ("optional<utf8_string>", False, "authoritative_payload"),
    }
    span_specs = {
        "source": ("optional<typed_id<source_span_id>>", False, "authoritative_payload"),
        "source_snapshot": (
            "optional<typed_id<source_snapshot_id>>",
            False,
            "authoritative_payload",
        ),
        "source_file": ("optional<typed_id<file_id>>", False, "authoritative_payload"),
        "source_begin": ("optional<uint64>", False, "authoritative_payload"),
        "source_end": ("optional<uint64>", False, "authoritative_payload"),
        "source_role": (
            "optional<open_symbol<source.range-role/1>>",
            False,
            "authoritative_payload",
        ),
        "source_read_only": ("optional<bool>", False, "authoritative_payload"),
        "source_origin_chain": ("optional<bytes>", False, "authoritative_payload"),
    }
    source_targets = [
        "source.span.v1.span",
        "source.span.v1.snapshot",
        "source.span.v1.file",
        "source.span.v1.begin",
        "source.span.v1.end",
        "source.span.v1.role",
        "source.span.v1.read_only",
    ]

    for name in sorted(CLANG22_OBSERVATION_RELATIONS):
        relation = relations[name]
        prefix = f"{name}.v2."
        if (
            relation["descriptor_id"] != f"{name}.v2"
            or relation["version"] != "2.0.0"
            or relation["semantic_major"] != 2
            or relation["semantics"] != f"{name}/2"
            or relation["owner_namespace"] != "cxxlens.clang22.reference"
            or relation["stability"] != "versioned"
            or relation.get("api_surface") != "dynamic_only"
            or relation.get("generated_cpp_tag") is not None
        ):
            fail(
                "relation.observation-v2-authority-invalid",
                f"{name} descriptor/version/owner/dynamic authority differs",
            )

        has_span = name != "frontend.clang22.type_observation"
        expected_specs = dict(base_specs)
        if has_span:
            expected_specs.update(span_specs)
        _, columns_by_name = _column_maps(relation)
        missing_span = sorted(
            set(CLANG22_PRIMARY_SPAN_SUFFIXES) - set(columns_by_name)
        ) if has_span else []
        if missing_span:
            fail(
                "relation.observation-span-column-missing",
                f"{name} primary span columns are missing: {missing_span}",
            )
        if set(columns_by_name) != set(expected_specs):
            fail(
                "relation.observation-column-set-invalid",
                f"{name} observation column set differs",
            )
        for column_name, expected in expected_specs.items():
            column = columns_by_name[column_name]
            actual = (column["type"], column["required"], column["identity_role"])
            if actual != expected or column["id"] != prefix + column_name:
                fail(
                    "relation.observation-column-contract-invalid",
                    f"{name}.{column_name} column contract differs",
                )

        expected_projection = [
            prefix + "compile_unit",
            prefix + "semantic_key",
            prefix + "payload_digest",
        ]
        if has_span:
            expected_projection.extend(prefix + suffix for suffix in CLANG22_PRIMARY_SPAN_SUFFIXES)
            expected_projection.append(prefix + "source_origin_chain")
        claim = relation["claim"]
        if (
            claim["key"] != [prefix + "observation"]
            or claim["domain_identity"]["result_column"] != prefix + "observation"
            or claim["domain_identity"]["projection"] != expected_projection
        ):
            fail(
                "relation.observation-identity-invalid",
                f"{name} observation identity projection differs",
            )

        compile_reference = {
            "source_columns": [prefix + "compile_unit"],
            "target_relation": "build.compile_unit",
            "target_columns": ["build.compile_unit.v1.compile_unit"],
            "strength": "hard",
            "on_missing": "reject_batch",
        }
        expected_references = [compile_reference]
        if has_span:
            expected_references.append(
                {
                    "source_columns": [
                        prefix + suffix for suffix in CLANG22_PRIMARY_SPAN_SUFFIXES
                    ],
                    "target_relation": "source.span",
                    "target_columns": source_targets,
                    "strength": "hard",
                    "on_missing": "reject_batch",
                }
            )
        canonical_references = sorted(
            relation["references"], key=lambda row: canonical_json(row)
        )
        if canonical_references != sorted(
            expected_references, key=lambda row: canonical_json(row)
        ):
            fail(
                "relation.observation-span-reference-invalid"
                if has_span
                else "relation.observation-reference-invalid",
                f"{name} hard-reference projection differs",
            )

        constraints = relation.get("row_constraints")
        if has_span:
            expected_group = sorted(
                prefix + suffix for suffix in CLANG22_PRIMARY_SPAN_SUFFIXES
            )
            actual_groups = (
                []
                if constraints is None
                else sorted(sorted(group) for group in constraints["all_or_none"])
            )
            if actual_groups != [expected_group]:
                fail(
                    "relation.observation-span-constraint-invalid",
                    f"{name} primary span all-or-none constraint differs",
                )
        elif constraints is not None:
            fail(
                "relation.observation-type-span-forbidden",
                "type observation must not carry a primary span constraint",
            )

        if (
            relation["partition"]["suggested_keys"] != [prefix + "compile_unit"]
            or relation["provenance"]["minimum"] != "direct_observation"
            or relation["evolution_policy"] != "ng0.additive.v1"
        ):
            fail(
                "relation.observation-policy-invalid",
                f"{name} partition/provenance/evolution policy differs",
            )


def validate_envelope(registry: dict[str, Any]) -> None:
    envelope = registry["system_claim_envelope"]
    columns = rows_by(envelope["columns"], "id", "claim envelope column IDs")
    by_name = rows_by(envelope["columns"], "name", "claim envelope column names")
    expected_names = {
        "descriptor",
        "semantic_key",
        "assertion",
        "content",
        "presence",
        "interpretation",
        "stage",
        "producer",
        "producer_input_basis",
        "provenance_root",
        "guarantee",
    }
    if set(by_name) != expected_names:
        fail("relation.envelope-incomplete", "system claim envelope column set differs")
    if by_name["presence"]["type"] != "condition_ref":
        fail("relation.envelope-condition-invalid", "presence must be condition_ref")
    if by_name["producer_input_basis"]["type"] != "tagged<producer-input-basis/1>":
        fail(
            "relation.envelope-producer-basis-invalid",
            "producer input basis must use the tagged store contract",
        )
    if envelope["condition_authority"] != "envelope-presence-only":
        fail("relation.envelope-condition-invalid", "envelope is not condition authority")
    if envelope["instance_schema"] != CLAIM_ENVELOPE_SCHEMA.as_posix():
        fail("relation.envelope-schema-invalid", "claim envelope instance schema differs")
    if len(columns) != len(envelope["columns"]):
        fail("relation.duplicate-id", "claim envelope column IDs are not unique")
    identity = envelope["identity"]
    if "canonical-condition" not in identity["assertion"]:
        fail("relation.identity-condition-missing", "assertion ID omits condition")
    if "condition" in identity["semantic_key"]:
        fail("relation.identity-cycle", "semantic key includes condition")


def validate_registry(
    registry: dict[str, Any],
    *,
    schema: dict[str, Any] | None = None,
) -> dict[str, dict[str, Any]]:
    static_projection = registry.get("api_projection", {}).get("static")
    if static_projection != {
        "descriptor_source": "relations[generated_cpp_tag!=null].descriptor_id",
        "column_source": "relations[generated_cpp_tag!=null].columns[].id",
    }:
        fail(
            "relation.static-projection-includes-dynamic",
            "static projection does not exclude dynamic-only descriptors",
        )
    if schema is not None:
        schema_validate(registry, schema, "relation registry")
    validate_envelope(registry)

    relations = rows_by(registry["relations"], "name", "relation names")
    rows_by(registry["relations"], "descriptor_id", "relation descriptor IDs")
    missing = sorted(REQUIRED_RELATIONS - set(relations))
    if missing:
        fail("relation.required-relation-missing", f"required relations missing: {missing}")

    generated_tags: list[str] = []
    for name, relation in relations.items():
        tag = relation.get("generated_cpp_tag")
        surface = relation.get("api_surface")
        if surface is None and isinstance(tag, str):
            generated_tags.append(tag)
        elif surface == "dynamic_only" and tag is None:
            pass
        else:
            fail(
                "relation.api-surface-invalid",
                f"{name} generated C++ tag/dynamic-only classification differs",
            )
    duplicate_tags = sorted(
        tag for tag in set(generated_tags) if generated_tags.count(tag) != 1
    )
    if duplicate_tags:
        fail("relation.duplicate-id", f"generated C++ tags duplicate: {duplicate_tags}")

    evolution = rows_by(registry["evolution_policies"], "id", "evolution policies")
    symbols = rows_by(registry["symbol_contracts"], "id", "symbol contracts")
    all_column_ids: set[str] = {
        column["id"] for column in registry["system_claim_envelope"]["columns"]
    }
    relation_columns: dict[str, dict[str, dict[str, Any]]] = {}

    for name, relation in relations.items():
        major = int(relation["version"].split(".", 1)[0])
        expected_descriptor = f"{name}.v{major}"
        if relation["semantic_major"] != major or relation["descriptor_id"] != expected_descriptor:
            fail(
                "relation.descriptor-id-invalid",
                f"{name} descriptor ID/version/semantic major disagree",
            )
        columns, names = _column_maps(relation)
        relation_columns[name] = columns
        _validate_row_constraints(relation, columns)
        duplicate_global = sorted(set(columns) & all_column_ids)
        if duplicate_global:
            fail("relation.duplicate-id", f"global column IDs duplicate: {duplicate_global}")
        all_column_ids.update(columns)
        prefix = f"{name}.v{major}."
        if any(not identifier.startswith(prefix) for identifier in columns):
            fail("relation.column-id-invalid", f"{name} column ID has another owner")
        if "presence" in names or any(
            column["type"] == "condition_ref" for column in columns.values()
        ):
            fail(
                "relation.condition-duplicated",
                f"{name} duplicates envelope presence in user columns",
            )

        key = set(relation["claim"]["key"])
        if not key.issubset(columns):
            fail("relation.key-column-unknown", f"{name} key names unknown columns")
        role_key = {
            identifier
            for identifier, column in columns.items()
            if column["identity_role"] == "claim_key"
        }
        if key != role_key:
            fail("relation.key-role-mismatch", f"{name} key and claim_key roles differ")

        for key_column in relation["partition"]["suggested_keys"]:
            if key_column not in columns:
                fail(
                    "relation.partition-column-unknown",
                    f"{name} partition references {key_column}",
                )
        for index in relation["indexes"]:
            if not set(index).issubset(columns):
                fail("relation.index-column-unknown", f"{name} index names unknown column")

        domain = relation["claim"]["domain_identity"]
        result = domain["result_column"]
        projection = set(domain["projection"])
        if result is not None:
            if result not in columns:
                fail("relation.identity-column-unknown", f"{name} result ID is unknown")
            if result in projection:
                fail("relation.identity-cycle", f"{name} ID projection contains its result")
            if result not in key:
                fail("relation.identity-key-mismatch", f"{name} result ID is not a key")
        if not projection.issubset(columns):
            fail("relation.identity-column-unknown", f"{name} ID projection is unknown")
        display = {
            identifier
            for identifier, column in columns.items()
            if column["identity_role"] == "display"
        }
        if projection & display:
            fail("relation.identity-display-field", f"{name} ID uses display fields")

        cardinality = relation["claim"]["cardinality"]
        merge = relation["merge"]
        expected_mode = "functional_assertion" if cardinality == "functional_assertion" else "set"
        if merge["mode"] != expected_mode:
            fail("relation.merge-cardinality-mismatch", f"{name} merge mode differs")
        conflicts = set(merge["conflict_columns"])
        if not conflicts.issubset(columns):
            fail("relation.conflict-column-unknown", f"{name} conflict column unknown")
        authoritative_payload = {
            identifier
            for identifier, column in columns.items()
            if column["identity_role"] == "authoritative_payload"
        }
        if cardinality == "functional_assertion" and conflicts != authoritative_payload:
            fail(
                "relation.conflict-projection-incomplete",
                f"{name} conflict projection must include every authoritative payload column",
            )
        if cardinality == "multivalued" and conflicts:
            fail("relation.multivalue-conflict-invalid", f"{name} set has conflict columns")
        if relation["evolution_policy"] not in evolution:
            fail("relation.evolution-policy-unknown", f"{name} evolution policy missing")

        for column in columns.values():
            match = re.fullmatch(r"closed_symbol<([^>]+)>", column["type"])
            if match and match.group(1) not in symbols:
                fail(
                    "relation.closed-symbol-contract-missing",
                    f"{name}.{column['name']} lacks a symbol contract",
                )

    hard_graph = {name: set() for name in relations}
    for name, relation in relations.items():
        columns = relation_columns[name]
        for reference in relation["references"]:
            if not set(reference["source_columns"]).issubset(columns):
                fail(
                    "relation.reference-column-unknown",
                    f"{name} reference source is unknown",
                )
            target_name = reference["target_relation"]
            if target_name not in relations:
                fail(
                    "relation.reference-target-unknown",
                    f"{name} references unknown relation {target_name}",
                )
            target_columns = relation_columns[target_name]
            if not set(reference["target_columns"]).issubset(target_columns):
                fail(
                    "relation.reference-column-unknown",
                    f"{name} reference target column is unknown",
                )
            if reference["strength"] == "hard":
                if reference["on_missing"] != "reject_batch":
                    fail("relation.hard-reference-policy", f"{name} hard reference is not rejecting")
                hard_graph[name].add(target_name)
            elif reference["on_missing"] != "unresolved":
                fail("relation.soft-reference-policy", f"{name} soft reference is not accounted")
    assert_acyclic(hard_graph)
    _validate_clang22_observation_relations(relations)

    call_site = relations["cc.call_site"]
    _, call_names = _column_maps(call_site)
    if "compile_unit" not in call_names:
        fail("relation.partition-column-unknown", "cc.call_site lacks compile_unit")
    if "direct_target" in call_names:
        fail("relation.call-model-duplicated", "direct target is embedded in cc.call_site")
    direct = relations["cc.call_direct_target"]
    _, direct_names = _column_maps(direct)
    if set(direct_names) != {"call", "target", "resolution"}:
        fail("relation.call-model-invalid", "cc.call_direct_target columns differ")
    entity = relations["cc.entity"]
    entity_projection = entity["claim"]["domain_identity"]["projection"]
    if "cc.entity.v1.anchor" in entity_projection:
        fail(
            "relation.entity-occurrence-in-identity",
            "cc.entity semantic identity includes a declaration/definition occurrence anchor",
        )
    if "cc.entity.v1.provider_local_key" not in entity_projection:
        fail("relation.entity-semantic-key-missing", "cc.entity identity lacks semantic key")
    entity_columns, _ = _column_maps(entity)
    if entity_columns["cc.entity.v1.anchor"]["identity_role"] != "display":
        fail("relation.entity-anchor-authoritative", "cc.entity anchor is not occurrence-only")
    if "cc.entity.v1.anchor" in entity["merge"]["conflict_columns"]:
        fail("relation.entity-anchor-conflict", "cc.entity merge conflicts on occurrence anchor")
    if relations["company.lock.acquire"]["stability"] != "external-versioned":
        fail("relation.extension-not-external", "external exemplar is not external-versioned")
    return relations


def apply_patches(document: dict[str, Any], patches: list[dict[str, Any]]) -> dict[str, Any]:
    result = copy.deepcopy(document)
    for patch in patches:
        parts = [part.replace("~1", "/").replace("~0", "~") for part in patch["path"].split("/")[1:]]
        if not parts:
            fail("relation.vector-invalid", "patch cannot replace the document root")
        parent: Any = result
        for part in parts[:-1]:
            parent = parent[int(part)] if isinstance(parent, list) else parent[part]
        leaf = parts[-1]
        operation = patch["op"]
        if operation == "add":
            if isinstance(parent, list):
                if leaf == "-":
                    parent.append(copy.deepcopy(patch["value"]))
                else:
                    parent.insert(int(leaf), copy.deepcopy(patch["value"]))
            else:
                parent[leaf] = copy.deepcopy(patch["value"])
        elif operation == "remove":
            if isinstance(parent, list):
                parent.pop(int(leaf))
            else:
                del parent[leaf]
        elif operation == "replace":
            if isinstance(parent, list):
                parent[int(leaf)] = copy.deepcopy(patch["value"])
            else:
                parent[leaf] = copy.deepcopy(patch["value"])
        else:
            fail("relation.vector-invalid", f"unknown patch operation {operation}")
    return result


def resolve_reference(
    registry: dict[str, Any],
    input_value: dict[str, Any],
) -> tuple[str, str]:
    relation = next(
        row for row in registry["relations"] if row["name"] == input_value["relation"]
    )
    matches = [
        reference
        for reference in relation["references"]
        if input_value["source_column"] in reference["source_columns"]
    ]
    if len(matches) != 1:
        fail("relation.vector-invalid", "reference vector does not select exactly one reference")
    reference = matches[0]
    if input_value["source_value"] in input_value["available_target_values"]:
        return "accepted", "relation.reference-resolved"
    if reference["strength"] == "hard":
        return "rejected", "relation.hard-reference-missing"
    if input_value["unresolved_accounted"]:
        return "accepted", "relation.soft-reference-unresolved"
    return "rejected", "relation.soft-reference-unaccounted"


def validate_symbol(
    registry: dict[str, Any],
    input_value: dict[str, Any],
) -> tuple[str, str]:
    match = re.fullmatch(r"(open|closed)_symbol<([^>]+)>", input_value["type"])
    if match is None:
        fail("relation.vector-invalid", "symbol vector type is invalid")
    openness, contract_id = match.groups()
    contracts = {row["id"]: row for row in registry["symbol_contracts"]}
    contract = contracts.get(contract_id)
    if openness == "open":
        if contract is not None and contract["openness"] != "open":
            fail("relation.symbol-openness-mismatch", f"{contract_id} is not open")
        return "accepted", "relation.open-symbol-preserved"
    if contract is None or contract["openness"] != "closed":
        fail("relation.closed-symbol-contract-missing", f"{contract_id} is not closed")
    if input_value["value"] not in contract["values"]:
        return "rejected", "relation.closed-symbol-unknown"
    return "accepted", "relation.closed-symbol-known"


def validate_scalar(input_value: dict[str, Any]) -> tuple[str, str]:
    scalar_type = input_value["type"]
    value = input_value["value"]
    valid = False
    if scalar_type == "digest":
        valid = re.fullmatch(
            r"(?:sha256|semantic-v2:sha256):[0-9a-f]{64}", value
        ) is not None
    elif scalar_type == "semantic_version":
        match = re.fullmatch(
            r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)",
            value,
        )
        valid = match is not None and all(
            int(component) <= 0xFFFFFFFF for component in match.groups()
        )
    elif re.fullmatch(r"typed_id<[a-z][a-z0-9_]*_id>", scalar_type):
        valid = bool(value) and all(
            ord(character) >= 0x20 and ord(character) != 0x7F
            for character in value
        )
    elif scalar_type.startswith("set<") and scalar_type.endswith(">"):
        valid = value == value.lower() and len(value) % 2 == 0
        try:
            encoded = bytes.fromhex(value)
        except ValueError:
            valid = False
            encoded = b""
        offset = 0
        previous: bytes | None = None
        while valid and offset < len(encoded):
            if len(encoded) - offset < 4:
                valid = False
                break
            length = int.from_bytes(encoded[offset : offset + 4], "little")
            offset += 4
            if length == 0 or length > len(encoded) - offset:
                valid = False
                break
            element = encoded[offset : offset + length]
            offset += length
            if previous is not None and previous >= element:
                valid = False
                break
            try:
                text = element.decode("utf-8")
            except UnicodeDecodeError:
                valid = False
                break
            if not text or any(
                ord(character) < 0x20 or ord(character) == 0x7F
                for character in text
            ):
                valid = False
                break
            previous = element
    return (
        ("accepted", "relation.scalar-valid")
        if valid
        else ("rejected", "relation.scalar-invalid")
    )


def semver(value: str) -> tuple[int, int, int]:
    match = re.fullmatch(r"([0-9]+)\.([0-9]+)\.([0-9]+)", value)
    if match is None:
        fail("relation.vector-invalid", f"invalid semantic version {value}")
    return tuple(int(part) for part in match.groups())  # type: ignore[return-value]


def validate_evolution(
    registry: dict[str, Any],
    input_value: dict[str, Any],
) -> tuple[str, str]:
    relations = {row["name"]: row for row in registry["relations"]}
    relation = relations[input_value["relation"]]
    policies = {row["id"]: row for row in registry["evolution_policies"]}
    policy = policies[relation["evolution_policy"]]
    before = semver(input_value["from_version"])
    after = semver(input_value["to_version"])
    if before[0] != after[0] or after <= before:
        return "rejected", "relation.major-change-required"
    change = input_value["change"]
    column = input_value["column"]
    if (
        change in policy["minor"]
        and change == "optional-column"
        and column["required"] is False
        and input_value["old_reader_behavior"] == policy["old_reader_unknown_optional"]
    ):
        return "accepted", "relation.minor-additive"
    return "rejected", "relation.major-change-required"


def compare_api_ids(input_value: dict[str, Any]) -> tuple[str, str]:
    if (
        input_value["static_descriptor_id"] == input_value["dynamic_descriptor_id"]
        and input_value["static_column_id"] == input_value["dynamic_column_id"]
    ):
        return "accepted", "relation.api-id-parity"
    return "rejected", "relation.api-id-mismatch"


def _replace_strings(value: Any, old: str, new: str) -> Any:
    if isinstance(value, str):
        return value.replace(old, new)
    if isinstance(value, list):
        return [_replace_strings(item, old, new) for item in value]
    if isinstance(value, dict):
        return {key: _replace_strings(item, old, new) for key, item in value.items()}
    return value


def register_extension(
    registry: dict[str, Any],
    input_value: dict[str, Any],
    schema: dict[str, Any],
) -> tuple[str, str]:
    code_changes = sum(
        input_value[field]
        for field in (
            "central_enum_changes",
            "central_switch_changes",
            "central_source_list_changes",
        )
    )
    if code_changes:
        return "rejected", "relation.extension-code-change"
    exemplar = next(
        row for row in registry["relations"] if row["name"] == input_value["exemplar"]
    )
    old_name = exemplar["name"]
    new_name = input_value["new_name"]
    extension = _replace_strings(copy.deepcopy(exemplar), old_name, new_name)
    extension["name"] = new_name
    extension["descriptor_id"] = f"{new_name}.v{extension['semantic_major']}"
    extension["owner_namespace"] = input_value["owner_namespace"]
    extension["semantics"] = f"{new_name}/1"
    extension["generated_cpp_tag"] = (
        "cxxlens::" + "::".join(new_name.split("."))
    )
    candidate = copy.deepcopy(registry)
    candidate["relations"].append(extension)
    validate_registry(candidate, schema=schema)
    return "accepted", "relation.extension-code-diff-zero"


def validate_query_columns(
    registry: dict[str, Any],
    input_value: dict[str, Any],
) -> tuple[str, str]:
    relations = {row["name"]: row for row in registry["relations"]}
    for operand in input_value["columns"]:
        relation = relations.get(operand["relation"])
        if relation is None:
            return "rejected", "relation.query-relation-unknown"
        names = {column["name"] for column in relation["columns"]}
        if operand["column"] not in names:
            return "rejected", "relation.query-column-unknown"
    return "accepted", "relation.query-columns-valid"


def execute_vector(
    registry: dict[str, Any],
    registry_schema: dict[str, Any],
    vector: dict[str, Any],
) -> tuple[str, str]:
    operation = vector["operation"]
    input_value = vector["input"]
    if operation == "validate_registry":
        candidate = apply_patches(registry, input_value["patches"])
        try:
            validate_registry(candidate, schema=registry_schema)
        except RelationContractError as error:
            return "rejected", error.code
        return "accepted", "relation.registry-valid"
    if operation == "validate_envelope_instance":
        try:
            schema_validate(
                input_value["document"],
                load_yaml(ROOT / CLAIM_ENVELOPE_SCHEMA),
                "claim envelope instance",
            )
        except RelationContractError:
            return "rejected", "relation.envelope-instance-invalid"
        return "accepted", "relation.envelope-instance-valid"
    if operation == "resolve_reference":
        return resolve_reference(registry, input_value)
    if operation == "validate_symbol":
        return validate_symbol(registry, input_value)
    if operation == "validate_scalar":
        return validate_scalar(input_value)
    if operation == "validate_evolution":
        return validate_evolution(registry, input_value)
    if operation == "compare_api_ids":
        return compare_api_ids(input_value)
    if operation == "register_extension":
        return register_extension(registry, input_value, registry_schema)
    if operation == "validate_query_columns":
        return validate_query_columns(registry, input_value)
    fail("relation.vector-invalid", f"unknown vector operation {operation}")


def validate_vectors(
    registry: dict[str, Any],
    registry_schema: dict[str, Any],
    vectors: dict[str, Any],
) -> list[dict[str, str]]:
    by_id = rows_by(vectors["vectors"], "id", "conformance vector IDs")
    if set(by_id) != REQUIRED_VECTOR_IDS:
        fail("relation.vector-set-invalid", "required conformance vector set differs")
    results = []
    for vector in vectors["vectors"]:
        decision, reason_code = execute_vector(registry, registry_schema, vector)
        expected = vector["expected"]
        if decision != expected["decision"] or reason_code != expected["reason_code"]:
            fail(
                "relation.vector-result-mismatch",
                f"{vector['id']} returned {decision}/{reason_code}, expected "
                f"{expected['decision']}/{expected['reason_code']}",
            )
        if (vector["class"] == "positive") != (decision == "accepted"):
            fail("relation.vector-class-mismatch", f"{vector['id']} class differs")
        results.append(
            {
                "id": vector["id"],
                "class": vector["class"],
                "decision": decision,
                "reason_code": reason_code,
            }
        )
    return results


def validate_design(root: pathlib.Path) -> None:
    design = (root / "docs/design/cxxlens_next_generation_integrated_design_ja.md").read_text(
        encoding="utf-8"
    )
    required = (
        "1.0.0-normative",
        "schemas/cxxlens_ng_relation_registry.yaml",
        "envelope-presence-only",
        "cc.call_direct_target",
        "Issue #60",
        "detached-cell-value-v2",
        "Issue #152",
        "cross-TU semantic entity identity",
    )
    for marker in required:
        if marker not in design:
            fail("relation.design-marker-missing", f"design marker missing: {marker}")
    for stale in ("col<call::direct_target>()", 'calls.column("direct_target")'):
        if stale in design:
            fail("relation.call-example-stale", f"stale call model remains: {stale}")
    index = (root / "docs/design/catalogs/README.md").read_text(encoding="utf-8")
    if "accepted exact scalar-value and cross-TU entity identity contract" not in index or "#152" not in index:
        fail("relation.catalog-index-stale", "catalog index does not show accepted #152")


def validate_contract(root: pathlib.Path) -> tuple[dict[str, Any], list[dict[str, str]]]:
    registry = load_yaml(root / REGISTRY)
    registry_schema = load_yaml(root / REGISTRY_SCHEMA)
    vectors = load_yaml(root / VECTORS)
    claim_schema = load_yaml(root / CLAIM_ENVELOPE_SCHEMA)
    schema_validate(registry, registry_schema, "relation registry")
    try:
        jsonschema.Draft202012Validator.check_schema(claim_schema)
    except jsonschema.SchemaError as error:
        fail("relation.schema-invalid", f"claim envelope schema: {error.message}")
    schema_validate(vectors, load_yaml(root / VECTORS_SCHEMA), "relation vectors")
    validate_registry(registry)
    results = validate_vectors(registry, registry_schema, vectors)
    validate_design(root)
    return registry, results


def make_report(
    registry: dict[str, Any],
    vector_results: list[dict[str, str]],
) -> dict[str, Any]:
    descriptors = [
        {
            "name": row["name"],
            "descriptor_id": row["descriptor_id"],
            "version": row["version"],
            "digest": digest(canonical_relation_projection(row)),
        }
        for row in sorted(registry["relations"], key=lambda item: item["name"])
    ]
    return {
        "schema": "cxxlens.relation-conformance-report.v1",
        "status": "green",
        "registry": REGISTRY.as_posix(),
        "registry_digest": digest(registry_semantic_projection(registry)),
        "descriptors": descriptors,
        "vectors": vector_results,
    }


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    registry, results = validate_contract(root)
    report = make_report(registry, results)
    schema_validate(report, load_yaml(root / REPORT_SCHEMA), "relation report")
    if args.mode == "report":
        if args.output is None:
            fail("relation.output-missing", "report mode requires --output")
        output = args.output if args.output.is_absolute() else root / args.output
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(
            json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    print(
        "NG relation contract passed: "
        f"{len(registry['relations'])} descriptors, {len(results)} vectors, "
        f"{report['registry_digest']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        RelationContractError,
        json.JSONDecodeError,
        jsonschema.ValidationError,
        OSError,
        yaml.YAMLError,
    ) as error:
        print(f"NG relation contract failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
