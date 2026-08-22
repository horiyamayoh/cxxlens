#!/usr/bin/env python3
"""Compatibility v2 and ordinary support-table tests."""

from __future__ import annotations

import copy
import contextlib
import json
import pathlib
import shutil
import sys
import tempfile
import unittest

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_release_contract as contract  # noqa: E402


class CompatibilityV2Test(unittest.TestCase):
    @contextlib.contextmanager
    def support_root(self, document: dict[str, object]):
        """Create the smallest repository fixture for support-table checks."""
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            schema_path = root / contract.SUPPORT_SCHEMA
            schema_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / contract.SUPPORT_SCHEMA, schema_path)
            table_path = root / contract.SUPPORT_TABLE
            table_path.parent.mkdir(parents=True, exist_ok=True)
            table_path.write_text(
                yaml.safe_dump(document, sort_keys=False), encoding="utf-8"
            )
            yield root

    def axes(self) -> list[dict[str, object]]:
        return [
            {
                "axis": "distribution",
                "version": "1.0.0",
                "features": [{"id": "core", "requirement": "required"}],
                "contract_digest": None,
            }
        ]

    def request(self, *, operation: str = "inspect") -> dict[str, object]:
        value: dict[str, object] = {
            "schema": "cxxlens.ng-compatibility-request.v2",
            "request_id": "compat-test",
            "operation": operation,
            "release_id": "distribution-1.0.0",
            "context": "release-startup",
            "required_axes": self.axes(),
            "offered_axes": self.axes(),
        }
        if operation == "doctor":
            value["environment"] = {
                "os": "linux",
                "architecture": "x86_64",
                "toolchain": "clang22",
                "linkage": "static",
            }
        return value

    def test_check_has_no_operational_report_contract(self) -> None:
        contract.validate_repository(ROOT)

    def test_supported_environment_and_axes(self) -> None:
        report = contract.decide(ROOT, self.request(operation="doctor"))
        self.assertEqual(report["schema"], "cxxlens.ng-compatibility-report.v2")
        self.assertEqual(report["decision"], "supported")
        self.assertNotIn("qualification_state", report)
        self.assertNotIn("evidence_refs", json.dumps(report))

    def test_unlisted_windows_environment_is_unsupported(self) -> None:
        request = self.request(operation="doctor")
        request["environment"] = {
            "os": "windows",
            "architecture": "x86_64",
            "toolchain": "msvc19",
            "linkage": "shared",
        }
        report = contract.decide(ROOT, request)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.environment-unsupported", report["reason_codes"])

    def test_old_environment_fields_are_rejected(self) -> None:
        request = self.request(operation="doctor")
        request["environment"]["runtime_qualified"] = True  # type: ignore[index]
        with self.assertRaises(Exception):
            contract.decide(ROOT, request)

    def test_report_schema_rejects_qualification_state(self) -> None:
        report = contract.decide(ROOT, self.request())
        invalid = copy.deepcopy(report)
        invalid["qualification_state"] = "qualified"
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "report.json"
            path.write_text(json.dumps(invalid), encoding="utf-8")
            with self.assertRaises(Exception):
                contract.validate(
                    invalid,
                    contract.load(ROOT / contract.REPORT_SCHEMA),
                    "invalid report",
                )

    def test_report_schema_rejects_release_not_qualified_reason(self) -> None:
        report = contract.decide(ROOT, self.request())
        invalid = copy.deepcopy(report)
        invalid["reason_codes"] = ["compat.release-not-qualified"]
        with self.assertRaises(Exception):
            contract.validate(
                invalid,
                contract.load(ROOT / contract.REPORT_SCHEMA),
                "invalid compatibility reason",
            )

    def test_unlisted_linux_toolchain_is_unsupported(self) -> None:
        request = self.request(operation="doctor")
        request["environment"]["toolchain"] = "gcc14"  # type: ignore[index]
        report = contract.decide(ROOT, request)
        self.assertEqual(report["decision"], "unsupported")

    def test_unlisted_linux_architecture_is_unsupported(self) -> None:
        request = self.request(operation="doctor")
        request["environment"]["architecture"] = "aarch64"  # type: ignore[index]
        report = contract.decide(ROOT, request)
        self.assertEqual(report["decision"], "unsupported")

    def test_unlisted_linkage_is_unsupported(self) -> None:
        request = self.request(operation="doctor")
        request["environment"]["linkage"] = "lto"  # type: ignore[index]
        report = contract.decide(ROOT, request)
        self.assertEqual(report["decision"], "unsupported")

    def test_axis_version_mismatch_is_not_supported(self) -> None:
        request = self.request()
        request["offered_axes"][0]["version"] = "1.0.1"  # type: ignore[index]
        report = contract.decide(ROOT, request)
        self.assertEqual(report["decision"], "unsupported")

    def test_missing_doctor_environment_axis_is_rejected(self) -> None:
        request = self.request(operation="doctor")
        del request["environment"]["architecture"]  # type: ignore[index]
        with self.assertRaises(Exception):
            contract.decide(ROOT, request)

    def test_missing_axis_field_is_rejected(self) -> None:
        request = self.request()
        del request["required_axes"][0]["contract_digest"]  # type: ignore[index]
        with self.assertRaises(Exception):
            contract.decide(ROOT, request)

    def test_contract_digest_mismatch_is_unsupported(self) -> None:
        request = self.request()
        request["offered_axes"][0]["contract_digest"] = "sha256:" + "0" * 64  # type: ignore[index]
        report = contract.decide(ROOT, request)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.contract-digest-mismatch", report["reason_codes"])

    def test_duplicate_axis_is_rejected(self) -> None:
        request = self.request()
        request["offered_axes"].append(copy.deepcopy(request["offered_axes"][0]))  # type: ignore[index]
        with self.assertRaises(contract.ReleaseContractError):
            contract.decide(ROOT, request)

    def test_major_axis_mismatch_is_unsupported(self) -> None:
        request = self.request()
        request["offered_axes"][0]["version"] = "2.0.0"  # type: ignore[index]
        report = contract.decide(ROOT, request)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.axis-major-mismatch", report["reason_codes"])

    def test_older_same_major_axis_requires_migration(self) -> None:
        request = self.request()
        request["required_axes"][0]["version"] = "1.1.0"  # type: ignore[index]
        report = contract.decide(ROOT, request)
        self.assertEqual(report["decision"], "migration-required")
        self.assertIn("compat.migration-required", report["reason_codes"])

    def test_empty_support_table_is_rejected(self) -> None:
        table = copy.deepcopy(contract.load(ROOT / contract.SUPPORT_TABLE))
        table["entries"] = []
        with self.support_root(table) as root:
            with self.assertRaises(contract.ReleaseContractError):
                contract.validate_support_table(root)

    def test_duplicate_support_environment_is_rejected(self) -> None:
        table = copy.deepcopy(contract.load(ROOT / contract.SUPPORT_TABLE))
        table["entries"].append(copy.deepcopy(table["entries"][0]))
        with self.support_root(table) as root:
            with self.assertRaises(contract.ReleaseContractError):
                contract.validate_support_table(root)

    def test_windows_and_msvc_support_rows_are_rejected(self) -> None:
        for field, value in (("os", "windows"), ("compiler_provider_major", "msvc19")):
            with self.subTest(field=field):
                table = copy.deepcopy(contract.load(ROOT / contract.SUPPORT_TABLE))
                row = copy.deepcopy(table["entries"][0])
                row["surface"] = "invalid-surface"
                row[field] = value
                table["entries"].append(row)
                with self.support_root(table) as root:
                    with self.assertRaisesRegex(
                        contract.ReleaseContractError,
                        "Windows/MSVC must remain unsupported and unlisted",
                    ):
                        contract.validate_support_table(root)


if __name__ == "__main__":
    unittest.main()
