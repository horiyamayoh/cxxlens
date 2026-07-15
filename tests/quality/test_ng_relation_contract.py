#!/usr/bin/env python3
"""Positive and fail-closed tests for the NG0 exact relation contract."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_relation_contract import (  # noqa: E402
    REGISTRY,
    REGISTRY_SCHEMA,
    VECTORS,
    RelationContractError,
    apply_patches,
    compare_api_ids,
    execute_vector,
    load_yaml,
    make_report,
    register_extension,
    registry_semantic_projection,
    resolve_reference,
    schema_validate,
    validate_contract,
    validate_evolution,
    validate_query_columns,
    validate_registry,
    validate_symbol,
    validate_vectors,
)


class NgRelationContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.registry = load_yaml(ROOT / REGISTRY)
        cls.registry_schema = load_yaml(ROOT / REGISTRY_SCHEMA)
        cls.vectors = load_yaml(ROOT / VECTORS)

    def vector(self, identifier: str) -> dict:
        return next(row for row in self.vectors["vectors"] if row["id"] == identifier)

    def test_exact_registry_has_all_ng0_relations_and_valid_vectors(self) -> None:
        registry, results = validate_contract(ROOT)
        self.assertEqual(len(registry["relations"]), 18)
        self.assertEqual(len(results), 20)
        self.assertEqual({row["decision"] for row in results}, {"accepted", "rejected"})

    def test_claim_condition_exists_only_in_system_envelope(self) -> None:
        envelope = self.registry["system_claim_envelope"]
        self.assertEqual(envelope["condition_authority"], "envelope-presence-only")
        for relation in self.registry["relations"]:
            self.assertNotIn("presence", {row["name"] for row in relation["columns"]})
            self.assertNotIn("condition_ref", {row["type"] for row in relation["columns"]})

        vector = self.vector("envelope-only-condition")
        self.assertEqual(
            execute_vector(self.registry, self.registry_schema, vector),
            ("rejected", "relation.condition-duplicated"),
        )

    def test_claim_envelope_instance_forbids_containing_snapshot_identity(self) -> None:
        accepted = self.vector("envelope-instance-valid")
        rejected = self.vector("envelope-containing-snapshot-forbidden")
        self.assertEqual(
            execute_vector(self.registry, self.registry_schema, accepted),
            ("accepted", "relation.envelope-instance-valid"),
        )
        self.assertEqual(
            execute_vector(self.registry, self.registry_schema, rejected),
            ("rejected", "relation.envelope-instance-invalid"),
        )

    def test_authoritative_id_projection_cannot_contain_result_id(self) -> None:
        candidate = apply_patches(
            self.registry,
            self.vector("noncircular-domain-identity")["input"]["patches"],
        )
        with self.assertRaisesRegex(RelationContractError, "relation.identity-cycle"):
            validate_registry(candidate, schema=self.registry_schema)

    def test_call_model_uses_separate_direct_target_relation(self) -> None:
        calls = next(row for row in self.registry["relations"] if row["name"] == "cc.call_site")
        targets = next(
            row
            for row in self.registry["relations"]
            if row["name"] == "cc.call_direct_target"
        )
        self.assertNotIn("direct_target", {row["name"] for row in calls["columns"]})
        self.assertEqual(
            {row["name"] for row in targets["columns"]},
            {"call", "target", "resolution"},
        )
        self.assertEqual(
            validate_query_columns(self.registry, self.vector("exact-call-model")["input"]),
            ("accepted", "relation.query-columns-valid"),
        )

    def test_call_site_partition_uses_declared_compile_unit_column(self) -> None:
        vector = self.vector("call-site-partition-column")
        self.assertEqual(
            execute_vector(self.registry, self.registry_schema, vector),
            ("rejected", "relation.partition-column-unknown"),
        )

    def test_hard_reference_missing_rejects_even_if_unresolved_is_present(self) -> None:
        value = self.vector("hard-reference-missing")["input"]
        self.assertEqual(
            resolve_reference(self.registry, value),
            ("rejected", "relation.hard-reference-missing"),
        )

    def test_soft_reference_requires_explicit_unresolved_accounting(self) -> None:
        accepted = self.vector("soft-reference-missing-accounted")["input"]
        rejected = self.vector("soft-reference-missing-unaccounted")["input"]
        self.assertEqual(
            resolve_reference(self.registry, accepted),
            ("accepted", "relation.soft-reference-unresolved"),
        )
        self.assertEqual(
            resolve_reference(self.registry, rejected),
            ("rejected", "relation.soft-reference-unaccounted"),
        )

    def test_unknown_open_symbol_is_preserved_and_closed_symbol_is_rejected(self) -> None:
        open_value = self.vector("open-symbol-unknown-preserved")["input"]
        closed_value = self.vector("closed-symbol-unknown-rejected")["input"]
        self.assertEqual(
            validate_symbol(self.registry, open_value),
            ("accepted", "relation.open-symbol-preserved"),
        )
        self.assertEqual(
            validate_symbol(self.registry, closed_value),
            ("rejected", "relation.closed-symbol-unknown"),
        )

    def test_minor_evolution_is_additive_and_unknown_preserving_only(self) -> None:
        optional = self.vector("minor-optional-column-preserved")["input"]
        required = self.vector("minor-required-column-rejected")["input"]
        self.assertEqual(
            validate_evolution(self.registry, optional),
            ("accepted", "relation.minor-additive"),
        )
        self.assertEqual(
            validate_evolution(self.registry, required),
            ("rejected", "relation.major-change-required"),
        )

    def test_static_and_dynamic_surfaces_share_exact_ids(self) -> None:
        parity = self.vector("static-dynamic-column-parity")["input"]
        mismatch = self.vector("static-dynamic-column-mismatch")["input"]
        self.assertEqual(
            compare_api_ids(parity),
            ("accepted", "relation.api-id-parity"),
        )
        self.assertEqual(
            compare_api_ids(mismatch),
            ("rejected", "relation.api-id-mismatch"),
        )

    def test_external_relation_needs_no_central_code_or_source_list_change(self) -> None:
        value = self.vector("external-relation-registration")["input"]
        self.assertEqual(
            register_extension(self.registry, value, self.registry_schema),
            ("accepted", "relation.extension-code-diff-zero"),
        )
        changed = copy.deepcopy(value)
        changed["central_source_list_changes"] = 1
        self.assertEqual(
            register_extension(self.registry, changed, self.registry_schema),
            ("rejected", "relation.extension-code-change"),
        )

    def test_registry_schema_rejects_bootstrap_shape(self) -> None:
        candidate = copy.deepcopy(self.registry)
        candidate["maturity"] = "bootstrap"
        with self.assertRaisesRegex(RelationContractError, "relation.schema-invalid"):
            schema_validate(candidate, self.registry_schema, "relation registry")

    def test_report_contains_exact_descriptor_digests(self) -> None:
        results = validate_vectors(self.registry, self.registry_schema, self.vectors)
        report = make_report(self.registry, results)
        self.assertEqual(report["status"], "green")
        self.assertEqual(len(report["descriptors"]), 18)
        self.assertEqual(len({row["digest"] for row in report["descriptors"]}), 18)

    def test_registry_digest_ignores_authority_metadata_and_relation_order(self) -> None:
        left = copy.deepcopy(self.registry)
        right = copy.deepcopy(self.registry)
        right["authority"]["owner_issue"] = "#999"
        right["relations"].reverse()
        self.assertEqual(
            registry_semantic_projection(left),
            registry_semantic_projection(right),
        )


if __name__ == "__main__":
    unittest.main()
