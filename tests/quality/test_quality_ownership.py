#!/usr/bin/env python3
"""Positive and fail-closed tests for quality evidence ownership."""

from __future__ import annotations

import copy
import pathlib
import shutil
import sys
import unittest
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/quality"))

from check_quality_ownership import (  # noqa: E402
    MANIFEST,
    QualityOwnershipError,
    canonical_digest,
    evidence_id,
    load_yaml,
    select_mode,
    validate_evidence,
    validate_manifest,
)
from collect_toolchain_provenance import pinned_actions  # noqa: E402
from install_artifact_manifest import (  # noqa: E402
    InstallArtifactError,
    build_manifest,
    verify_manifest,
)


def evidence(check: str) -> dict:
    configuration = "test"
    record = {
        "schema": "cxxlens.quality-evidence.v1",
        "logical_check_id": check,
        "check_version": 1,
        "configuration": configuration,
        "revision": "1" * 40,
        "tree": "2" * 40,
        "toolchain_digest": "sha256:" + "3" * 64,
        "configuration_digest": canonical_digest(configuration),
        "checker_digest": "sha256:" + "5" * 64,
        "input_digest": "sha256:" + "6" * 64,
        "output_digest": "sha256:" + "7" * 64,
        "result": "passed",
    }
    record["evidence_id"] = evidence_id(record)
    return record


class QualityOwnershipTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = load_yaml(ROOT / MANIFEST)

    def test_manifest_and_repository_wiring_are_valid(self) -> None:
        validate_manifest(ROOT, self.manifest)

    def test_exact_evidence_set_is_accepted(self) -> None:
        validate_evidence(
            [evidence("a"), evidence("b")], {("a", "test"), ("b", "test")}
        )

    def test_duplicate_evidence_is_rejected(self) -> None:
        row = evidence("a")
        with self.assertRaisesRegex(QualityOwnershipError, "duplicate logical evidence"):
            validate_evidence([row, copy.deepcopy(row)], {("a", "test")})

    def test_missing_required_evidence_is_rejected(self) -> None:
        with self.assertRaisesRegex(QualityOwnershipError, "instance set differs"):
            validate_evidence([evidence("a")], {("a", "test"), ("b", "test")})

    def test_each_binding_field_mutation_is_rejected(self) -> None:
        baseline = evidence("a")
        for field in (
            "revision",
            "tree",
            "toolchain_digest",
            "configuration_digest",
            "checker_digest",
            "input_digest",
            "output_digest",
        ):
            with self.subTest(field=field):
                changed = copy.deepcopy(baseline)
                changed[field] += "x"
                with self.assertRaisesRegex(QualityOwnershipError, "does not match"):
                    validate_evidence([changed], {("a", "test")})

    def test_configuration_mutation_is_rejected_independently(self) -> None:
        changed = evidence("a")
        changed["configuration"] = "other"
        changed["evidence_id"] = evidence_id(changed)
        with self.assertRaisesRegex(QualityOwnershipError, "configuration digest"):
            validate_evidence([changed], {("a", "other")})

    def test_one_check_can_require_distinct_configurations(self) -> None:
        static = evidence("a")
        static["configuration"] = "static"
        static["configuration_digest"] = canonical_digest("static")
        static["evidence_id"] = evidence_id(static)
        shared = copy.deepcopy(static)
        shared["configuration"] = "shared"
        shared["configuration_digest"] = canonical_digest("shared")
        shared["evidence_id"] = evidence_id(shared)
        validate_evidence(
            [static, shared], {("a", "static"), ("a", "shared")}
        )

    def test_selector_expands_authoritative_and_unknown_changes(self) -> None:
        for path in (
            "include/cxxlens/sdk.hpp",
            "schemas/new.yaml",
            "cmake/new.cmake",
            ".github/workflows/quality.yml",
            "tools/quality/check_quality_ownership.py",
            "unclassified.asset",
        ):
            with self.subTest(path=path):
                self.assertEqual(select_mode([path]), "full")
        self.assertEqual(select_mode(["src/sdk/common.cpp"]), "fast")
        self.assertEqual(select_mode(["docs/development/build-and-test.md"]), "check")
        self.assertEqual(select_mode(["src/sdk/common.cpp"], graph_available=False), "full")

    def test_mutable_workflow_action_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            workflows = root / ".github/workflows"
            workflows.mkdir(parents=True)
            (workflows / "quality.yml").write_text(
                "steps:\n  - uses: actions/checkout@v4\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(ValueError, "not pinned"):
                pinned_actions(root)

    def test_install_artifact_binding_and_file_swap_are_fail_closed(self) -> None:
        compiler = pathlib.Path(shutil.which("c++") or "")
        with tempfile.TemporaryDirectory() as temporary:
            prefix = pathlib.Path(temporary) / "prefix"
            prefix.mkdir()
            artifact = prefix / "artifact.txt"
            artifact.write_text("accepted\n", encoding="utf-8")
            baseline = build_manifest(ROOT, prefix, compiler, "static-test")
            verify_manifest(ROOT, prefix, compiler, "static-test", baseline)

            for mutate in (
                lambda row: row["source"].__setitem__("revision", "0" * 40),
                lambda row: row["source"].__setitem__("tree", "0" * 40),
                lambda row: row.__setitem__("configuration", "shared-test"),
                lambda row: row["toolchain"].__setitem__(
                    "binary_digest", "sha256:" + "0" * 64
                ),
            ):
                with self.subTest(mutation=mutate):
                    changed = copy.deepcopy(baseline)
                    mutate(changed)
                    with self.assertRaisesRegex(InstallArtifactError, "binding mismatch"):
                        verify_manifest(ROOT, prefix, compiler, "static-test", changed)

            artifact.write_text("substituted\n", encoding="utf-8")
            with self.assertRaisesRegex(InstallArtifactError, "binding mismatch"):
                verify_manifest(ROOT, prefix, compiler, "static-test", baseline)


if __name__ == "__main__":
    unittest.main()
