#!/usr/bin/env python3
"""Positive and fail-closed tests for the NG release compatibility contract."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_release_contract import (  # noqa: E402
    ReleaseContractError,
    decide,
    load_document,
    schema_validate,
    validate_boundary,
    validate_bundle,
    validate_package_qualification,
)


SNAPSHOT_AXES = [
    "snapshot-format",
    "relation-descriptor",
    "identity-contract",
    "condition-semantics",
]
PROVIDER_AXES = [
    "provider-protocol",
    "provider-implementation",
    "relation-descriptor",
    "identity-contract",
    "condition-semantics",
]
DIGEST_AXES = {
    "relation-descriptor",
    "provider-implementation",
    "native-sdk",
    "model-assumption-pack",
}
DEFAULT_DIGEST = "sha256:" + "1" * 64


def axis(
    name: str,
    version: str = "1.0.0",
    *,
    required: tuple[str, ...] = (),
    optional: tuple[str, ...] = (),
    digest: str | None = None,
) -> dict:
    return {
        "axis": name,
        "version": version,
        "features": [
            *({"id": feature, "requirement": "required"} for feature in required),
            *({"id": feature, "requirement": "optional"} for feature in optional),
        ],
        "contract_digest": DEFAULT_DIGEST if digest is None and name in DIGEST_AXES else digest,
    }


def request(
    axes: list[str],
    *,
    context: str,
    operation: str = "inspect",
) -> dict:
    document = {
        "schema": "cxxlens.ng-compatibility-request.v1",
        "request_id": "test.request",
        "operation": operation,
        "release_id": "distribution-1.0",
        "context": context,
        "required_axes": [axis(name) for name in axes],
        "offered_axes": [axis(name) for name in axes],
    }
    if operation == "doctor":
        document["environment"] = {
            "runtime_qualified": False,
            "evidence_refs": [],
        }
    return document


class NgReleaseContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.bundle = validate_bundle(ROOT)
        cls.request_schema = load_document(
            ROOT / "schemas/cxxlens_ng_compatibility_request.schema.yaml"
        )

    def test_exact_snapshot_tuple_is_supported_for_contract_inspection(self) -> None:
        report = decide(
            self.bundle,
            request(SNAPSHOT_AXES, context="snapshot-open"),
            ROOT,
        )
        self.assertEqual(report["decision"], "supported")
        self.assertEqual(report["reason_codes"], ["compat.exact"])
        self.assertFalse(report["fallback_used"])

    def test_newer_same_major_is_supported(self) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["offered_axes"][0]["version"] = "1.2.0"
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "supported")
        self.assertIn("compat.same-major", report["reason_codes"])

    def test_explicit_same_major_migration_is_reported(self) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["required_axes"][0]["version"] = "1.1.0"
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "migration-required")
        self.assertEqual(
            report["migration_steps"][0]["migration_id"],
            "snapshot-format-1.0-to-1.1",
        )

    def test_major_mismatch_is_unsupported_without_fallback(self) -> None:
        document = request(PROVIDER_AXES, context="provider-handshake")
        document["offered_axes"][0]["version"] = "2.0.0"
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.axis-major-mismatch", report["reason_codes"])
        self.assertEqual(report["selected_bundle"], "distribution-1.0")
        self.assertFalse(report["fallback_used"])

    def test_unknown_required_feature_is_unsupported(self) -> None:
        document = request(PROVIDER_AXES, context="provider-handshake")
        document["required_axes"][0]["features"] = [
            {"id": "future-required-feature", "requirement": "required"}
        ]
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.required-feature-missing", report["reason_codes"])

    def test_required_descriptor_digest_mismatch_is_unsupported(self) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        descriptor = next(
            row for row in document["offered_axes"] if row["axis"] == "relation-descriptor"
        )
        descriptor["contract_digest"] = "sha256:" + "2" * 64
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.contract-digest-mismatch", report["reason_codes"])

    def test_unknown_optional_feature_is_preserved_as_unavailable(self) -> None:
        document = request(PROVIDER_AXES, context="provider-handshake")
        document["required_axes"][0]["features"] = [
            {"id": "future-optional-feature", "requirement": "optional"}
        ]
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "supported")
        self.assertIn("compat.optional-feature-unavailable", report["reason_codes"])

    def test_missing_axis_is_structured_unsupported(self) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["offered_axes"].pop()
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.axis-missing", report["reason_codes"])

    def test_duplicate_axis_is_rejected_without_first_wins(self) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["offered_axes"].append(copy.deepcopy(document["offered_axes"][0]))
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.axis-duplicate", report["reason_codes"])
        snapshot = next(row for row in report["axis_results"] if row["axis"] == "snapshot-format")
        self.assertIsNone(snapshot["offered_version"])

    def test_unknown_release_is_rejected_without_implicit_selection(self) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["release_id"] = "distribution-9.9"
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIsNone(report["selected_bundle"])
        self.assertEqual(report["reason_codes"], ["compat.release-unknown"])

    def test_doctor_reports_missing_runtime_qualification(self) -> None:
        report = decide(
            self.bundle,
            request(SNAPSHOT_AXES, context="snapshot-open", operation="doctor"),
            ROOT,
        )
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.release-not-qualified", report["reason_codes"])
        self.assertEqual(report["environment_findings"][0]["severity"], "blocker")

    def test_doctor_accepts_commit_bound_runtime_qualification(self) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open", operation="doctor")
        document["environment"] = {
            "runtime_qualified": True,
            "evidence_refs": ["cxxlens-ng-release-qualification-exact-sha"],
        }
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "supported")
        self.assertEqual(report["environment_findings"], [])

    def test_language_neutral_kernel_cannot_depend_on_cxx_semantics(self) -> None:
        bundle = copy.deepcopy(self.bundle)
        kernel = next(
            row
            for row in bundle["product_boundary"]["components"]
            if row["id"] == "relation-kernel"
        )
        kernel["depends_on"] = ["cc-cpp-semantics"]
        with self.assertRaisesRegex(ReleaseContractError, "dependency cycle|forbidden"):
            validate_boundary(bundle)

    def test_language_neutral_kernel_cannot_export_native_value(self) -> None:
        bundle = copy.deepcopy(self.bundle)
        kernel = next(
            row
            for row in bundle["product_boundary"]["components"]
            if row["id"] == "relation-kernel"
        )
        kernel["exported_values"].append("clang-ast-node")
        with self.assertRaisesRegex(ReleaseContractError, "exports native"):
            validate_boundary(bundle)

    def test_package_qualification_cannot_omit_shared_configuration(self) -> None:
        bundle = copy.deepcopy(self.bundle)
        bundle["distribution_surface"]["package_qualification"]["configurations"].pop()
        with self.assertRaisesRegex(ReleaseContractError, "static/shared matrix"):
            validate_package_qualification(bundle, ROOT)

    def test_doctor_request_without_environment_is_schema_rejected(self) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["operation"] = "doctor"
        with self.assertRaisesRegex(ReleaseContractError, "schema validation"):
            schema_validate(document, self.request_schema, "compatibility request")


if __name__ == "__main__":
    unittest.main()
