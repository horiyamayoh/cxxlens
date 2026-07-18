#!/usr/bin/env python3
"""Positive and fail-closed tests for API development Wave 0 readiness."""

from __future__ import annotations

import copy
import pathlib
import shutil
import sys
import tempfile
import unittest
from unittest import mock

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_api_development_readiness import (  # noqa: E402
    ReadinessError,
    build_report,
    load_document,
    validate_documents,
)


REQUIRED_FILES = (
    ".github/workflows/quality.yml",
    "CMakeLists.txt",
    "docs/development/agent-api-development-goal.md",
    "schemas/cxxlens_ng_acceptance_manifest.yaml",
    "schemas/cxxlens_ng_api_development_readiness.schema.yaml",
    "schemas/cxxlens_ng_api_development_readiness.yaml",
    "schemas/cxxlens_ng_public_api_catalog.yaml",
    "schemas/cxxlens_ng_relation_registry.yaml",
    "schemas/cxxlens_ng_release_bundle.yaml",
    "tools/quality/check_ng_migration_completion.py",
)


class NgApiDevelopmentReadinessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = validate_documents(ROOT)

    def copied_root(self, temporary: str) -> pathlib.Path:
        root = pathlib.Path(temporary)
        for relative in REQUIRED_FILES:
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        shutil.copytree(ROOT / "include/cxxlens", root / "include/cxxlens")
        return root

    @staticmethod
    def write_yaml(path: pathlib.Path, document: dict) -> None:
        path.write_text(
            yaml.safe_dump(document, sort_keys=False, allow_unicode=True),
            encoding="utf-8",
        )

    def test_repository_is_wave0_ready(self) -> None:
        self.assertEqual(self.manifest["maturity"], "implemented")
        self.assertEqual(
            self.manifest["target_contract"]["provider_sdk_role"],
            "high-level-author-sdk",
        )

    def test_cmake_dependency_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            cmake = root / "CMakeLists.txt"
            cmake.write_text(
                cmake.read_text(encoding="utf-8").replace(
                    "target_link_libraries(cxxlens_provider_sdk PUBLIC cxxlens::cxxlens\n"
                    "                                                  cxxlens::recipes)",
                    "target_link_libraries(cxxlens_provider_sdk PUBLIC cxxlens::cxxlens)",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ReadinessError, "CMake target graph differs"):
                validate_documents(root)

    def test_catalog_unregistered_header_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            (root / "include/cxxlens/unregistered.hpp").write_text(
                "#pragma once\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(ReadinessError, "extra=.*unregistered"):
                validate_documents(root)

    def test_catalog_missing_header_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            (root / "include/cxxlens/cxxlens.hpp").unlink()
            with self.assertRaisesRegex(ReadinessError, "missing=.*cxxlens.hpp"):
                validate_documents(root)

    def test_catalog_relation_without_registry_binding_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            header = "include/cxxlens/relations/synthetic_unbound.hpp"
            (root / header).write_text("#pragma once\n", encoding="utf-8")
            catalog_path = root / "schemas/cxxlens_ng_public_api_catalog.yaml"
            catalog = load_document(catalog_path)
            catalog["packages"][0]["headers"].append(header)
            self.write_yaml(catalog_path, catalog)
            with self.assertRaisesRegex(ReadinessError, "lack registry binding"):
                validate_documents(root)

    def test_multiple_active_write_units_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            manifest_path = root / "schemas/cxxlens_ng_api_development_readiness.yaml"
            manifest = load_document(manifest_path)
            unit = {
                "issue": "#200",
                "contract_ids": ["synthetic.contract"],
                "write_paths": ["include/cxxlens/synthetic.hpp"],
                "completed_stages": [],
            }
            manifest["api_unit_workflow"]["active_write_units"] = [
                unit,
                {**copy.deepcopy(unit), "issue": "#201"},
            ]
            self.write_yaml(manifest_path, manifest)
            with self.assertRaisesRegex(ReadinessError, "schema validation failed"):
                validate_documents(root)

    def test_gate_owner_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            acceptance_path = root / "schemas/cxxlens_ng_acceptance_manifest.yaml"
            acceptance = load_document(acceptance_path)
            next(
                row for row in acceptance["entries"] if row["id"] == "gate.g5"
            )["owner_issue"] = "#999"
            self.write_yaml(acceptance_path, acceptance)
            with self.assertRaisesRegex(ReadinessError, "G5 gate owner differs"):
                validate_documents(root)

    def test_report_requires_exact_ci_job_set(self) -> None:
        git_state = {
            "revision": "1" * 40,
            "tree": "2" * 40,
            "branch": "main",
            "clean": True,
        }
        with tempfile.TemporaryDirectory() as temporary:
            with mock.patch(
                "check_ng_api_development_readiness.current_git_state",
                return_value=git_state,
            ):
                with self.assertRaisesRegex(ReadinessError, "CI job evidence differs"):
                    build_report(
                        ROOT,
                        self.manifest,
                        pathlib.Path(temporary),
                        "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
                        ["build-test (OFF)"],
                        "2026-07-18T00:00:00Z",
                        git_state["revision"],
                    )


if __name__ == "__main__":
    unittest.main()
