#!/usr/bin/env python3
"""Direct Protocol 2.0 wire, identity, and failure tests."""

from __future__ import annotations

import copy
import itertools
import pathlib
import sys
import unittest

import jsonschema


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_provider_protocol import (  # noqa: E402
    FRAME,
    PROTOCOL_MAJOR,
    PROTOCOL_MINOR,
    ProviderContractError,
    cbor_decode,
    cbor_encode,
    decode_frame,
    encode_frame,
    load_yaml,
    validate_contract_shape,
)


class ProviderProtocol2Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = load_yaml(ROOT / "schemas/cxxlens_ng_provider_protocol_v2.yaml")

    def test_protocol2_authority_rejects_unsupported_peer(self) -> None:
        compatibility = self.contract["compatibility"]
        self.assertEqual(compatibility["accepted_major"], PROTOCOL_MAJOR)
        self.assertEqual(compatibility["accepted_minor"], PROTOCOL_MINOR)
        self.assertEqual(compatibility["downgrade"], "reject")
        self.assertEqual(compatibility["unsupported_peer"], "reject-before-payload")
        validate_contract_shape(self.contract)

    def test_fixed_header_and_canonical_cbor_are_deterministic(self) -> None:
        self.assertEqual(FRAME.size, 104)
        value = {"z": [1, True, None], "a": b"bytes", "unicode": "\0€😀"}
        encoded = cbor_encode(value)
        self.assertEqual(cbor_decode(encoded), value)
        for permutation in itertools.permutations(value.items()):
            self.assertEqual(cbor_encode(dict(permutation)), encoded)

    def test_frame_round_trip_and_digest_tamper_rejection(self) -> None:
        frame = encode_frame({"task": "t1"}, b"payload", message_type=9, sequence=3)
        decoded = decode_frame(self.contract, frame)
        self.assertEqual(decoded["protocol_major"], PROTOCOL_MAJOR)
        self.assertEqual(decoded["protocol_minor"], PROTOCOL_MINOR)
        self.assertEqual(decoded["payload_hex"], b"payload".hex())
        tampered = frame[:-1] + bytes([frame[-1] ^ 1])
        with self.assertRaisesRegex(ProviderContractError, "checksum-mismatch"):
            decode_frame(self.contract, tampered)

    def test_major_downgrade_and_reserved_flags_fail_closed(self) -> None:
        with self.assertRaisesRegex(ProviderContractError, "protocol-major-mismatch"):
            decode_frame(
                self.contract,
                encode_frame({}, protocol_major=1),
            )
        with self.assertRaisesRegex(ProviderContractError, "protocol-minor-mismatch"):
            decode_frame(
                self.contract,
                encode_frame({}, protocol_minor=1),
            )
        with self.assertRaisesRegex(ProviderContractError, "invalid-frame-flags"):
            decode_frame(self.contract, encode_frame({}, flags=4))

    def test_contract_mutation_is_rejected_without_fixed_cardinality_assertions(self) -> None:
        changed = copy.deepcopy(self.contract)
        changed["wire"]["limits"]["payload_bytes"] += 1
        with self.assertRaises(ProviderContractError):
            validate_contract_shape(changed)


if __name__ == "__main__":
    unittest.main()
