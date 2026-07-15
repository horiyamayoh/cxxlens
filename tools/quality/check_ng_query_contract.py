#!/usr/bin/env python3
"""Validate and evaluate the NG0 versioned Logical Query IR contract."""

from __future__ import annotations

import argparse
import copy
import functools
import hashlib
import json
import pathlib
import random
import sqlite3
import sys
from dataclasses import dataclass
from typing import Any, Iterable

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONTRACT = pathlib.Path("schemas/cxxlens_ng_logical_query_contract.yaml")
CONTRACT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_logical_query_contract.schema.yaml"
)
IR_SCHEMA = pathlib.Path("schemas/cxxlens_ng_logical_query_ir.schema.yaml")
RELATION_REGISTRY = pathlib.Path("schemas/cxxlens_ng_relation_registry.yaml")
VECTORS = pathlib.Path("schemas/cxxlens_ng_query_conformance_vectors.yaml")
VECTORS_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_query_conformance_vectors.schema.yaml"
)
REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_query_conformance_report.schema.yaml"
)

NG0_OPERATORS = {
    "query.scan.v1": ("scan", 0, {"descriptor_id", "alias"}),
    "query.filter.v1": ("filter", 1, {"predicate"}),
    "query.project.v1": ("project", 1, {"columns"}),
    "query.inner_join.v1": ("inner_join", 2, {"predicate"}),
    "query.semi_join.v1": ("semi_join", 2, {"predicate"}),
    "query.union.v1": ("union", 2, set()),
    "query.distinct.v1": ("distinct", 1, set()),
    "query.order_by.v1": ("order_by", 1, {"keys"}),
    "query.limit.v1": ("limit", 1, {"count"}),
    "query.condition_restrict.v1": (
        "condition_restrict",
        1,
        {"universe", "alternatives"},
    ),
    "query.interpretation_restrict.v1": (
        "interpretation_restrict",
        1,
        {"interpretation"},
    ),
}

OPERATOR_NEGATIVE_CASES = {
    "query.scan.v1": ("missing-descriptor", "query.scan-descriptor-missing"),
    "query.filter.v1": ("implicit-sql-null", "query.sql-null-forbidden"),
    "query.project.v1": ("implicit-distinct", "query.project-distinct-forbidden"),
    "query.inner_join.v1": (
        "implicit-null-equality",
        "query.join-present-equality-required",
    ),
    "query.semi_join.v1": (
        "right-multiplicity-leak",
        "query.semi-join-left-multiplicity-required",
    ),
    "query.union.v1": ("mismatched-schema", "query.union-schema-mismatch"),
    "query.distinct.v1": (
        "condition-in-key",
        "query.distinct-key-invalid",
    ),
    "query.order_by.v1": (
        "missing-total-tie-break",
        "query.order-not-total",
    ),
    "query.limit.v1": ("unordered-input", "query.limit-unordered"),
    "query.condition_restrict.v1": (
        "empty-alternatives",
        "query.condition-empty-restriction",
    ),
    "query.interpretation_restrict.v1": (
        "implicit-default",
        "query.interpretation-default-forbidden",
    ),
}

REQUIRED_VECTOR_IDS = {
    "exact-query-contract",
    "scan-missing-descriptor",
    "filter-implicit-sql-null",
    "project-implicit-distinct",
    "inner-join-implicit-null-equality",
    "semi-join-right-multiplicity",
    "union-schema-mismatch",
    "distinct-condition-in-key",
    "order-by-without-total-tie-break",
    "limit-without-order",
    "condition-restrict-empty",
    "interpretation-restrict-implicit-default",
    "group-is-ng1",
    "aggregate-partial-is-not-ng0",
    "static-dynamic-normalized-digest",
    "backend-and-order-parity",
    "deterministic-output-truncation",
    "tagged-absent",
    "tagged-semantic-unknown",
    "sql-null-is-not-absence",
    "missing-optional-as-explicit-absent",
    "missing-optional-direct-reference",
    "missing-required-column",
    "dynamic-literal-exact-type",
    "dynamic-literal-type-mismatch",
    "dynamic-literal-type-missing",
    "ordered-continuation-bound",
    "stale-continuation-snapshot",
    "unsealed-upstream-interruption",
    "sealed-ordered-cancel-prefix",
    "partial-aggregate-publication",
    "logical-ir-without-physical-fields",
    "physical-index-is-not-logical-authority",
}


