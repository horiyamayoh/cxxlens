#!/usr/bin/env python3
"""Properties and negative tests for the NG provider security contract."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_security_contract import (  # noqa: E402
    CERTIFICATIONS,
    NAMESPACES,
    PROFILE,
    SUPPORT,
    SecurityContractError,
    authorize_product_execution,
    discover,
    load_yaml,
    namespace_authority,
    report,
    sample_certificate_registry,
    sample_subject,
    sandbox,
    schema_validate,
    signature_subject,
    support_tuple,
    validate_all,
    validate_untrusted_input,
    verify_certificate,
)


class NgSecurityContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.profile = load_yaml(ROOT / PROFILE)
        cls.namespaces = load_yaml(ROOT / NAMESPACES)
        cls.certifications = load_yaml(ROOT / CERTIFICATIONS)
        cls.support = load_yaml(ROOT / SUPPORT)

    def test_exact_contract_vectors_and_registries(self) -> None:
        profile, results, counts = validate_all(ROOT)
        self.assertEqual(profile["maturity"], "accepted")
        self.assertEqual(len(results), 35)
        self.assertEqual(sum(row["decision"] == "accepted" for row in results), 10)
        self.assertEqual(counts["production_certificates"], 0)
        self.assertEqual(report(profile, results, counts)["status"], "green")

    def test_schema_conformance_never_grants_standard_authority(self) -> None:
        with self.assertRaisesRegex(SecurityContractError, "canonical-authority-denied"):
            namespace_authority(
                self.namespaces,
                kind="relation",
                name="cc.call_site",
                publisher="cxxlens.project",
                provider_id="clang22",
                qualifications=["schema-conformant"],
                certification_source="trusted-registry",
            )

    def test_manifest_self_certification_is_non_authoritative(self) -> None:
        with self.assertRaisesRegex(SecurityContractError, "certification-self-asserted"):
            namespace_authority(
                self.namespaces,
                kind="relation",
                name="core.unresolved",
                publisher="cxxlens.project",
                provider_id="builtin",
                qualifications=["canonical-semantic-qualified"],
                certification_source="manifest-self-assertion",
            )

    def test_provider_owned_namespace_binds_exact_identity(self) -> None:
        accepted = namespace_authority(
            self.namespaces,
            kind="relation",
            name="provider.acme.scanner.observation",
            publisher="acme",
            provider_id="scanner",
            qualifications=["schema-conformant"],
            certification_source="trusted-registry",
        )
        self.assertEqual(accepted["authority"], "provider-owned")
        with self.assertRaisesRegex(SecurityContractError, "namespace-collision"):
            namespace_authority(
                self.namespaces,
                kind="relation",
                name="provider.other.scanner.observation",
                publisher="acme",
                provider_id="scanner",
                qualifications=["schema-conformant"],
                certification_source="trusted-registry",
            )

    def test_certificate_signature_binds_all_subject_fields(self) -> None:
        left = sample_subject()
        right = copy.deepcopy(left)
        right["binary_digest"] = "sha256:" + "9" * 64
        self.assertNotEqual(signature_subject(left), signature_subject(right))

    def test_revocation_and_stale_certificate_fail_closed(self) -> None:
        registry, certificate, evidence = sample_certificate_registry(self.certifications)
        required = {"level": "canonical-semantic-qualified", "relation": "company.lock.acquire@1", "interpretation": "provider.cxxlens.conformance.company-lock@1", "toolchain": "fixture", "platform": "portable"}
        registry["revocations"] = [{"certificate_id": certificate["id"], "effective_sequence": 7, "reason": "compromised"}]
        with self.assertRaisesRegex(SecurityContractError, "certificate-revoked"):
            verify_certificate(registry, certificate=certificate, expected_subject=sample_subject(), signature_evidence=evidence, trusted_epoch=150, required=required)
        registry["revocations"] = []
        with self.assertRaisesRegex(SecurityContractError, "certificate-stale"):
            verify_certificate(registry, certificate=certificate, expected_subject=sample_subject(), signature_evidence=evidence, trusted_epoch=201, required=required)

    def test_conformance_anchor_cannot_grant_production(self) -> None:
        registry, certificate, evidence = sample_certificate_registry(self.certifications)
        required = {"level": "canonical-semantic-qualified", "relation": "company.lock.acquire@1", "interpretation": "provider.cxxlens.conformance.company-lock@1", "toolchain": "fixture", "platform": "portable"}
        with self.assertRaisesRegex(SecurityContractError, "certificate-untrusted-issuer"):
            verify_certificate(registry, certificate=certificate, expected_subject=sample_subject(), signature_evidence=evidence, trusted_epoch=150, required=required, production=True)

    def test_path_only_and_shadowing_are_structured_rejections(self) -> None:
        request = {"provider_id": "p", "provider_version": "1.0.0"}
        path = {"source": "path", "locator": "PATH:p", "provider_id": "p", "provider_version": "1.0.0", "package_identity": "pkg:p", "binary_digest": "a", "trust_valid": True, "rejection_code": "security.signature-mismatch"}
        with self.assertRaisesRegex(SecurityContractError, "path-only-discovery"):
            discover(request, [path])
        first = dict(path, source="explicit_path", locator="/a")
        second = dict(path, source="system_registry", locator="/b", binary_digest="b")
        with self.assertRaisesRegex(SecurityContractError, "provider-shadowing"):
            discover(request, [first, second])

    def test_invalid_explicit_candidate_blocks_valid_lower_candidate(self) -> None:
        request = {"provider_id": "p", "provider_version": "1.0.0"}
        base = {"provider_id": "p", "provider_version": "1.0.0", "package_identity": "pkg:p", "binary_digest": "digest", "rejection_code": "security.signature-mismatch"}
        candidates = [
            {**base, "source": "explicit_path", "locator": "/bad", "trust_valid": False},
            {**base, "source": "installation_manifest", "locator": "/good", "trust_valid": True},
        ]
        with self.assertRaisesRegex(SecurityContractError, "downgrade-forbidden"):
            discover(request, candidates)

    def test_discovery_order_is_not_input_order(self) -> None:
        request = {"provider_id": "p", "provider_version": "1.0.0"}
        base = {"provider_id": "p", "provider_version": "1.0.0", "package_identity": "pkg:p", "binary_digest": "digest", "trust_valid": True, "rejection_code": "security.signature-mismatch"}
        candidates = [
            {**base, "source": "system_registry", "locator": "/system"},
            {**base, "source": "explicit_path", "locator": "/explicit"},
        ]
        self.assertEqual(discover(request, candidates)["selected_source"], "explicit_path")
        self.assertEqual(discover(request, list(reversed(candidates)))["selected_source"], "explicit_path")

    def test_sandbox_uses_strongest_minimum(self) -> None:
        evidence = {"platform": "linux", "mechanism": "seccomp", "achieved": "best_effort", "policy_digest": "p", "evidence_digest": "e"}
        with self.assertRaisesRegex(SecurityContractError, "sandbox-insufficient"):
            sandbox("best_effort", "enforced", "none", evidence)
        evidence["achieved"] = "certified"
        self.assertEqual(sandbox("best_effort", "enforced", "none", evidence), "certified")

    def test_sandbox_assurance_is_a_closed_enum_at_every_boundary(self) -> None:
        evidence = {"platform": "linux", "mechanism": "seccomp", "achieved": "enforced", "policy_digest": "p", "evidence_digest": "e"}
        for invalid in ("4", "255"):
            with self.assertRaisesRegex(SecurityContractError, "sandbox-assurance-invalid"):
                sandbox("best_effort", invalid, "none", evidence)
            invalid_evidence = dict(evidence, achieved=invalid)
            with self.assertRaisesRegex(SecurityContractError, "sandbox-assurance-invalid"):
                sandbox("best_effort", "enforced", "none", invalid_evidence)

    def test_product_execution_needs_opt_in_and_complete_audit(self) -> None:
        fields = self.profile["product_execution"]["audit_fields"]
        audit = {field: "denied" if field == "network" else "value" for field in fields}
        with self.assertRaisesRegex(SecurityContractError, "opt-in-required"):
            authorize_product_execution({"explicit_opt_in": False, "audit": audit}, fields)
        audit.pop("process_tree_digest")
        with self.assertRaisesRegex(SecurityContractError, "audit-evidence-incomplete"):
            authorize_product_execution({"explicit_opt_in": True, "audit": audit}, fields)

    def test_untrusted_validation_requires_every_boundary_check(self) -> None:
        value = {name: True for name in ["size_limit", "schema_digest", "canonical_encoding", "reference_integrity", "trust_binding"]}
        validate_untrusted_input(value)
        value["reference_integrity"] = False
        with self.assertRaisesRegex(SecurityContractError, "untrusted-input-invalid"):
            validate_untrusted_input(value)

    def test_support_is_exact_full_tuple_and_never_infers_planned_production(self) -> None:
        fields = self.support["tuple_identity"]
        conformance = {field: self.support["entries"][2][field] for field in fields}
        self.assertEqual(support_tuple(self.support, conformance, "conformance-only"), "conformance-only")
        planned = {field: self.support["entries"][1][field] for field in fields}
        with self.assertRaisesRegex(SecurityContractError, "support-tuple-unsupported"):
            support_tuple(self.support, planned, "production-supported")

    def test_profile_schema_rejects_discovery_precedence_drift(self) -> None:
        changed = copy.deepcopy(self.profile)
        changed["discovery"]["precedence"].reverse()
        with self.assertRaisesRegex(SecurityContractError, "untrusted-input-invalid"):
            schema_validate(changed, load_yaml(ROOT / "schemas/cxxlens_ng_security_profile.schema.yaml"), "profile")


if __name__ == "__main__":
    unittest.main()
