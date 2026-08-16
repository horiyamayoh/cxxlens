#!/usr/bin/env python3
"""Contract and adversarial tests for the readiness-derived #275 projection."""

from __future__ import annotations

import copy
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/quality"))

import check_ng_use_case_capability_catalog as catalog  # noqa: E402


class UseCaseCapabilityCatalogTests(unittest.TestCase):
    def readiness(self) -> dict:
        document = catalog.load_yaml(ROOT / catalog.READINESS_PATH)
        self.assertIsInstance(document, dict)
        return document

    def test_main_readiness_projects_to_deterministic_demand_registry(self) -> None:
        report = catalog.build_report(ROOT)
        self.assertEqual(report["source"]["tracking_issue"], "#275")
        self.assertEqual(report["projection"]["status"], "declaration-only-not-qualified")
        self.assertEqual(len(report["use_cases"]), 7)
        self.assertEqual(
            report["use_cases"][0]["id"],
            "abi-ir-binary-evidence",
        )
        self.assertIn(
            {"id": "source-closure", "demanded_by": ["repository-semantic-query"]},
            report["capability_registry"],
        )
        self.assertEqual(
            report["use_cases"],
            sorted(report["use_cases"], key=lambda entry: entry["id"]),
        )
        catalog.validate_report(report)

    def test_duplicate_use_case_is_rejected(self) -> None:
        readiness = self.readiness()
        families = readiness["product_direction"]["roadmap"]["use_case_families"]
        families.append(copy.deepcopy(families[0]))
        with self.assertRaisesRegex(catalog.CatalogError, "duplicate or invalid use-case ID"):
            catalog.project_use_cases(readiness)

    def test_duplicate_capability_reference_is_rejected(self) -> None:
        readiness = self.readiness()
        families = readiness["product_direction"]["roadmap"]["use_case_families"]
        families[0]["capabilities"].append(families[0]["capabilities"][0])
        with self.assertRaisesRegex(catalog.CatalogError, "duplicate capability reference"):
            catalog.project_use_cases(readiness)

    def test_wrong_authority_contract_is_rejected(self) -> None:
        readiness = self.readiness()
        readiness["product_direction"]["contract"] = "invented.contract.v1"
        with self.assertRaisesRegex(catalog.CatalogError, "accepted direction"):
            catalog.project_use_cases(readiness)

    def test_report_cannot_claim_provider_or_qualification_bindings(self) -> None:
        report = catalog.build_report(ROOT)
        report["projection"]["excludes"].remove("provider-bindings")
        with self.assertRaises(catalog.CatalogError):
            catalog.validate_report(report)

    def test_dirty_readiness_source_is_rejected_before_report_binding(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for relative_path in catalog.CATALOG_SOURCE_PATHS:
                destination = root / relative_path
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / relative_path, destination)
            subprocess.run(["git", "init", "--quiet", str(root)], check=True)
            subprocess.run(
                [
                    "git",
                    "-C",
                    str(root),
                    "add",
                    "--",
                    *(path.as_posix() for path in catalog.CATALOG_SOURCE_PATHS),
                ],
                check=True,
            )
            subprocess.run(
                [
                    "git",
                    "-C",
                    str(root),
                    "-c",
                    "user.name=Catalog Test",
                    "-c",
                    "user.email=catalog-test@localhost",
                    "commit",
                    "--quiet",
                    "-m",
                    "fixture",
                ],
                check=True,
            )
            readiness_path = root / catalog.READINESS_PATH
            readiness_path.write_text(
                readiness_path.read_text(encoding="utf-8") + "\n# dirty\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(catalog.CatalogError, "source files are dirty"):
                catalog.build_report(root)

    def test_report_rejects_duplicate_use_case_id(self) -> None:
        report = copy.deepcopy(catalog.build_report(ROOT))
        report["use_cases"].append(copy.deepcopy(report["use_cases"][0]))
        with self.assertRaisesRegex(catalog.CatalogError, "duplicate use-case IDs"):
            catalog.validate_report(report)

    def test_report_rejects_duplicate_capability_id(self) -> None:
        report = copy.deepcopy(catalog.build_report(ROOT))
        report["capability_registry"].append(copy.deepcopy(report["capability_registry"][0]))
        with self.assertRaisesRegex(catalog.CatalogError, "duplicate capability IDs"):
            catalog.validate_report(report)

    def test_report_rejects_missing_capability_registry_entry(self) -> None:
        report = copy.deepcopy(catalog.build_report(ROOT))
        report["capability_registry"].pop()
        with self.assertRaisesRegex(catalog.CatalogError, "exactly match"):
            catalog.validate_report(report)

    def test_report_rejects_dangling_demanded_by_reference(self) -> None:
        report = copy.deepcopy(catalog.build_report(ROOT))
        report["capability_registry"][0]["demanded_by"] = ["not-a-use-case"]
        with self.assertRaisesRegex(catalog.CatalogError, "mapping is not exact"):
            catalog.validate_report(report)


if __name__ == "__main__":
    unittest.main()
