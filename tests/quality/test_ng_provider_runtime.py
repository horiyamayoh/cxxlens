#!/usr/bin/env python3
"""Direct runtime contract tests for Protocol 2/task v4."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_provider_runtime as runtime  # noqa: E402


class ProviderRuntime2Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = runtime.load(
            ROOT / "schemas/cxxlens_ng_provider_runtime_contract.yaml"
        )
        cls.protocol = runtime.load(
            ROOT / "schemas/cxxlens_ng_provider_protocol_v2.yaml"
        )

    def test_repository_runtime_contract(self) -> None:
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/quality/check_ng_provider_runtime.py"),
                "check",
                "--root",
                str(ROOT),
            ],
            check=True,
        )

    def test_host_input_binds_protocol2_request22_and_taskv4(self) -> None:
        runtime.validate_host_input_authority(self.contract, self.protocol)
        self.assertEqual(self.protocol["compatibility"]["accepted_major"], 2)
        self.assertEqual(self.protocol["compatibility"]["accepted_minor"], 0)
        self.assertEqual(self.protocol["request_task"]["request_version"], "2.2.0")
        self.assertEqual(
            self.protocol["request_task"]["task_schema"], "cxxlens.clang22.task.v4"
        )

    def test_task_codec_requires_one_v4_marker_and_no_legacy_marker(self) -> None:
        codec = "const char* codec = \"cxxlens.clang22.task.v4\";"
        runtime.validate_task_codec_markers(codec)
        with self.assertRaisesRegex(runtime.ContractError, "exactly one installed task.v4"):
            runtime.validate_task_codec_markers(
                codec.replace("cxxlens.clang22.task.v4", "task-v4-missing")
            )
        with self.assertRaisesRegex(runtime.ContractError, "exactly one installed task.v4"):
            runtime.validate_task_codec_markers(codec + codec)

    def test_runtime_keeps_binary_identity_sandbox_and_resource_contracts(self) -> None:
        budget = self.contract["runtime"]["budget"]
        self.assertEqual(
            budget["process_isolation_only"]["address_space_bytes"],
            "rlimit-as-not-rss",
        )
        self.assertEqual(
            budget["process_isolation_only"]["transport_bytes"],
            "combined-stdout-stderr-drain-and-rlimit-fsize",
        )
        self.assertEqual(
            self.contract["runtime"]["launch"]["executable_binding"]["digest_subject"],
            "exact-sealed-image-bytes",
        )
        self.assertEqual(
            self.contract["runtime"]["sandbox"]["evidence_digest_v3"][2],
            "measured-executable-digest",
        )

    def test_contract_schema_has_no_source_implementation_sha_authority(self) -> None:
        protocol = yaml.safe_load(
            (ROOT / "schemas/cxxlens_ng_provider_protocol_v2.yaml").read_text(
                encoding="utf-8"
            )
        )
        self.assertNotIn("implementation_byte_identity", protocol["compatibility"])
        self.assertNotIn("implementation_byte_sha256", protocol["compatibility"])


if __name__ == "__main__":
    unittest.main()
