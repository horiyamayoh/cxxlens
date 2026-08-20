#!/usr/bin/env python3
"""Fail-closed tests for autonomous work-unit selection."""

from __future__ import annotations

import hashlib
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_work_units import MANIFEST, SCHEMA, WorkUnitError, validate  # noqa: E402


class WorkUnitTest(unittest.TestCase):
    def copied_root(self, temporary: str) -> pathlib.Path:
        root = pathlib.Path(temporary)
        manifest = yaml.safe_load((ROOT / MANIFEST).read_text(encoding="utf-8"))
        paths = {MANIFEST, SCHEMA}
        for entry in manifest["entries"]:
            paths.update(pathlib.Path(value) for value in entry["authority_sources"])
        for relative in paths:
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        return root

    @staticmethod
    def rewrite(root: pathlib.Path, mutate) -> None:
        path = root / MANIFEST
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
        mutate(value)
        path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")

    def test_repository_inventory_is_valid(self) -> None:
        validate(ROOT)

    def test_unknown_dependency_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["entries"][0]["units"][0]["depends_on"].append("wu-999-missing"))
            with self.assertRaisesRegex(WorkUnitError, "unknown dependency"):
                validate(root)

    def test_undeclared_owned_overlap_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["entries"][1]["units"][0]["owned_paths"].append("tests/unit/sdk/provider_runtime_test.cpp"))
            with self.assertRaisesRegex(WorkUnitError, "undeclared owned-path overlap"):
                validate(root)

    def test_authority_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            authority = root / "AGENTS.md"
            authority.write_text(authority.read_text(encoding="utf-8") + "drift\n", encoding="utf-8")
            with self.assertRaisesRegex(WorkUnitError, "authority digest drift"):
                validate(root)

    def test_generated_output_inside_unit_scope_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            def mutate(value) -> None:
                unit = value["entries"][0]["units"][0]
                unit["generated_surfaces"].append(unit["owned_paths"][0])
            self.rewrite(root, mutate)
            with self.assertRaisesRegex(WorkUnitError, "integration-owned"):
                validate(root)

    def test_asymmetric_serialization_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["entries"][2]["units"][0]["serialized_with"].pop())
            with self.assertRaisesRegex(WorkUnitError, "asymmetric serialization"):
                validate(root)

    def test_sqlite_product_must_have_one_owner(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            def mutate(value) -> None:
                value["entries"][3]["units"][0]["owned_products"].append(
                    "sqlite.active-read-connection"
                )
            self.rewrite(root, mutate)
            with self.assertRaisesRegex(WorkUnitError, "duplicate product owner"):
                validate(root)

    def test_consumed_product_requires_transitive_dependency(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            def mutate(value) -> None:
                for entry in value["entries"]:
                    for unit in entry["units"]:
                        if unit["id"] == "wu-202-normalization-effect":
                            unit["depends_on"] = ["wu-173-governance"]
            self.rewrite(root, mutate)
            with self.assertRaisesRegex(WorkUnitError, "consumed product lacks dependency"):
                validate(root)

    def test_available_product_receipt_authenticates_exact_git_blobs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            artifact_path = "docs/development/work-unit-products/sqlite-active-read-connection.json"
            evidence_path = "docs/development/work-unit-evidence/sqlite-active-read-connection.json"
            for path, payload in ((artifact_path, '{"product":"active-read"}\n'), (evidence_path, '{"evidence":"focused-pass"}\n')):
                destination = root / path
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(payload, encoding="utf-8")
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            subprocess.run(["git", "-C", str(root), "config", "user.name", "receipt-test"], check=True)
            subprocess.run(["git", "-C", str(root), "config", "user.email", "receipt@test.invalid"], check=True)
            subprocess.run(["git", "-C", str(root), "add", "."], check=True)
            subprocess.run(["git", "-C", str(root), "commit", "-qm", "producer"], check=True)
            commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
            tree = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD^{tree}"], text=True).strip()
            def committed_digest(path: str) -> str:
                payload = subprocess.check_output(["git", "-C", str(root), "show", f"{commit}:{path}"])
                return "sha256:" + hashlib.sha256(payload).hexdigest()

            def make_available(value) -> None:
                value["product_receipts"]["sqlite.active-read-connection"] = {
                    "contract": "cxxlens.sqlite-active-read-connection.v1",
                    "receipt_profile": "exact-producer-commit-tree-artifact-and-evidence-digests",
                    "status": "available",
                    "producer_unit": "wu-201-zero-effect-active-read",
                    "producer_commit": commit,
                    "producer_tree": tree,
                    "artifact_path": artifact_path,
                    "artifact_digest": committed_digest(artifact_path),
                    "evidence_path": evidence_path,
                    "evidence_digest": committed_digest(evidence_path),
                }

            self.rewrite(root, make_available)
            validate(root)
            self.rewrite(
                root,
                lambda value: value["product_receipts"]["sqlite.active-read-connection"].update(
                    {"producer_commit": tree}
                ),
            )
            with self.assertRaisesRegex(WorkUnitError, "not a commit"):
                validate(root)
            self.rewrite(root, make_available)
            self.rewrite(
                root,
                lambda value: value["product_receipts"]["sqlite.active-read-connection"].update(
                    {"producer_unit": "wu-205-nested-mapping-lease"}
                ),
            )
            with self.assertRaisesRegex(WorkUnitError, "producer mismatch"):
                validate(root)
            self.rewrite(root, make_available)
            self.rewrite(
                root,
                lambda value: value["product_receipts"]["sqlite.active-read-connection"].update(
                    {"producer_tree": "0" * 40}
                ),
            )
            with self.assertRaisesRegex(WorkUnitError, "tree mismatch"):
                validate(root)
            self.rewrite(root, make_available)
            self.rewrite(
                root,
                lambda value: value["product_receipts"]["sqlite.active-read-connection"].update(
                    {"artifact_path": "AGENTS.md", "artifact_digest": committed_digest("AGENTS.md")}
                ),
            )
            with self.assertRaisesRegex(WorkUnitError, "surface mismatch"):
                validate(root)
            self.rewrite(root, make_available)
            self.rewrite(
                root,
                lambda value: value["product_receipts"]["sqlite.active-read-connection"].update(
                    {"evidence_digest": "sha256:" + "0" * 64}
                ),
            )
            with self.assertRaisesRegex(WorkUnitError, "evidence digest mismatch"):
                validate(root)


if __name__ == "__main__":
    unittest.main()
