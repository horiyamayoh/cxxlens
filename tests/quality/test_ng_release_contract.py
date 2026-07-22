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
    EXPECTED_PRODUCTION_SCOPE_NAMESPACES,
    ReleaseContractError,
    canonical_document_digest,
    decide,
    load_document,
    schema_validate,
    validate_boundary,
    validate_bundle,
    validate_package_qualification,
    validate_release_mapping,
    validate_version_contract,
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
    "snapshot-format",
    "relation-descriptor",
    "provider-implementation",
    "native-sdk",
    "model-assumption-pack",
}
DEFAULT_DIGEST = "sha256:" + "1" * 64
SNAPSHOT_CONTRACT_DIGEST = canonical_document_digest(
    load_document(ROOT / "schemas/cxxlens_ng_sqlite_store_contract.yaml")
)
SNAPSHOT_CURRENT_FEATURES = (
    "memory-readable-2.6.0-direct",
    "sqlite-readable-3.0.0-direct",
    "sqlite-v2.6.0-read-only-migration-required",
    "compact-v2.6.0-to-v3.0.0-explicit",
)
SNAPSHOT_V2_ARTIFACT_CONTEXT = "sqlite-store-artifact-v2.6.0-pre-migration"


def axis(
    name: str,
    version: str | None = None,
    *,
    required: tuple[str, ...] = (),
    optional: tuple[str, ...] = (),
    digest: str | None = None,
) -> dict:
    if version is None:
        version = "3.0.0" if name == "snapshot-format" else "1.0.0"
    if name == "snapshot-format" and not required and not optional:
        required = SNAPSHOT_CURRENT_FEATURES
    if name == "snapshot-format" and digest is None:
        digest = SNAPSHOT_CONTRACT_DIGEST
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

    def test_unknown_newer_snapshot_physical_minor_is_rejected(self) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["offered_axes"][0]["version"] = "3.1.0"
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.axis-version-mismatch", report["reason_codes"])

    def test_truthful_current_binary_with_v2_sqlite_artifact_requires_migration(
        self,
    ) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["offered_axes"][0]["features"].append(
            {"id": SNAPSHOT_V2_ARTIFACT_CONTEXT, "requirement": "required"}
        )
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "migration-required")
        snapshot = next(
            row for row in report["axis_results"] if row["axis"] == "snapshot-format"
        )
        self.assertEqual(snapshot["required_version"], "3.0.0")
        self.assertEqual(snapshot["offered_version"], "3.0.0")
        self.assertEqual(snapshot["missing_required_features"], [])
        self.assertEqual(
            {row["id"] for row in document["offered_axes"][0]["features"]},
            {*SNAPSHOT_CURRENT_FEATURES, SNAPSHOT_V2_ARTIFACT_CONTEXT},
        )
        self.assertEqual(
            report["migration_steps"][0]["migration_id"],
            "compact-v2.6.0-to-v3.0.0",
        )
        self.assertEqual(
            report["migration_steps"][0]["handler"], "snapshot-store-compact"
        )

    def test_v2_artifact_context_without_current_binary_capabilities_is_rejected(
        self,
    ) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["offered_axes"][0]["features"] = [
            {"id": SNAPSHOT_V2_ARTIFACT_CONTEXT, "requirement": "required"},
            {
                "id": "memory-readable-2.6.0-direct",
                "requirement": "required",
            },
            {
                "id": "compact-v2.6.0-to-v3.0.0-explicit",
                "requirement": "required",
            },
        ]
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.required-feature-missing", report["reason_codes"])
        self.assertEqual(report["migration_steps"], [])

    def test_v2_artifact_context_without_explicit_compact_capability_is_rejected(
        self,
    ) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["offered_axes"][0]["features"] = [
            {"id": feature, "requirement": "required"}
            for feature in SNAPSHOT_CURRENT_FEATURES
            if feature != "compact-v2.6.0-to-v3.0.0-explicit"
        ]
        document["offered_axes"][0]["features"].append(
            {"id": SNAPSHOT_V2_ARTIFACT_CONTEXT, "requirement": "required"}
        )
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.required-feature-missing", report["reason_codes"])
        self.assertEqual(report["migration_steps"], [])

    def test_physical_v2_version_cannot_substitute_for_release_axis_version(self) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["offered_axes"][0]["version"] = "2.6.0"
        document["offered_axes"][0]["features"].append(
            {"id": SNAPSHOT_V2_ARTIFACT_CONTEXT, "requirement": "required"}
        )
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.axis-major-mismatch", report["reason_codes"])
        self.assertEqual(report["migration_steps"], [])

    def test_memory_readable_format_cannot_substitute_for_release_axis_version(
        self,
    ) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["offered_axes"][0]["version"] = "2.6.0"
        document["offered_axes"][0]["features"] = [
            {
                "id": "memory-readable-2.6.0-direct",
                "requirement": "required",
            }
        ]
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.axis-major-mismatch", report["reason_codes"])
        self.assertEqual(report["migration_steps"], [])

    def test_backend_format_cannot_substitute_for_required_release_axis(self) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["required_axes"][0]["version"] = "2.6.0"
        document["offered_axes"][0]["version"] = "2.6.0"
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.axis-version-mismatch", report["reason_codes"])
        self.assertEqual(report["migration_steps"], [])

    def test_v2_artifact_context_requirement_must_be_required(self) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["offered_axes"][0]["features"].append(
            {"id": SNAPSHOT_V2_ARTIFACT_CONTEXT, "requirement": "optional"}
        )
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.request-invalid", report["reason_codes"])
        self.assertEqual(report["migration_steps"], [])

    def test_other_snapshot_axis_cross_major_tuples_are_rejected(self) -> None:
        for version in ("1.9.9", "2.5.0", "4.0.0"):
            with self.subTest(version=version):
                document = request(SNAPSHOT_AXES, context="snapshot-open")
                document["offered_axes"][0]["version"] = version
                document["offered_axes"][0]["features"].append(
                    {
                        "id": SNAPSHOT_V2_ARTIFACT_CONTEXT,
                        "requirement": "required",
                    }
                )
                report = decide(self.bundle, document, ROOT)
                self.assertEqual(report["decision"], "unsupported")
                self.assertIn(
                    "compat.axis-major-mismatch", report["reason_codes"]
                )
                self.assertEqual(report["migration_steps"], [])

    def test_snapshot_migration_registry_rejects_implicit_execution(self) -> None:
        bundle = copy.deepcopy(self.bundle)
        bundle["migrations"][0]["implicit"] = True
        with self.assertRaisesRegex(ReleaseContractError, "must be explicit"):
            validate_version_contract(bundle, ROOT)

    def test_snapshot_migration_registry_rejects_other_cross_major_path(self) -> None:
        bundle = copy.deepcopy(self.bundle)
        other = copy.deepcopy(bundle["migrations"][0])
        other.update(
            {
                "id": "unregistered-snapshot-cross-major",
                "from": "1.0.0",
                "to": "2.0.0",
            }
        )
        bundle["migrations"].append(other)
        with self.assertRaisesRegex(
            ReleaseContractError, "exact registered|must not cross"
        ):
            validate_version_contract(bundle, ROOT)

    def test_snapshot_migration_registry_is_scoped_to_sqlite_readable_format(
        self,
    ) -> None:
        bundle = copy.deepcopy(self.bundle)
        bundle["migrations"][0]["coordinate"] = "snapshot-format-axis"
        with self.assertRaisesRegex(
            ReleaseContractError, "exact registered|must not cross"
        ):
            validate_version_contract(bundle, ROOT)

    def test_snapshot_axis_requires_exact_sqlite_contract_digest(self) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["offered_axes"][0]["contract_digest"] = "sha256:" + "2" * 64
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.contract-digest-mismatch", report["reason_codes"])

    def test_current_snapshot_axis_cannot_omit_physical_capability_features(self) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open")
        document["required_axes"][0]["features"] = []
        document["offered_axes"][0]["features"] = []
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertIn("compat.required-feature-missing", report["reason_codes"])

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

    def test_doctor_runtime_evidence_cannot_override_intermediate_release_state(self) -> None:
        document = request(SNAPSHOT_AXES, context="snapshot-open", operation="doctor")
        document["environment"] = {
            "runtime_qualified": True,
            "evidence_refs": ["cxxlens-ng-release-qualification-exact-sha"],
        }
        report = decide(self.bundle, document, ROOT)
        self.assertEqual(report["decision"], "unsupported")
        self.assertEqual(report["qualification_state"], "qualification-in-progress")
        self.assertIn("compat.release-not-qualified", report["reason_codes"])
        self.assertEqual(report["environment_findings"][0]["severity"], "blocker")

    def test_production_scope_closure_binds_exact_thirty_namespaces(self) -> None:
        closure = self.bundle["production_scope_closure"]
        self.assertEqual(len(closure["namespace_ids"]), 30)
        self.assertEqual(closure["namespace_ids"], EXPECTED_PRODUCTION_SCOPE_NAMESPACES)
        self.assertEqual(closure["evaluation"]["ci_job"], "release-evaluation")
        self.assertFalse(closure["evaluation"]["not_qualified_satisfies_gate_release"])

    def test_production_scope_closure_cannot_omit_namespace(self) -> None:
        bundle = copy.deepcopy(self.bundle)
        bundle["production_scope_closure"]["namespace_ids"].pop()
        with self.assertRaisesRegex(ReleaseContractError, "scope closure binding differs"):
            validate_release_mapping(bundle, ROOT)

    def test_production_scope_closure_artifact_binding_is_exact(self) -> None:
        bundle = copy.deepcopy(self.bundle)
        bundle["production_scope_closure"]["evaluation"]["artifact"] = "stale-artifact"
        with self.assertRaisesRegex(ReleaseContractError, "scope closure binding differs"):
            validate_release_mapping(bundle, ROOT)

    def test_distribution_one_is_truthfully_not_qualified(self) -> None:
        release = next(
            row for row in self.bundle["releases"] if row["id"] == "distribution-1.0"
        )
        self.assertEqual(release["state"], "qualification-in-progress")
        self.assertFalse(release["production_supported"])

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
