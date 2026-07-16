#!/usr/bin/env python3
"""Property, mutation, and failure-isolation tests for the NG store contract."""

from __future__ import annotations

import copy
import itertools
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_snapshot_store_contract import (  # noqa: E402
    CONTRACT,
    CLOSURE_FIELDS,
    SELECTOR_FIELDS,
    StoreContractError,
    canonical_binary,
    claim_identity,
    closure_binding,
    closure_mutation_matrix,
    compact,
    format_open,
    identity_digest,
    load_yaml,
    producer_basis,
    publish,
    select_current,
    series_id,
    snapshot_digest_matrix,
    validate_all,
    validate_identity_graph,
)


class NgSnapshotStoreContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = load_yaml(ROOT / CONTRACT)

    def test_contract_and_exact_vector_set(self) -> None:
        contract, results, comparisons = validate_all(ROOT)
        self.assertEqual(contract["maturity"], "accepted")
        self.assertEqual(len(results), 31)
        self.assertEqual(comparisons, 36)

    def test_binary_encoding_separates_types_and_boundaries(self) -> None:
        values = [None, False, 0, b"0", "0", ["a", "bc"], ["ab", "c"]]
        encoded = [canonical_binary(value) for value in values]
        self.assertEqual(len(encoded), len(set(encoded)))

    def test_domain_separation_changes_digest(self) -> None:
        fields = ["same", 1]
        self.assertNotEqual(
            identity_digest("semantic-key", fields),
            identity_digest("assertion", fields),
        )

    def test_identity_graph_is_dag_and_cycle_fails(self) -> None:
        order = validate_identity_graph(self.contract)
        self.assertLess(order.index("partition_content_digest"), order.index("snapshot_id"))
        with self.assertRaisesRegex(StoreContractError, "identity-cycle"):
            validate_identity_graph(
                self.contract,
                {"node": "partition_content_digest", "dependency": "snapshot_id"},
            )

    def test_claim_identity_has_no_containing_snapshot_dependency(self) -> None:
        value = {
            "relation_descriptor_id": "cc.entity.v1",
            "semantic_major": 1,
            "authoritative_key_tuple": ["entity-1"],
            "condition_universe_id": "universe-1",
            "canonical_condition": "true",
            "interpretation_domain_id": "cc.canonical-1",
            "producer_semantic_contract": "sha256:" + "a" * 64,
            "authoritative_payload_tuple": ["function"],
        }
        ids = claim_identity(value)
        self.assertEqual(set(ids), {"semantic_key_id", "assertion_id", "content_digest"})
        with self.assertRaisesRegex(StoreContractError, "containing-snapshot"):
            claim_identity(dict(value, containing_snapshot_id="snapshot-output"))

    def test_direct_and_derived_basis_are_tagged(self) -> None:
        direct = producer_basis(
            {"kind": "direct", "basis_digest": "sha256:" + "a" * 64}
        )
        derived = producer_basis(
            {
                "kind": "derived",
                "input_snapshot": "snapshot-prior",
                "input_generation": 1,
                "output_generation": 2,
                "consumed_partition_content_digests": ["partition-a"],
                "transform_semantics": "sha256:" + "b" * 64,
            }
        )
        self.assertNotEqual(direct, derived)
        with self.assertRaisesRegex(StoreContractError, "direct-basis-snapshot"):
            producer_basis(
                {
                    "kind": "direct",
                    "basis_digest": "sha256:" + "a" * 64,
                    "input_snapshot": "snapshot-prior",
                }
            )

    def test_every_closure_identity_field_is_digest_bound(self) -> None:
        value = {field: f"value-{index}" for index, field in enumerate(CLOSURE_FIELDS)}
        baseline = closure_binding(value)
        result = closure_mutation_matrix(value)
        self.assertEqual(result["distinct_ids"], len(CLOSURE_FIELDS) + 1)
        for field in CLOSURE_FIELDS:
            changed = copy.deepcopy(value)
            changed[field] += "-changed"
            self.assertNotEqual(baseline, closure_binding(changed), field)

    def test_snapshot_digest_is_invariant_under_all_perturbations(self) -> None:
        base = {
            "snapshot_semantics_version": "1.0.0",
            "catalog_semantic_digest": "sha256:" + "1" * 64,
            "condition_universe_id": "universe-1",
            "relation_registry_digest": "sha256:" + "2" * 64,
            "interpretation_policy_digest": "sha256:" + "3" * 64,
            "closure_ids": ["closure-1"],
            "partitions": [],
        }
        for index in range(3):
            base["partitions"].append(
                {
                    "relation_descriptor_id": "cc.entity.v1",
                    "scope": f"scope-{index}",
                    "condition": "condition-1",
                    "interpretation": "cc.canonical-1",
                    "producer_semantics": "sha256:" + "4" * 64,
                    "input_basis_digest": "sha256:" + "5" * 64,
                    "precision_profile": "exact",
                    "assumption_set_id": "empty",
                    "claim_content_digests": [f"claim-{index}"],
                    "coverage_units": [f"covered-{index}"],
                }
            )
        _, comparisons = snapshot_digest_matrix(base)
        self.assertEqual(comparisons, 36)

    def test_series_selector_has_no_ambient_defaults(self) -> None:
        selector = {field: f"value-{field}" for field in SELECTOR_FIELDS}
        self.assertTrue(series_id(selector).startswith("snapshot-series:sha256:"))
        for field in SELECTOR_FIELDS:
            incomplete = dict(selector)
            del incomplete[field]
            with self.assertRaisesRegex(StoreContractError, "selection-authority"):
                series_id(incomplete)

    def test_current_does_not_fallback_from_corrupt_head(self) -> None:
        selector = {field: f"value-{field}" for field in SELECTOR_FIELDS}
        publications = [
            {
                "publication_id": "p1",
                "selector": selector,
                "sequence": 1,
                "state": "committed",
                "physical_state": "intact",
                "snapshot_id": "s1",
            },
            {
                "publication_id": "p2",
                "selector": selector,
                "sequence": 2,
                "state": "committed",
                "physical_state": "corrupt",
                "snapshot_id": "s2",
            },
        ]
        with self.assertRaisesRegex(StoreContractError, "current-corrupt"):
            select_current({"selector": selector, "publications": publications})

    def test_failed_publish_and_compaction_preserve_prior(self) -> None:
        head, reason = publish(
            {
                "current_head": "p1",
                "expected_parent": "p1",
                "candidate": "p2",
                "validated": False,
                "history": ["created", "staged", "validating", "rejected", "rolled_back"],
            }
        )
        self.assertEqual((head, reason), ("p1", "store.publish-failure-isolated"))
        generation, reason = compact(
            {
                "current_generation": "g1",
                "candidate_generation": "g2",
                "pinned_generations": ["g1"],
                "current_semantic_digest": "d1",
                "candidate_semantic_digest": "d1",
                "candidate_valid": False,
            }
        )
        self.assertEqual(generation["active_generation"], "g1")
        self.assertEqual(reason, "store.compact-failure-isolated")

    def test_format_migration_never_changes_semantic_identity(self) -> None:
        digest = "sha256:" + "a" * 64
        result, reason = format_open(
            {
                "source_format": "1.0.0",
                "reader_major": 2,
                "semantic_digest": digest,
                "migrations": [
                    {"from_major": 1, "to_major": 2, "result_semantic_digest": digest}
                ],
            }
        )
        self.assertEqual(result, digest)
        self.assertEqual(reason, "store.format_migration-valid")
        for changed in ("sha256:" + "b" * 64, "sha256:" + "c" * 64):
            with self.assertRaisesRegex(StoreContractError, "semantic-drift"):
                format_open(
                    {
                        "source_format": "1.0.0",
                        "reader_major": 2,
                        "semantic_digest": digest,
                        "migrations": [
                            {
                                "from_major": 1,
                                "to_major": 2,
                                "result_semantic_digest": changed,
                            }
                        ],
                    }
                )

    def test_canonical_set_inputs_are_order_invariant(self) -> None:
        values = ["a", "b", "c"]
        digests = {
            identity_digest("claim-set", sorted(permutation))
            for permutation in itertools.permutations(values)
        }
        self.assertEqual(len(digests), 1)


if __name__ == "__main__":
    unittest.main()
