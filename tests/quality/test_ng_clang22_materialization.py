#!/usr/bin/env python3
"""Focused product tests for the Protocol 2/request 2.2 materializer boundary."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_clang22_materialization as materialization  # noqa: E402
import check_ng_provider_protocol as protocol  # noqa: E402


class MaterializationProtocol2Tests(unittest.TestCase):
    def test_request_schema_is_v2_2_and_does_not_embed_source_bytes(self) -> None:
        request = yaml.safe_load(
            (ROOT / materialization.REQUEST_SCHEMA).read_text(encoding="utf-8")
        )
        self.assertEqual(
            request["properties"]["request_version"]["const"], "2.2.0"
        )
        request_text = (ROOT / materialization.REQUEST_SCHEMA).read_text(
            encoding="utf-8"
        )
        self.assertNotIn("content_base64", request_text)
        worker = request["properties"]["worker"]["properties"]
        self.assertEqual(worker["protocol_major"]["const"], 2)
        self.assertEqual(worker["protocol_minor"]["const"], 0)

    def test_task_v4_schema_has_metadata_only_source_binding(self) -> None:
        task = yaml.safe_load(
            (ROOT / materialization.TASK_V4_SCHEMA).read_text(encoding="utf-8")
        )
        self.assertEqual(
            task["properties"]["schema"]["const"], "cxxlens.clang22.task.v4"
        )
        self.assertIn("base_task_digest", task["required"])
        self.assertEqual(
            {
                field
                for field in task["required"]
                if field.startswith("base_task_") and field.endswith("_digest")
            },
            {"base_task_digest"},
        )
        self.assertNotIn(
            "content_base64",
            (ROOT / materialization.TASK_V4_SCHEMA).read_text(encoding="utf-8"),
        )

    def test_materializer_checker_rejects_protocol_downgrade(self) -> None:
        protocol_contract = yaml.safe_load(
            (ROOT / materialization.PROVIDER_PROTOCOL).read_text(encoding="utf-8")
        )
        changed = copy.deepcopy(protocol_contract)
        changed["compatibility"]["accepted_minor"] = 1
        with self.assertRaises(protocol.ProviderContractError):
            protocol.validate_task_input_authority(changed)
        result = materialization.validate_v2_2_documents(ROOT)
        self.assertEqual(result["versioning"]["request"], "2.2.0")
        self.assertNotEqual(
            changed["compatibility"]["accepted_minor"],
            protocol_contract["compatibility"]["accepted_minor"],
        )

    def test_task_v4_projection_has_metadata_only_source_binding(self) -> None:
        task = {
            "schema": "cxxlens.clang22.task.v4",
            "task_id": "task:semantic-v2:sha256:" + "a" * 64,
            "selected_catalog_compile_unit_id": "catalog-compile-unit:one",
            "compile_unit_id": "compile-unit:one",
            "source": {
                "source_snapshot_id": "snapshot:one",
                "file_id": "file:one",
                "logical_path": "project://src/main.cc",
                "content_digest": "sha256:" + "b" * 64,
                "size_bytes": 1,
                "encoding": "utf8",
                "line_index_id": "line-index:one",
                "read_only": True,
            },
        }
        request = {
            "project": {
                "catalog_id": "catalog:one",
                "catalog_digest": "semantic-v2:sha256:" + "c" * 64,
                "logical_root": "project://",
                "catalog_environment_digest": "sha256:" + "d" * 64,
                "catalog_compile_units": [],
            },
            "tasks": [task],
        }
        projection = materialization.worker_task_v4_projection(request, task)
        self.assertNotIn(b"content_base64", projection)
        self.assertIn(b"cxxlens.clang22.task.v4", projection)
        legacy = copy.deepcopy(request)
        legacy["tasks"][0]["source"]["content_base64"] = "YQ=="
        with self.assertRaises(materialization.MaterializationError):
            materialization.worker_task_v4_projection(
                legacy, legacy["tasks"][0]
            )

    def test_protocol2_contract_preserves_wire_and_content_integrity(self) -> None:
        contract = protocol.load_yaml(ROOT / protocol.CONTRACT)
        protocol.validate_contract_shape(contract)
        self.assertEqual(contract["wire"]["fixed_header_bytes"], protocol.FRAME.size)
        self.assertEqual(contract["wire"]["checksums"], "independent-full-sha256")
        # Implementation bytes are not part of the protocol authority.  The
        # product contract retains only the fixed wire framing and payload
        # integrity checks above.
        self.assertNotIn("implementation_byte_sha256", contract["compatibility"])
        self.assertNotIn("implementation_byte_identity", contract["compatibility"])


if __name__ == "__main__":
    unittest.main()
