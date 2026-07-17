#!/usr/bin/env python3
"""Positive, negative, parity, and partiality tests for Logical Query IR v1."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_query_contract import (  # noqa: E402
    CONTRACT,
    CONTRACT_SCHEMA,
    IR_SCHEMA,
    NG0_OPERATORS,
    RELATION_REGISTRY,
    VECTORS,
    MemorySource,
    QueryContractError,
    evaluate_backend_matrix,
    evaluate_ir,
    execute_vector,
    load_yaml,
    make_report,
    normalized_ir_digest,
    relation_columns,
    schema_validate,
    validate_all,
    validate_cell,
    validate_continuation,
    validate_contract,
    validate_dynamic_literal,
    validate_ir,
    validate_logical_authority,
    validate_partial_execution,
    validate_schema_minor,
)


class NgQueryContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = load_yaml(ROOT / CONTRACT)
        cls.contract_schema = load_yaml(ROOT / CONTRACT_SCHEMA)
        cls.ir_schema = load_yaml(ROOT / IR_SCHEMA)
        cls.registry = load_yaml(ROOT / RELATION_REGISTRY)
        cls.vectors = load_yaml(ROOT / VECTORS)

    def vector(self, identifier: str) -> dict:
        return next(row for row in self.vectors["vectors"] if row["id"] == identifier)

    def test_contract_has_exact_eleven_ng0_operators_and_thirty_five_vectors(self) -> None:
        contract, results = validate_all(ROOT)
        self.assertEqual(len(contract["operator_profiles"]), 11)
        self.assertEqual(len(results), 35)
        self.assertEqual(
            {row["id"] for row in contract["operator_profiles"]},
            set(NG0_OPERATORS),
        )

    def test_every_ng0_operator_has_a_negative_vector(self) -> None:
        covered = {
            row["input"]["operator"]
            for row in self.vectors["vectors"]
            if row["operation"] == "validate_operator_use"
        }
        self.assertEqual(covered, set(NG0_OPERATORS))
        for row in self.vectors["vectors"]:
            if row["operation"] == "validate_operator_use":
                actual = execute_vector(
                    self.contract,
                    self.contract_schema,
                    self.ir_schema,
                    self.registry,
                    row,
                )
                self.assertEqual(actual["decision"], "rejected")
                self.assertEqual(actual["reason_code"], row["expected"]["reason_code"])

    def test_group_and_aggregate_are_deferred_from_ng0(self) -> None:
        deferred = {row["id"] for row in self.contract["deferred_operators"]}
        self.assertIn("query.group.v1", deferred)
        self.assertIn("query.aggregate.v1", deferred)
        self.assertNotIn("query.group.v1", NG0_OPERATORS)
        self.assertNotIn("query.aggregate.v1", NG0_OPERATORS)

    def test_static_and_dynamic_surfaces_have_one_normalized_digest(self) -> None:
        value = self.vector("static-dynamic-normalized-digest")["input"]
        self.assertEqual(
            normalized_ir_digest(value["static"]),
            normalized_ir_digest(value["dynamic"]),
        )

    def test_memory_sqlite_and_physical_orders_have_same_semantic_result(self) -> None:
        value = self.vector("backend-and-order-parity")["input"]
        result, executions = evaluate_backend_matrix(value["ir"], value["dataset"])
        self.assertEqual(executions, 6)
        self.assertEqual(result["status"], "complete")
        self.assertEqual(
            [row["values"]["output.call"]["value"] for row in result["rows"]],
            ["call-1", "call-4"],
        )

    def test_distinct_unions_conditions_without_losing_evidence(self) -> None:
        value = self.vector("backend-and-order-parity")["input"]
        result = evaluate_ir(value["ir"], MemorySource(value["dataset"], "reverse"))
        first = result["rows"][0]
        self.assertEqual(first["multiplicity"], 1)
        self.assertEqual(first["presence"]["alternatives"], ["debug", "release"])
        self.assertEqual(
            first["claim_contributors"],
            ["claim-call-1", "claim-entity-danger", "claim-target-1"],
        )

    def test_output_cap_is_deterministic_truncation_after_complete_upstream(self) -> None:
        value = self.vector("deterministic-output-truncation")["input"]
        result, executions = evaluate_backend_matrix(value["ir"], value["dataset"])
        self.assertEqual(executions, 6)
        self.assertEqual(result["status"], "truncated")
        self.assertEqual(len(result["rows"]), 1)
        self.assertEqual(result["rows"][0]["values"]["output.call"]["value"], "call-1")

    def test_unordered_limit_fails_during_real_ir_validation(self) -> None:
        base = copy.deepcopy(
            self.vector("static-dynamic-normalized-digest")["input"]["static"]
        )
        scan = {
            "operator": "query.scan.v1",
            "inputs": [],
            "arguments": {"descriptor_id": "cc.call_site.v1", "alias": "calls"},
        }
        base["root"] = {
            "operator": "query.project.v1",
            "inputs": [
                {
                    "operator": "query.limit.v1",
                    "inputs": [scan],
                    "arguments": {"count": 1},
                }
            ],
            "arguments": {
                "columns": [
                    {
                        "column": {
                            "column_id": "cc.call_site.v1.call",
                            "availability": "require",
                            "source_alias": "calls",
                        },
                        "output": "output.call",
                    }
                ]
            },
        }
        base["output_schema"] = [
            {
                "id": "output.call",
                "type": "typed_id<cc_call_id>",
                "required": True,
            }
        ]
        with self.assertRaisesRegex(QueryContractError, "query.limit-unordered"):
            validate_ir(
                base,
                self.ir_schema,
                self.contract,
                self.registry,
            )

    def test_absent_unknown_and_null_are_three_distinct_states(self) -> None:
        validate_cell({"state": "absent"})
        validate_cell({"state": "unknown", "reason": "unresolved-1"})
        with self.assertRaisesRegex(QueryContractError, "query.sql-null-forbidden"):
            validate_cell({"state": "present", "type": "utf8_string", "value": None})

    def test_optional_minor_fallback_requires_explicit_absent_policy(self) -> None:
        accepted = self.vector("missing-optional-as-explicit-absent")["input"]
        validate_schema_minor(accepted)
        with self.assertRaisesRegex(
            QueryContractError, "query.optional-column-unavailable"
        ):
            validate_schema_minor(self.vector("missing-optional-direct-reference")["input"])
        with self.assertRaisesRegex(QueryContractError, "query.required-column-missing"):
            validate_schema_minor(self.vector("missing-required-column")["input"])

    def test_dynamic_literal_requires_exact_explicit_column_type(self) -> None:
        columns = relation_columns(self.registry)
        validate_dynamic_literal(
            columns,
            "cc.entity.v1.qualified_name",
            {"type": "utf8_string", "value": "app::dangerous"},
        )
        with self.assertRaisesRegex(QueryContractError, "query.literal-type-mismatch"):
            validate_dynamic_literal(
                columns,
                "cc.entity.v1.qualified_name",
                {"type": "bytes", "value": "app::dangerous"},
            )

    def test_continuation_binds_query_snapshot_schema_order_and_last_key(self) -> None:
        validate_continuation(self.vector("ordered-continuation-bound")["input"])
        with self.assertRaisesRegex(QueryContractError, "query.continuation-stale"):
            validate_continuation(self.vector("stale-continuation-snapshot")["input"])

    def test_unsealed_partial_is_empty_and_ordered_sealed_prefix_is_explicit(self) -> None:
        self.assertEqual(
            validate_partial_execution(
                self.vector("unsealed-upstream-interruption")["input"]
            ),
            ("failed_before_result", 0),
        )
        self.assertEqual(
            validate_partial_execution(
                self.vector("sealed-ordered-cancel-prefix")["input"]
            ),
            ("cancelled_with_partial", 2),
        )
        with self.assertRaisesRegex(QueryContractError, "query.partial-aggregate"):
            validate_partial_execution(
                self.vector("partial-aggregate-publication")["input"]
            )

    def test_physical_plan_fields_cannot_enter_versioned_logical_authority(self) -> None:
        validate_logical_authority(
            self.contract,
            self.vector("logical-ir-without-physical-fields")["input"]["document"],
        )
        with self.assertRaisesRegex(QueryContractError, "query.physical-authority-leak"):
            validate_logical_authority(
                self.contract,
                self.vector("physical-index-is-not-logical-authority")["input"]["document"],
            )

    def test_contract_schema_rejects_bootstrap_maturity(self) -> None:
        candidate = copy.deepcopy(self.contract)
        candidate["maturity"] = "bootstrap"
        with self.assertRaisesRegex(QueryContractError, "query.schema-invalid"):
            schema_validate(candidate, self.contract_schema, "query contract")

    def test_report_has_exact_operator_digests_and_backend_comparisons(self) -> None:
        contract, results = validate_all(ROOT)
        report = make_report(contract, results)
        self.assertEqual(report["status"], "green")
        self.assertEqual(len(report["operator_digests"]), 11)
        self.assertEqual(len({row["digest"] for row in report["operator_digests"]}), 11)
        self.assertEqual(report["backend_matrix"]["comparisons"], 12)

    def test_contract_rejects_enabling_implicit_sql_null(self) -> None:
        candidate = copy.deepcopy(self.contract)
        candidate["collection_model"]["null_policy"][
            "implicit_three_valued_logic"
        ] = "enabled"
        with self.assertRaisesRegex(QueryContractError, "query.sql-null-forbidden"):
            validate_contract(candidate, self.contract_schema)


if __name__ == "__main__":
    unittest.main()
