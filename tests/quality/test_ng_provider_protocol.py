#!/usr/bin/env python3
"""Properties and fault isolation tests for the NG provider protocol."""

from __future__ import annotations

import copy
import itertools
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_provider_protocol import (  # noqa: E402
    CONTRACT,
    FRAME,
    ProviderContractError,
    cbor_decode,
    cbor_encode,
    decode_frame,
    encode_frame,
    failure,
    flow,
    group,
    load_yaml,
    negotiate,
    plan,
    reuse,
    run_fuzz,
    sample_manifest,
    sample_task,
    schema_validate,
    surface_parity,
    validate_all,
)


class NgProviderProtocolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = load_yaml(ROOT / CONTRACT)

    def test_contract_vectors_fuzz_and_surface_matrix(self) -> None:
        contract, results, comparisons, fuzz_cases = validate_all(ROOT)
        self.assertEqual(contract["maturity"], "accepted")
        self.assertEqual(len(results), 34)
        self.assertEqual(comparisons, 6)
        self.assertEqual(fuzz_cases, 19)

    def test_fixed_header_is_exactly_104_bytes(self) -> None:
        self.assertEqual(FRAME.size, 104)
        self.assertEqual(self.contract["wire"]["fixed_header_bytes"], 104)

    def test_cbor_round_trip_and_map_order_are_deterministic(self) -> None:
        value = {"z": [1, True, None], "a": b"bytes", "longer": -2, "unicode": "\0€😀"}
        encoded = cbor_encode(value)
        self.assertEqual(cbor_decode(encoded), value)
        for permutation in itertools.permutations(value.items()):
            self.assertEqual(cbor_encode(dict(permutation)), encoded)

    def test_cbor_text_utf8_differential_corpus_is_strict(self) -> None:
        for invalid in (
            b"\x61\x80",
            b"\x61\xc2",
            b"\x62\xc0\x80",
            b"\x63\xe0\x80\x80",
            b"\x64\xf0\x80\x80\x80",
            b"\x63\xed\xa0\x80",
            b"\x64\xf4\x90\x80\x80",
        ):
            with self.assertRaisesRegex(ProviderContractError, "malformed-frame"):
                cbor_decode(invalid)

    def test_frame_round_trip_preserves_control_and_payload(self) -> None:
        frame = encode_frame({"task": "t1"}, b"payload", message_type=9, stream_id=2, sequence=3)
        decoded = decode_frame(self.contract, frame)
        self.assertEqual(decoded["control"], {"task": "t1"})
        self.assertEqual(decoded["payload_hex"], b"payload".hex())
        self.assertEqual(decoded["sequence"], 3)
        self.assertEqual(decoded["protocol_minor"], 0)
        self.assertEqual(decoded["flags"], 0)

    def test_wire_version_and_unknown_message_classification_fail_closed(self) -> None:
        with self.assertRaisesRegex(ProviderContractError, "protocol-major-mismatch"):
            decode_frame(self.contract, encode_frame({}, protocol_major=2))
        with self.assertRaisesRegex(ProviderContractError, "protocol-minor-mismatch"):
            decode_frame(self.contract, encode_frame({}, protocol_minor=1))
        accepted = decode_frame(
            self.contract,
            encode_frame({}, protocol_minor=1),
            negotiated_minor=1,
        )
        self.assertEqual(accepted["protocol_minor"], 1)
        with self.assertRaisesRegex(ProviderContractError, "unknown-message-type"):
            decode_frame(self.contract, encode_frame({}, message_type=65000))

    def test_frame_flags_are_fail_closed_and_optional_extensions_are_accounted(self) -> None:
        flags = self.contract["wire"]["flags"]
        optional = decode_frame(
            self.contract,
            encode_frame({}, message_type=65000, flags=flags["optional_extension"]),
        )
        self.assertTrue(optional["skipped_optional"])
        self.assertEqual(optional["message_type"], 65000)
        self.assertGreater(optional["accounted_bytes"], FRAME.size)
        with self.assertRaisesRegex(ProviderContractError, "unknown-required-extension"):
            decode_frame(
                self.contract,
                encode_frame({}, message_type=65000, flags=flags["required_extension"]),
            )
        with self.assertRaisesRegex(ProviderContractError, "unsupported-compression"):
            decode_frame(
                self.contract,
                encode_frame({}, flags=flags["compressed_payload"]),
            )
        with self.assertRaisesRegex(ProviderContractError, "invalid-frame-flags"):
            decode_frame(self.contract, encode_frame({}, flags=16))

    def test_unhashable_cbor_map_key_is_stable_rejection(self) -> None:
        with self.assertRaisesRegex(ProviderContractError, "unhashable CBOR map key"):
            cbor_decode(b"\xa1\x80\x01")

    def test_credit_is_two_dimensional_and_sequence_contiguous(self) -> None:
        base = {"stream_id": 7, "staged_digest": "digest"}
        with self.assertRaisesRegex(ProviderContractError, "credit-exceeded"):
            flow({**base, "credit": {"bytes": 1, "frames": 1}, "frames": [{"sequence": 0, "bytes": 2}], "ack": {"stream_id": 7, "highest_contiguous_sequence": 0, "staged_digest": "digest", "return_bytes": 0, "return_frames": 0}})
        with self.assertRaisesRegex(ProviderContractError, "sequence-gap"):
            flow({**base, "credit": {"bytes": 10, "frames": 1}, "frames": [{"sequence": 1, "bytes": 1}], "ack": {"stream_id": 7, "highest_contiguous_sequence": 1, "staged_digest": "digest", "return_bytes": 0, "return_frames": 0}})

    def test_ack_binds_stream_and_staged_digest(self) -> None:
        value = {"stream_id": 7, "staged_digest": "digest", "credit": {"bytes": 10, "frames": 1}, "frames": [{"sequence": 0, "bytes": 1}], "ack": {"stream_id": 8, "highest_contiguous_sequence": 0, "staged_digest": "digest", "return_bytes": 0, "return_frames": 0}}
        with self.assertRaisesRegex(ProviderContractError, "ack-invalid"):
            flow(value)

    def test_negotiation_never_uses_adjacent_provider(self) -> None:
        value = {"host": {"major": 1, "minor": 0, "features": ["streaming"]}, "requested": {"provider_id": "p", "provider_version": "1.0.0", "binary_digest": "d"}, "offered": {"provider_id": "p", "provider_version": "1.1.0", "binary_digest": "d", "semantic_contract_digest": "s", "protocol": {"major": 1, "minimum_minor": 0, "maximum_minor": 1}, "required_features": ["streaming"], "optional_features": []}}
        with self.assertRaisesRegex(ProviderContractError, "adjacent-fallback"):
            negotiate(value)

    def test_hard_reference_crossing_dependency_group_rejects(self) -> None:
        base = {"state": "sealed", "batches_sealed": True, "digests_valid": True, "coverage_balanced": True, "unresolved_accounted": True, "closures_valid": True}
        value = {"partial_policy": "declared_dependency_groups", "groups": [dict(base, id="d1", atomic_groups=["a"], hard_references=[{"source": "a", "target": "b"}]), dict(base, id="d2", atomic_groups=["b"], hard_references=[])]}
        with self.assertRaisesRegex(ProviderContractError, "hard-reference-group"):
            group(value)

    def test_partial_adoption_requires_predeclared_boundary(self) -> None:
        complete = {"id": "d1", "state": "sealed", "atomic_groups": ["a"], "batches_sealed": True, "digests_valid": True, "coverage_balanced": True, "unresolved_accounted": True, "closures_valid": True, "hard_references": []}
        failed = dict(complete, id="d2", state="streaming", atomic_groups=["b"], batches_sealed=False)
        declared, _ = group({"partial_policy": "declared_dependency_groups", "fail_group": "d2", "groups": [complete, failed]})
        forbidden, _ = group({"partial_policy": "forbid", "fail_group": "d2", "groups": [complete, failed]})
        self.assertEqual(declared["adopted"], ["d1"])
        self.assertEqual(forbidden["adopted"], [])

    def test_plan_order_is_input_order_invariant(self) -> None:
        tasks = [
            {"id": "a", "provider_id": "p.a", "provider_version": "1", "binary_digest": "a", "input_stage": "observation", "output_stage": "assertion", "depends_on": []},
            {"id": "b", "provider_id": "p.b", "provider_version": "1", "binary_digest": "b", "input_stage": "assertion", "output_stage": "canonical_claim", "depends_on": ["a"]},
        ]
        outputs = {tuple(plan({"profile": "NG0", "tasks": list(permutation)})[0]) for permutation in itertools.permutations(tasks)}
        self.assertEqual(outputs, {("a", "b")})

    def test_ng0_cycle_never_uses_tie_break(self) -> None:
        tasks = [{"id": "a", "provider_id": "p", "provider_version": "1", "binary_digest": "d", "input_stage": "canonical_claim", "output_stage": "derived_claim", "depends_on": ["a"]}]
        with self.assertRaisesRegex(ProviderContractError, "dependency-cycle"):
            plan({"profile": "NG0", "tasks": tasks})

    def test_binary_and_semantic_digest_both_invalidate_reuse(self) -> None:
        stored = {field: field for field in ("provider_id", "provider_version", "semantic_contract_digest", "binary_digest", "protocol_major", "relation_descriptor_digests", "input_partition_digests", "condition_universe", "interpretation", "model_assumption_pack")}
        self.assertEqual(reuse({"stored": stored, "requested": copy.deepcopy(stored)}), "reusable")
        with self.assertRaisesRegex(ProviderContractError, "binary-mismatch"):
            reuse({"stored": stored, "requested": dict(stored, binary_digest="changed")})
        with self.assertRaisesRegex(ProviderContractError, "semantic-mismatch"):
            reuse({"stored": stored, "requested": dict(stored, semantic_contract_digest="changed")})

    def test_failure_never_mutates_prior_snapshot(self) -> None:
        result = failure({"reason": "provider.crash", "coverage_accounted": True, "unresolved_accounted": True, "current_group": "d2", "adopted_groups": ["d1"], "partial_policy": "declared_dependency_groups"}, self.contract)
        self.assertEqual(result["prior_snapshot"], "unchanged")
        self.assertEqual(result["retained"], ["d1"])

    def test_in_process_and_out_of_process_are_semantically_equal(self) -> None:
        import check_ng_provider_protocol as module
        module._CONTRACT_CACHE = self.contract
        _, comparisons = surface_parity({"rows": [{"key": "b"}, {"key": "a"}], "coverage": ["covered"]})
        self.assertEqual(comparisons, 6)

    def test_fuzz_corpus_has_only_stable_rejections(self) -> None:
        corpus = load_yaml(ROOT / "schemas/cxxlens_ng_provider_fuzz_corpus.yaml")
        result = run_fuzz(self.contract, corpus)
        self.assertEqual(result["stable_rejections"], len(corpus["cases"]))
        self.assertEqual(result["crashes"], 0)

    def test_manifest_and_task_examples_are_schema_valid(self) -> None:
        schema_validate(sample_manifest(), load_yaml(ROOT / "schemas/cxxlens_ng_provider_manifest.schema.yaml"), "manifest")
        schema_validate(sample_task(), load_yaml(ROOT / "schemas/cxxlens_ng_provider_task.schema.yaml"), "task")

    def test_exact_contract_schema_rejects_limit_policy_drift(self) -> None:
        changed = copy.deepcopy(self.contract)
        changed["wire"]["limits"]["payload_bytes"] += 1
        with self.assertRaisesRegex(ProviderContractError, "schema-invalid"):
            schema_validate(changed, load_yaml(ROOT / "schemas/cxxlens_ng_provider_protocol.schema.yaml"), "provider protocol")


if __name__ == "__main__":
    unittest.main()
