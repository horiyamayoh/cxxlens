#!/usr/bin/env python3
"""Protocol 2 contract authority, identity, and failure tests."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_provider_protocol import (  # noqa: E402
    PROTOCOL_MAJOR,
    PROTOCOL_MINOR,
    ProviderContractError,
    load_yaml,
    validate_shared_coverage_records,
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

    def test_shared_coverage_retains_opaque_records_in_wire_order(self) -> None:
        records = [
            {"kind": "task", "id": "task-1", "state": "covered", "reason": ""},
            {
                "kind": "relation",
                "id": "relation-1",
                "state": "unresolved",
                "reason": "provider-not-admitted",
            },
        ]
        retained = validate_shared_coverage_records("task-1", records)
        self.assertEqual(retained, records)
        self.assertIsNot(retained, records)
        self.assertIsNot(retained[0], records[0])

    def test_shared_coverage_rejects_missing_or_duplicate_task_record(self) -> None:
        records = [{"kind": "relation", "id": "relation-1", "state": "covered", "reason": ""}]
        with self.assertRaisesRegex(ProviderContractError, "coverage-incomplete"):
            validate_shared_coverage_records("task-1", records)
        records.insert(
            0, {"kind": "task", "id": "task-1", "state": "covered", "reason": ""}
        )
        records.append(records[0].copy())
        with self.assertRaisesRegex(ProviderContractError, "coverage-incomplete"):
            validate_shared_coverage_records("task-1", records)

    def test_protocol_resource_limit_mutation_is_rejected(self) -> None:
        changed = copy.deepcopy(self.contract)
        changed["wire"]["limits"]["payload_bytes"] += 1
        with self.assertRaises(ProviderContractError):
            validate_contract_shape(changed)


if __name__ == "__main__":
    unittest.main()