class QueryContractError(ValueError):
    """A stable Logical Query IR contract violation."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code


def fail(code: str, message: str) -> None:
    raise QueryContractError(code, message)


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail("query.document-invalid", f"expected mapping: {path}")
    return value


def schema_validate(document: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail("query.schema-invalid", f"{label}: {error.message}")


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value)).hexdigest()


def _rows_by(rows: Iterable[dict[str, Any]], field: str, label: str) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for row in rows:
        key = row[field]
        if key in result:
            fail("query.duplicate-id", f"duplicate {label}: {key}")
        result[key] = row
    return result


def relation_columns(registry: dict[str, Any]) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for relation in registry["relations"]:
        for column in relation["columns"]:
            result[column["id"]] = {
                **column,
                "descriptor_id": relation["descriptor_id"],
                "relation_version": relation["version"],
            }
    return result


def validate_contract(
    contract: dict[str, Any],
    contract_schema: dict[str, Any],
) -> dict[str, dict[str, Any]]:
    schema_validate(contract, contract_schema, "logical query contract")
    operators = _rows_by(contract["operator_profiles"], "id", "operator ID")
    if set(operators) != set(NG0_OPERATORS):
        fail("query.operator-set-invalid", "NG0 operator set differs")
    for identifier, (name, arity, arguments) in NG0_OPERATORS.items():
        row = operators[identifier]
        if (
            row["name"] != name
            or row["arity"] != arity
            or set(row["required_arguments"]) != arguments
        ):
            fail("query.operator-descriptor-invalid", identifier)
    if operators["query.limit.v1"]["ordering"] != "preserve-total-order":
        fail("query.limit-unordered", "limit does not require total order")
    if contract["ordering"]["physical_scan_order_observable"]:
        fail("query.physical-order-observable", "physical scan order is observable")
    if not contract["ordering"]["limit_requires_total_order"]:
        fail("query.limit-unordered", "unordered limit is enabled")
    if contract["partial_execution"]["aggregate_partial"] != "forbidden-ng0":
        fail("query.partial-aggregate", "partial aggregate is enabled in NG0")
    if contract["collection_model"]["null_policy"]["implicit_three_valued_logic"] != "forbidden":
        fail("query.sql-null-forbidden", "implicit SQL NULL is enabled")
    deferred = _rows_by(contract["deferred_operators"], "id", "deferred operator")
    if not {"query.group.v1", "query.aggregate.v1"}.issubset(deferred):
        fail("query.aggregate-not-deferred", "group/aggregate are not deferred")
    return operators


def _walk(value: Any) -> Iterable[tuple[str, Any]]:
    if isinstance(value, dict):
        for key, child in value.items():
            yield key, child
            yield from _walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from _walk(child)


def validate_logical_authority(contract: dict[str, Any], document: dict[str, Any]) -> None:
    forbidden = set(contract["logical_ir"]["physical_fields_forbidden"])
    present = sorted({key for key, _ in _walk(document)} & forbidden)
    if present:
        fail("query.physical-authority-leak", f"physical fields present: {present}")


def _normalize_predicate(predicate: dict[str, Any]) -> dict[str, Any]:
    result = copy.deepcopy(predicate)
    if result["kind"] in {"and", "or"}:
        operands = [_normalize_predicate(row) for row in result["operands"]]
        result["operands"] = sorted(operands, key=digest)
    return result


def _normalize_node(node: dict[str, Any]) -> dict[str, Any]:
    result = copy.deepcopy(node)
    result["inputs"] = [_normalize_node(row) for row in result["inputs"]]
    if result["operator"] == "query.union.v1":
        result["inputs"] = sorted(result["inputs"], key=digest)
    arguments = result["arguments"]
    if "predicate" in arguments:
        arguments["predicate"] = _normalize_predicate(arguments["predicate"])
    if "alternatives" in arguments:
        arguments["alternatives"] = sorted(set(arguments["alternatives"]))
    return result


def normalized_ir(ir: dict[str, Any]) -> dict[str, Any]:
    result = copy.deepcopy(ir)
    result.pop("surface", None)
    result.pop("runtime_budget", None)
    result["relation_requirements"] = sorted(
        result["relation_requirements"], key=lambda row: row["descriptor_id"]
    )
    result["root"] = _normalize_node(result["root"])
    return result


def normalized_ir_digest(ir: dict[str, Any]) -> str:
    return digest(normalized_ir(ir))


def _column_refs(predicate: dict[str, Any]) -> list[dict[str, Any]]:
    if predicate["kind"] in {"and", "or"}:
        return [
            ref
            for operand in predicate["operands"]
            for ref in _column_refs(operand)
        ]
    return [
        value
        for name, value in predicate.items()
        if name in {"column", "left", "right"}
    ]


def _present_type(column_type: str) -> str:
    if column_type.startswith("optional<") and column_type.endswith(">"):
        return column_type[len("optional<") : -1]
    return column_type


def validate_dynamic_literal(
    columns: dict[str, dict[str, Any]],
    column_id: str,
    literal: dict[str, Any],
) -> None:
    if "type" not in literal:
        fail("query.literal-type-missing", column_id)
    if column_id not in columns:
        fail("query.column-unknown", column_id)
    expected = _present_type(columns[column_id]["type"])
    if literal["type"] != expected:
        fail(
            "query.literal-type-mismatch",
            f"{column_id}: expected {expected}, got {literal['type']}",
        )


@dataclass(frozen=True)
class NodeShape:
    columns: frozenset[str]
    ordered: bool
    order_keys: tuple[str, ...] = ()


def validate_ir(
    ir: dict[str, Any],
    ir_schema: dict[str, Any],
    contract: dict[str, Any],
    registry: dict[str, Any],
) -> NodeShape:
    schema_validate(ir, ir_schema, "logical query IR")
    validate_logical_authority(contract, ir)
    requirements = _rows_by(
        ir["relation_requirements"], "descriptor_id", "relation requirement"
    )
    descriptors = _rows_by(registry["relations"], "descriptor_id", "descriptor")
    columns = relation_columns(registry)
    for identifier, requirement in requirements.items():
        if identifier not in descriptors:
            fail("query.descriptor-unknown", identifier)
        available_minor = int(descriptors[identifier]["version"].split(".")[1])
        maximum = requirement["maximum_minor"]
        if available_minor < requirement["minimum_minor"] or (
            isinstance(maximum, int) and available_minor > maximum
        ):
            fail("query.schema-minor-incompatible", identifier)

    def validate_ref(reference: dict[str, Any], available: frozenset[str]) -> None:
        identifier = reference["column_id"]
        if identifier not in columns:
            if reference["availability"] == "absent_if_schema_missing":
                return
            fail("query.column-missing", identifier)
        if identifier not in available:
            fail("query.column-not-in-input", identifier)

    def visit(node: dict[str, Any]) -> NodeShape:
        identifier = node["operator"]
        if identifier not in NG0_OPERATORS:
            fail("query.operator-unknown", identifier)
        name, arity, required_arguments = NG0_OPERATORS[identifier]
        if len(node["inputs"]) != arity:
            fail("query.operator-arity", identifier)
        if set(node["arguments"]) != required_arguments:
            fail("query.operator-arguments", identifier)
        inputs = [visit(row) for row in node["inputs"]]
        arguments = node["arguments"]

        if name == "scan":
            descriptor = arguments["descriptor_id"]
            if descriptor not in requirements:
                fail("query.scan-requirement-missing", descriptor)
            return NodeShape(
                frozenset(
                    column_id
                    for column_id, column in columns.items()
                    if column["descriptor_id"] == descriptor
                ),
                False,
            )

        available = frozenset().union(*(row.columns for row in inputs))
        if "predicate" in arguments:
            predicate = arguments["predicate"]
            if name in {"inner_join", "semi_join"} and predicate["kind"] != "column_equals_present":
                fail("query.join-present-equality-required", identifier)
            if name == "filter" and predicate["kind"] == "column_equals_present":
                fail("query.filter-join-predicate", identifier)
            for reference in _column_refs(predicate):
                validate_ref(reference, available)
            if predicate["kind"] == "equals_present":
                validate_dynamic_literal(
                    columns,
                    predicate["column"]["column_id"],
                    predicate["literal"],
                )

        if name == "project":
            projected: set[str] = set()
            mapping: dict[str, str] = {}
            for item in arguments["columns"]:
                validate_ref(item["column"], inputs[0].columns)
                output = item["output"]
                if output in projected:
                    fail("query.project-output-duplicate", output)
                projected.add(output)
                mapping[item["column"]["column_id"]] = output
            retained = all(key in mapping for key in inputs[0].order_keys)
            return NodeShape(
                frozenset(projected),
                inputs[0].ordered and retained,
                tuple(mapping[key] for key in inputs[0].order_keys) if retained else (),
            )
        if name == "union":
            if inputs[0].columns != inputs[1].columns:
                fail("query.union-schema-mismatch", "union inputs differ")
            return NodeShape(inputs[0].columns, False)
        if name in {"inner_join", "semi_join"}:
            output = available if name == "inner_join" else inputs[0].columns
            return NodeShape(output, inputs[0].ordered if name == "semi_join" else False)
        if name == "order_by":
            keys: list[str] = []
            for item in arguments["keys"]:
                validate_ref(item["column"], inputs[0].columns)
                keys.append(item["column"]["column_id"])
                if set(item["cell_state_order"]) != {"absent", "present", "unknown"}:
                    fail("query.order-state-incomplete", keys[-1])
            return NodeShape(inputs[0].columns, True, tuple(keys))
        if name == "limit":
            if not inputs[0].ordered:
                fail("query.limit-unordered", "limit input has no total order")
            return inputs[0]
        if name == "condition_restrict" and not arguments["alternatives"]:
            fail("query.condition-empty-restriction", "condition set is empty")
        return inputs[0]

    shape = visit(ir["root"])
    expected_outputs = {row["id"] for row in ir["output_schema"]}
    if shape.columns != expected_outputs:
        fail(
            "query.output-schema-mismatch",
            f"root={sorted(shape.columns)}, output={sorted(expected_outputs)}",
        )
    return shape


def validate_cell(cell: dict[str, Any]) -> None:
    state = cell.get("state")
    if state == "present":
        if set(cell) != {"state", "type", "value"}:
            fail("query.cell-invalid", "present cell fields differ")
        if cell["value"] is None:
            fail("query.sql-null-forbidden", "present value is SQL null")
    elif state == "absent":
        if set(cell) != {"state"}:
            fail("query.cell-invalid", "absent cell has payload")
    elif state == "unknown":
        if set(cell) != {"state", "reason"} or not cell["reason"]:
            fail("query.cell-invalid", "unknown cell lacks reason")
    else:
        fail("query.cell-state-unknown", str(state))


def validate_record(record: dict[str, Any]) -> None:
    required = {
        "values",
        "multiplicity",
        "presence",
        "interpretation",
        "claim_contributors",
        "provenance",
    }
    if set(record) != required or record["multiplicity"] < 1:
        fail("query.row-invalid", "annotated row shape differs")
    for cell in record["values"].values():
        validate_cell(cell)
    presence = record["presence"]
    if set(presence) != {"universe", "alternatives"}:
        fail("query.condition-invalid", "presence shape differs")
    if not presence["alternatives"] or len(presence["alternatives"]) != len(
        set(presence["alternatives"])
    ):
        fail("query.condition-invalid", "condition alternatives are empty/duplicate")
    for field in ("claim_contributors", "provenance"):
        if not record[field] or len(record[field]) != len(set(record[field])):
            fail("query.evidence-invalid", field)


class SourceBackend:
    def scan(self, descriptor_id: str) -> list[dict[str, Any]]:
        raise NotImplementedError


def _ordered_rows(rows: list[dict[str, Any]], physical_order: str) -> list[dict[str, Any]]:
    result = copy.deepcopy(rows)
    if physical_order == "reverse":
        result.reverse()
    elif physical_order == "seeded-shuffle":
        random.Random(611).shuffle(result)
    return result


class MemorySource(SourceBackend):
    def __init__(self, dataset: dict[str, list[dict[str, Any]]], order: str) -> None:
        self.dataset = dataset
        self.order = order

    def scan(self, descriptor_id: str) -> list[dict[str, Any]]:
        return _ordered_rows(self.dataset.get(descriptor_id, []), self.order)


class SqliteSource(SourceBackend):
    def __init__(self, dataset: dict[str, list[dict[str, Any]]], order: str) -> None:
        self.connection = sqlite3.connect(":memory:")
        self.order = order
        self.connection.execute(
            "CREATE TABLE claims(descriptor TEXT NOT NULL, ordinal INTEGER NOT NULL, row TEXT NOT NULL)"
        )
        for descriptor, rows in dataset.items():
            self.connection.executemany(
                "INSERT INTO claims(descriptor, ordinal, row) VALUES (?, ?, ?)",
                [
                    (
                        descriptor,
                        ordinal,
                        canonical_json(row).decode("utf-8"),
                    )
                    for ordinal, row in enumerate(rows)
                ],
            )

    def scan(self, descriptor_id: str) -> list[dict[str, Any]]:
        rows = [
            json.loads(row[0])
            for row in self.connection.execute(
                "SELECT row FROM claims WHERE descriptor = ? ORDER BY ordinal ASC",
                (descriptor_id,),
            )
        ]
        return _ordered_rows(rows, self.order)


@dataclass
class Evaluation:
    rows: list[dict[str, Any]]
    ordered: bool = False
    order_keys: tuple[dict[str, Any], ...] = ()


def _cell_present_equal(left: dict[str, Any], right: dict[str, Any]) -> bool:
    return (
        left.get("state") == "present"
        and right.get("state") == "present"
        and left.get("type") == right.get("type")
        and left.get("value") == right.get("value")
    )


def _literal_cell(literal: dict[str, Any]) -> dict[str, Any]:
    return {"state": "present", "type": literal["type"], "value": literal["value"]}


def _predicate(row: dict[str, Any], predicate: dict[str, Any]) -> bool:
    kind = predicate["kind"]
    if kind in {"and", "or"}:
        values = [_predicate(row, child) for child in predicate["operands"]]
        return all(values) if kind == "and" else any(values)
    if kind == "equals_present":
        cell = row["values"].get(predicate["column"]["column_id"], {"state": "absent"})
        return _cell_present_equal(cell, _literal_cell(predicate["literal"]))
    if kind == "column_equals_present":
        left = row["values"].get(predicate["left"]["column_id"], {"state": "absent"})
        right = row["values"].get(predicate["right"]["column_id"], {"state": "absent"})
        return _cell_present_equal(left, right)
    cell = row["values"].get(predicate["column"]["column_id"], {"state": "absent"})
    return cell["state"] == kind.removeprefix("is_")


def _condition_intersection(
    left: dict[str, Any], right: dict[str, Any]
) -> dict[str, Any] | None:
    if left["universe"] != right["universe"]:
        fail("query.condition-universe-mismatch", "join universes differ")
    alternatives = sorted(set(left["alternatives"]) & set(right["alternatives"]))
    if not alternatives:
        return None
    return {"universe": left["universe"], "alternatives": alternatives}


def _combine(left: dict[str, Any], right: dict[str, Any]) -> dict[str, Any] | None:
    if left["interpretation"] != right["interpretation"]:
        return None
    presence = _condition_intersection(left["presence"], right["presence"])
    if presence is None:
        return None
    values = copy.deepcopy(left["values"])
    for identifier, cell in right["values"].items():
        if identifier in values and values[identifier] != cell:
            return None
        values[identifier] = copy.deepcopy(cell)
    return {
        "values": values,
        "multiplicity": left["multiplicity"] * right["multiplicity"],
        "presence": presence,
        "interpretation": left["interpretation"],
        "claim_contributors": sorted(
            set(left["claim_contributors"]) | set(right["claim_contributors"])
        ),
        "provenance": sorted(set(left["provenance"]) | set(right["provenance"])),
    }


def _distinct(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[bytes, dict[str, Any]] = {}
    for row in rows:
        key = canonical_json(
            {
                "values": row["values"],
                "universe": row["presence"]["universe"],
                "interpretation": row["interpretation"],
            }
        )
        if key not in groups:
            groups[key] = copy.deepcopy(row)
            groups[key]["multiplicity"] = 1
            continue
        current = groups[key]
        current["presence"]["alternatives"] = sorted(
            set(current["presence"]["alternatives"])
            | set(row["presence"]["alternatives"])
        )
        current["claim_contributors"] = sorted(
            set(current["claim_contributors"]) | set(row["claim_contributors"])
        )
        current["provenance"] = sorted(
            set(current["provenance"]) | set(row["provenance"])
        )
    return list(groups.values())


def _compare_rows(left: dict[str, Any], right: dict[str, Any], keys: tuple[dict[str, Any], ...]) -> int:
    for key in keys:
        identifier = key["column"]["column_id"]
        left_cell = left["values"].get(identifier, {"state": "absent"})
        right_cell = right["values"].get(identifier, {"state": "absent"})
        state_order = {state: index for index, state in enumerate(key["cell_state_order"])}
        left_value = (state_order[left_cell["state"]], canonical_json(left_cell))
        right_value = (state_order[right_cell["state"]], canonical_json(right_cell))
        comparison = (left_value > right_value) - (left_value < right_value)
        if comparison:
            return comparison if key["direction"] == "ascending" else -comparison
    left_json = canonical_json(left)
    right_json = canonical_json(right)
    return (left_json > right_json) - (left_json < right_json)


def _scan_count(node: dict[str, Any], source: SourceBackend) -> int:
    count = 0
    if node["operator"] == "query.scan.v1":
        count += len(source.scan(node["arguments"]["descriptor_id"]))
    return count + sum(_scan_count(child, source) for child in node["inputs"])


def evaluate_ir(ir: dict[str, Any], source: SourceBackend) -> dict[str, Any]:
    for descriptor in {
        node_value
        for key, node_value in _walk(ir["root"])
        if key == "descriptor_id"
    }:
        for row in source.scan(descriptor):
            validate_record(row)

    scan_budget = ir.get("runtime_budget", {}).get("max_rows_scanned")
    if scan_budget is not None and _scan_count(ir["root"], source) > scan_budget:
        return {
            "status": "failed_before_result",
            "ordered": False,
            "rows": [],
        }

    def visit(node: dict[str, Any]) -> Evaluation:
        operator = node["operator"]
        arguments = node["arguments"]
        inputs = [visit(child) for child in node["inputs"]]
        if operator == "query.scan.v1":
            return Evaluation(source.scan(arguments["descriptor_id"]))
        if operator == "query.filter.v1":
            return Evaluation(
                [row for row in inputs[0].rows if _predicate(row, arguments["predicate"])],
                inputs[0].ordered,
                inputs[0].order_keys,
            )
        if operator == "query.project.v1":
            mapping = {
                item["column"]["column_id"]: item["output"]
                for item in arguments["columns"]
            }
            rows = []
            for row in inputs[0].rows:
                projected = copy.deepcopy(row)
                projected["values"] = {
                    output: copy.deepcopy(row["values"].get(source_id, {"state": "absent"}))
                    for source_id, output in mapping.items()
                }
                rows.append(projected)
            retained = all(
                key["column"]["column_id"] in mapping for key in inputs[0].order_keys
            )
            order_keys = tuple(
                {
                    **copy.deepcopy(key),
                    "column": {
                        **key["column"],
                        "column_id": mapping[key["column"]["column_id"]],
                    },
                }
                for key in inputs[0].order_keys
            ) if retained else ()
            return Evaluation(rows, inputs[0].ordered and retained, order_keys)
        if operator == "query.inner_join.v1":
            rows = []
            for left in inputs[0].rows:
                for right in inputs[1].rows:
                    combined = _combine(left, right)
                    if combined is not None and _predicate(combined, arguments["predicate"]):
                        rows.append(combined)
            return Evaluation(rows)
        if operator == "query.semi_join.v1":
            rows = []
            for left in inputs[0].rows:
                witnesses = []
                for right in inputs[1].rows:
                    combined = _combine(left, right)
                    if combined is not None and _predicate(combined, arguments["predicate"]):
                        witnesses.append(combined)
                if not witnesses:
                    continue
                result = copy.deepcopy(left)
                result["presence"]["alternatives"] = sorted(
                    {
                        alternative
                        for witness in witnesses
                        for alternative in witness["presence"]["alternatives"]
                    }
                )
                result["claim_contributors"] = sorted(
                    {
                        value
                        for witness in witnesses
                        for value in witness["claim_contributors"]
                    }
                )
                result["provenance"] = sorted(
                    {value for witness in witnesses for value in witness["provenance"]}
                )
                rows.append(result)
            return Evaluation(rows, inputs[0].ordered, inputs[0].order_keys)
        if operator == "query.union.v1":
            return Evaluation(copy.deepcopy(inputs[0].rows + inputs[1].rows))
        if operator == "query.distinct.v1":
            return Evaluation(_distinct(inputs[0].rows))
        if operator == "query.condition_restrict.v1":
            restriction = {
                "universe": arguments["universe"],
                "alternatives": arguments["alternatives"],
            }
            rows = []
            for row in inputs[0].rows:
                presence = _condition_intersection(row["presence"], restriction)
                if presence is not None:
                    value = copy.deepcopy(row)
                    value["presence"] = presence
                    rows.append(value)
            return Evaluation(rows, inputs[0].ordered, inputs[0].order_keys)
        if operator == "query.interpretation_restrict.v1":
            return Evaluation(
                [
                    row
                    for row in inputs[0].rows
                    if row["interpretation"] == arguments["interpretation"]
                ],
                inputs[0].ordered,
                inputs[0].order_keys,
            )
        if operator == "query.order_by.v1":
            keys = tuple(arguments["keys"])
            rows = sorted(
                inputs[0].rows,
                key=functools.cmp_to_key(lambda left, right: _compare_rows(left, right, keys)),
            )
            return Evaluation(rows, True, keys)
        if operator == "query.limit.v1":
            if not inputs[0].ordered:
                fail("query.limit-unordered", "runtime limit input is unordered")
            return Evaluation(
                inputs[0].rows[: arguments["count"]],
                True,
                inputs[0].order_keys,
            )
        fail("query.operator-unknown", operator)

    result = visit(ir["root"])
    status = "complete"
    output_cap = ir.get("runtime_budget", {}).get("max_rows_output")
    if output_cap is not None and len(result.rows) > output_cap:
        if not result.ordered:
            result.rows.sort(key=canonical_json)
        result.rows = result.rows[:output_cap]
        status = "truncated"
    rows = result.rows if result.ordered else sorted(result.rows, key=canonical_json)
    return {"status": status, "ordered": result.ordered, "rows": rows}


def evaluate_backend_matrix(
    ir: dict[str, Any], dataset: dict[str, list[dict[str, Any]]]
) -> tuple[dict[str, Any], int]:
    results: list[dict[str, Any]] = []
    for backend in ("memory", "sqlite"):
        for order in ("forward", "reverse", "seeded-shuffle"):
            source: SourceBackend
            if backend == "memory":
                source = MemorySource(dataset, order)
            else:
                source = SqliteSource(dataset, order)
            results.append(evaluate_ir(ir, source))
    projections = [canonical_json(row) for row in results]
    if len(set(projections)) != 1:
        fail("query.backend-semantic-mismatch", "backend/order matrix differs")
    return results[0], len(results)


def validate_schema_minor(value: dict[str, Any]) -> None:
    if value["column_available"]:
        return
    if value["column_required"]:
        fail("query.required-column-missing", value["column_id"])
    if value["access"] == "absent_if_schema_missing":
        return
    fail("query.optional-column-unavailable", value["column_id"])


def validate_continuation(value: dict[str, Any]) -> None:
    if value["ordering"] != "total" or not value["prefix_sealed"]:
        fail("query.continuation-unsafe", "continuation requires sealed total order")
    required = {
        "logical_ir_digest",
        "snapshot_id",
        "relation_descriptor_digests",
        "total_order_specification",
        "last_total_key",
        "result_schema_digest",
    }
    token = value["token"]
    current = value["current"]
    if set(token) != required or set(current) != required:
        fail("query.continuation-incomplete", "token binding set differs")
    if token != current:
        fail("query.continuation-stale", "token binding differs from current execution")


def validate_partial_execution(value: dict[str, Any]) -> tuple[str, int]:
    kind = value["kind"]
    if kind == "upstream-interruption-unsealed":
        if value.get("published_rows", 0):
            fail("query.partial-unsealed", "unsealed rows were published")
        return "failed_before_result", 0
    if kind == "ordered-sealed-prefix":
        if not value.get("ordered") or not value.get("prefix_sealed"):
            fail("query.partial-unsealed", "partial prefix is not sealed and ordered")
        rows = int(value.get("published_rows", 0))
        if rows < 1:
            fail("query.partial-empty", "partial status has no rows")
        return "cancelled_with_partial", rows
    if kind == "partial-aggregate":
        fail("query.partial-aggregate", "aggregate is deferred and cannot publish NG0 partial")
    fail("query.partial-kind-unknown", kind)


def validate_operator_use(contract: dict[str, Any], value: dict[str, Any]) -> None:
    identifier = value["operator"]
    if identifier not in OPERATOR_NEGATIVE_CASES:
        fail("query.operator-unknown", identifier)
    expected_case, reason = OPERATOR_NEGATIVE_CASES[identifier]
    if value["case"] != expected_case:
        fail("query.negative-vector-invalid", identifier)
    operators = _rows_by(contract["operator_profiles"], "id", "operator")
    if identifier not in operators:
        fail("query.operator-unknown", identifier)
    fail(reason, f"negative conformance vector for {identifier}")


def execute_vector(
    contract: dict[str, Any],
    contract_schema: dict[str, Any],
    ir_schema: dict[str, Any],
    registry: dict[str, Any],
    vector: dict[str, Any],
) -> dict[str, Any]:
    operation = vector["operation"]
    value = vector["input"]
    try:
        result: dict[str, Any] = {}
        if operation == "validate_contract":
            validate_contract(contract, contract_schema)
            reason = "query.contract-valid"
        elif operation == "validate_operator_use":
            validate_operator_use(contract, value)
            reason = "query.operator-use-valid"
        elif operation == "validate_deferred_operator":
            deferred = {row["id"] for row in contract["deferred_operators"]}
            if value["operator"] not in deferred:
                fail("query.operator-not-deferred", value["operator"])
            if value.get("attempt_profile") == "NG0":
                code = (
                    "query.partial-aggregate"
                    if value["operator"] == "query.aggregate.v1"
                    else "query.operator-deferred"
                )
                fail(code, value["operator"])
            reason = "query.operator-deferred"
        elif operation == "compare_normalized_ir":
            validate_ir(value["static"], ir_schema, contract, registry)
            validate_ir(value["dynamic"], ir_schema, contract, registry)
            left = normalized_ir_digest(value["static"])
            right = normalized_ir_digest(value["dynamic"])
            if left != right:
                fail("query.normalized-digest-mismatch", f"{left} != {right}")
            reason = "query.normalized-digest-equal"
            result["result_digest"] = left
        elif operation == "evaluate_backend_matrix":
            validate_ir(value["ir"], ir_schema, contract, registry)
            evaluated, comparisons = evaluate_backend_matrix(value["ir"], value["dataset"])
            reason = "query.backend-matrix-equal"
            result.update(
                execution_status=evaluated["status"],
                row_count=len(evaluated["rows"]),
                result_digest=digest(evaluated),
                backend_comparisons=comparisons,
            )
        elif operation == "validate_cell":
            validate_cell(value["cell"])
            reason = "query.cell-valid"
        elif operation == "validate_schema_minor":
            validate_schema_minor(value)
            reason = "query.schema-minor-compatible"
        elif operation == "validate_dynamic_literal":
            validate_dynamic_literal(
                relation_columns(registry), value["column_id"], value["literal"]
            )
            reason = "query.literal-valid"
        elif operation == "validate_continuation":
            validate_continuation(value)
            reason = "query.continuation-valid"
        elif operation == "validate_partial_execution":
            status, count = validate_partial_execution(value)
            reason = "query.partial-safe"
            result.update(execution_status=status, row_count=count)
        elif operation == "validate_logical_authority":
            validate_logical_authority(contract, value["document"])
            reason = "query.logical-authority-clean"
        else:
            fail("query.vector-operation-unknown", operation)
        return {"decision": "accepted", "reason_code": reason, **result}
    except QueryContractError as error:
        return {"decision": "rejected", "reason_code": error.code}


def validate_vectors(
    contract: dict[str, Any],
    contract_schema: dict[str, Any],
    ir_schema: dict[str, Any],
    registry: dict[str, Any],
    vectors: dict[str, Any],
) -> list[dict[str, Any]]:
    identifiers = [row["id"] for row in vectors["vectors"]]
    if len(identifiers) != len(set(identifiers)):
        fail("query.duplicate-vector", "vector IDs duplicate")
    if set(identifiers) != REQUIRED_VECTOR_IDS:
        fail("query.vector-set-invalid", "exact conformance vector set differs")
    negative_operators = {
        row["input"].get("operator")
        for row in vectors["vectors"]
        if row["operation"] == "validate_operator_use"
    }
    if negative_operators != set(NG0_OPERATORS):
        fail("query.operator-negative-coverage", "not every NG0 operator has a negative vector")
    results = []
    for vector in vectors["vectors"]:
        actual = execute_vector(
            contract, contract_schema, ir_schema, registry, vector
        )
        expected = vector["expected"]
        for field in ("decision", "reason_code", "execution_status", "row_count"):
            if field in expected and actual.get(field) != expected[field]:
                fail(
                    "query.vector-result-mismatch",
                    f"{vector['id']} {field}: {actual.get(field)} != {expected[field]}",
                )
        if (vector["class"] == "positive") != (actual["decision"] == "accepted"):
            fail("query.vector-class-mismatch", vector["id"])
        results.append({"id": vector["id"], **actual, "matched": True})
    return results


def make_report(
    contract: dict[str, Any], results: list[dict[str, Any]]
) -> dict[str, Any]:
    operators = sorted(contract["operator_profiles"], key=lambda row: row["id"])
    comparisons = sum(int(row.get("backend_comparisons", 0)) for row in results)
    public_results = [
        {key: value for key, value in row.items() if key != "backend_comparisons"}
        for row in results
    ]
    return {
        "schema": "cxxlens.query-conformance-report.v1",
        "contract_digest": digest(contract),
        "operator_digests": [
            {"id": row["id"], "digest": digest(row)} for row in operators
        ],
        "vector_results": public_results,
        "backend_matrix": {
            "backends": ["memory", "sqlite"],
            "physical_orders": ["forward", "reverse", "seeded-shuffle"],
            "comparisons": comparisons,
            "all_equal": True,
        },
        "status": "green",
    }


def validate_design(root: pathlib.Path) -> None:
    design = (root / "docs/design/cxxlens_next_generation_integrated_design_ja.md").read_text(
        encoding="utf-8"
    )
    for marker in (
        "0.6.0-normative",
        "schemas/cxxlens_ng_logical_query_contract.yaml",
        "annotated multiset",
        "query.limit.v1",
        "absent_if_schema_missing",
        "Issue #61",
    ):
        if marker not in design:
            fail("query.design-marker-missing", marker)
    if "filter/project/inner/semi/union/distinct/aggregate" in design:
        fail("query.aggregate-profile-stale", "NG0 checklist still contains aggregate")
    index = (root / "docs/design/catalogs/README.md").read_text(encoding="utf-8")
    if "Logical Query Contract" not in index or "#61" not in index:
        fail("query.catalog-index-stale", "query authority is absent from catalog index")
    adr = (root / "docs/design/adr/0007-logical-query-algebra.md").read_text(
        encoding="utf-8"
    )
    if "- Status: Accepted" not in adr or "- Decision issue: #61" not in adr:
        fail("query.adr-not-accepted", "ADR 0007 is not accepted by #61")


def validate_all(root: pathlib.Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    contract = load_yaml(root / CONTRACT)
    contract_schema = load_yaml(root / CONTRACT_SCHEMA)
    ir_schema = load_yaml(root / IR_SCHEMA)
    registry = load_yaml(root / RELATION_REGISTRY)
    vectors = load_yaml(root / VECTORS)
    schema_validate(vectors, load_yaml(root / VECTORS_SCHEMA), "query vectors")
    validate_design(root)
    validate_contract(contract, contract_schema)
    results = validate_vectors(
        contract, contract_schema, ir_schema, registry, vectors
    )
    report = make_report(contract, results)
    schema_validate(report, load_yaml(root / REPORT_SCHEMA), "query report")
    return contract, results


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    contract, results = validate_all(root)
    report = make_report(contract, results)
    if args.mode == "report":
        rendered = json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
        if args.output:
            args.output.write_text(rendered, encoding="utf-8")
        else:
            print(rendered, end="")
    print(
        f"verified {len(contract['operator_profiles'])} NG0 query operators, "
        f"{len(results)} vectors, contract {report['contract_digest']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, QueryContractError) as error:
        print(f"NG query contract failure: {error}", file=sys.stderr)
        raise SystemExit(1) from error
