#!/usr/bin/env python3
"""Executable provider trust, namespace, discovery, and sandbox contract."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import subprocess
import sys
from typing import Any, Callable

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
PROFILE = pathlib.Path("schemas/cxxlens_ng_security_profile.yaml")
PROFILE_SCHEMA = pathlib.Path("schemas/cxxlens_ng_security_profile.schema.yaml")
NAMESPACES = pathlib.Path("schemas/cxxlens_ng_namespace_registry.yaml")
NAMESPACES_SCHEMA = pathlib.Path("schemas/cxxlens_ng_namespace_registry.schema.yaml")
CERTIFICATIONS = pathlib.Path("schemas/cxxlens_ng_provider_certification_registry.yaml")
CERTIFICATIONS_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_provider_certification_registry.schema.yaml"
)
SUPPORT = pathlib.Path("schemas/cxxlens_ng_provider_support_matrix.yaml")
SUPPORT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_provider_support_matrix.schema.yaml")
VECTORS = pathlib.Path("schemas/cxxlens_ng_security_conformance_vectors.yaml")
VECTORS_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_security_conformance_vectors.schema.yaml"
)
REPORT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_security_conformance_report.schema.yaml")

SUBJECT_FIELDS = (
    "provider_id",
    "provider_version",
    "package_identity",
    "publisher",
    "manifest_digest",
    "binary_digest",
    "semantic_contract_digest",
)
ASSURANCE = {"none": 0, "best_effort": 1, "enforced": 2, "certified": 3}
DISCOVERY = {
    "explicit_path": 0,
    "installation_manifest": 1,
    "project_config": 2,
    "system_registry": 3,
}


class SecurityContractError(ValueError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code


def fail(code: str, message: str) -> None:
    raise SecurityContractError(code, message)


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail("security.untrusted-input-invalid", str(path))
    return value


def schema_validate(value: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(value)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail("security.untrusted-input-invalid", f"{label}: {error.message}")


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")


def digest(value: Any, domain: str | None = None) -> str:
    payload = canonical_json(value)
    if domain is not None:
        payload = domain.encode("utf-8") + b"\0" + payload
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def namespace_authority(
    registry: dict[str, Any],
    *,
    kind: str,
    name: str,
    publisher: str,
    provider_id: str,
    qualifications: list[str],
    certification_source: str,
) -> dict[str, Any]:
    candidates = [
        row
        for row in registry["entries"]
        if row["kind"] == kind and name.startswith(row["prefix"])
    ]
    if not candidates:
        if any(
            row["kind"] == kind and name.startswith(row["prefix"])
            for row in registry["delegated_roots"]
        ):
            fail("security.namespace-unowned", f"delegated child not registered: {name}")
        fail("security.namespace-unowned", name)
    maximum = max(len(row["prefix"]) for row in candidates)
    exact = [row for row in candidates if len(row["prefix"]) == maximum]
    if len(exact) != 1:
        fail("security.namespace-collision", name)
    owner = exact[0]
    if certification_source == "manifest-self-assertion":
        fail("security.certification-self-asserted", name)
    if owner["grant_source"] == "derived-provider-identity":
        required = f"provider.{publisher}.{provider_id}."
        if not name.startswith(required):
            fail("security.namespace-collision", f"expected {required}")
    elif owner["owner"] != publisher:
        fail("security.namespace-collision", f"owner {owner['owner']} != {publisher}")
    minimum = owner["minimum_qualification"]
    if minimum not in qualifications:
        code = (
            "security.canonical-authority-denied"
            if minimum == "canonical-semantic-qualified"
            else "security.namespace-unowned"
        )
        fail(code, f"missing {minimum}")
    if owner["authority"] == "standard" and certification_source != "trusted-registry":
        fail("security.canonical-authority-denied", "standard grant is registry-only")
    return {"namespace": owner["id"], "authority": owner["authority"]}


def signature_subject(subject: dict[str, Any]) -> str:
    if set(subject) != set(SUBJECT_FIELDS):
        fail("security.certificate-subject-mismatch", "subject field set")
    ordered = [[field, subject[field]] for field in SUBJECT_FIELDS]
    return digest(ordered, "cxxlens-provider-signature-subject-v1")


def verify_certificate(
    registry: dict[str, Any],
    *,
    certificate: dict[str, Any],
    expected_subject: dict[str, Any],
    signature_evidence: dict[str, Any] | None,
    trusted_epoch: int,
    required: dict[str, str],
    production: bool = False,
) -> dict[str, Any]:
    issuers = {row["id"]: row for row in registry["issuers"]}
    anchors = {row["id"]: row for row in registry["trust_anchors"]}
    issuer = issuers.get(certificate["issuer"])
    if issuer is None or issuer["trust_anchor"] not in anchors:
        fail("security.certificate-untrusted-issuer", certificate["issuer"])
    anchor = anchors[issuer["trust_anchor"]]
    if production and (
        issuer["scope"] != "production" or anchor["production_use"] != "allowed"
    ):
        fail("security.certificate-untrusted-issuer", "non-production trust anchor")
    if any(
        row["certificate_id"] == certificate["id"]
        and row["effective_sequence"] <= certificate["registry_sequence"]
        for row in registry["revocations"]
    ):
        fail("security.certificate-revoked", certificate["id"])
    validity = certificate["validity"]
    if not validity["not_before_epoch"] <= trusted_epoch <= validity["not_after_epoch"]:
        fail("security.certificate-stale", certificate["id"])
    if certificate["subject"] != expected_subject:
        fail("security.certificate-subject-mismatch", certificate["id"])
    if signature_evidence is None:
        fail("security.signature-missing", certificate["id"])
    expected_digest = signature_subject(expected_subject)
    if (
        signature_evidence.get("algorithm") != "ed25519"
        or signature_evidence.get("verdict") != "verified"
        or signature_evidence.get("key_fingerprint")
        != issuer["public_key_fingerprint"]
        or signature_evidence.get("signed_subject_digest") != expected_digest
        or signature_evidence.get("signature_digest")
        != certificate["certificate_signature"]
    ):
        fail("security.signature-mismatch", certificate["id"])
    matches = [
        row
        for row in certificate["qualifications"]
        if row["level"] == required["level"]
        and row["relation"] == required["relation"]
        and row["interpretation"] == required["interpretation"]
        and required["toolchain"] in row["toolchains"]
        and required["platform"] in row["platforms"]
    ]
    if not matches or required["level"] not in issuer["allowed_qualifications"]:
        fail("security.canonical-authority-denied", "qualification tuple")
    return {"certificate_id": certificate["id"], "subject_digest": expected_digest}


def discover(
    request: dict[str, Any], candidates: list[dict[str, Any]]
) -> dict[str, Any]:
    if any(row["source"] == "path" for row in candidates):
        fail("security.path-only-discovery", "PATH is not an authority source")
    matching = [
        row
        for row in candidates
        if row["provider_id"] == request["provider_id"]
        and row["provider_version"] == request["provider_version"]
    ]
    if not matching:
        fail("security.discovery-empty", request["provider_id"])
    packages = {(row["package_identity"], row["binary_digest"]) for row in matching}
    if len(packages) > 1:
        fail("security.provider-shadowing", request["provider_id"])
    for source in DISCOVERY:
        rows = [row for row in matching if row["source"] == source]
        identities = {
            (
                row["provider_id"],
                row["provider_version"],
                row["package_identity"],
                row["binary_digest"],
            )
            for row in rows
        }
        if len(rows) > 1 and len(identities) == 1:
            fail("security.duplicate-provider", f"same precedence {source}")
    ordered = sorted(matching, key=lambda row: (DISCOVERY[row["source"]], row["locator"]))
    selected = ordered[0]
    if not selected["trust_valid"]:
        if any(row["trust_valid"] for row in ordered[1:]):
            fail("security.downgrade-forbidden", selected["source"])
        fail(selected["rejection_code"], selected["locator"])
    return {
        "selected_source": selected["source"],
        "selected_locator": selected["locator"],
        "rejections": [
            {"locator": row["locator"], "reason": "security.lower-precedence"}
            for row in ordered[1:]
        ],
    }


def sandbox(
    manifest_minimum: str,
    request_minimum: str,
    profile_minimum: str,
    evidence: dict[str, Any],
) -> str:
    for boundary, assurance in (
        ("manifest minimum", manifest_minimum),
        ("request minimum", request_minimum),
        ("profile minimum", profile_minimum),
    ):
        if assurance not in ASSURANCE:
            fail("security.sandbox-assurance-invalid", boundary)
    required = max(
        (manifest_minimum, request_minimum, profile_minimum), key=ASSURANCE.__getitem__
    )
    required_fields = {
        "platform",
        "mechanism",
        "achieved",
        "policy_digest",
        "evidence_digest",
    }
    if set(evidence) != required_fields:
        fail("security.sandbox-insufficient", "missing achieved assurance evidence")
    if evidence["achieved"] not in ASSURANCE:
        fail("security.sandbox-assurance-invalid", "achieved")
    if ASSURANCE[evidence["achieved"]] < ASSURANCE[required]:
        fail("security.sandbox-insufficient", f"{evidence['achieved']} < {required}")
    return evidence["achieved"]


def authorize_product_execution(value: dict[str, Any], audit_fields: list[str]) -> None:
    if not value.get("explicit_opt_in", False):
        fail("security.opt-in-required", "product execution")
    audit = value.get("audit", {})
    if set(audit) != set(audit_fields) or audit.get("network") != "denied":
        fail("security.audit-evidence-incomplete", "product execution")


def validate_untrusted_input(value: dict[str, Any]) -> None:
    checks = {
        "size_limit",
        "schema_digest",
        "canonical_encoding",
        "reference_integrity",
        "trust_binding",
    }
    if set(value) != checks or not all(value.values()):
        fail("security.untrusted-input-invalid", "validation boundary")


def support_tuple(
    matrix: dict[str, Any], request: dict[str, Any], required_status: str
) -> str:
    fields = matrix["tuple_identity"]
    rows = [row for row in matrix["entries"] if all(row[field] == request[field] for field in fields)]
    if len(rows) != 1:
        fail("security.support-tuple-unsupported", "missing or ambiguous tuple")
    status = rows[0]["status"]
    if required_status == "production-supported" and status != required_status:
        fail("security.support-tuple-unsupported", status)
    if status in {"planned", "unsupported"}:
        fail("security.support-tuple-unsupported", status)
    return status


def sample_subject() -> dict[str, str]:
    return {
        "provider_id": "cxxlens.conformance.company-lock",
        "provider_version": "1.0.0",
        "package_identity": "pkg:cxxlens/conformance-company-lock@1.0.0",
        "publisher": "cxxlens.conformance",
        "manifest_digest": "sha256:" + "1" * 64,
        "binary_digest": "sha256:" + "2" * 64,
        "semantic_contract_digest": "sha256:" + "3" * 64,
    }


def sample_certificate_registry(base: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    registry = copy.deepcopy(base)
    subject = sample_subject()
    certificate = {
        "id": "certificate.company-lock.v1",
        "issuer": "cxxlens.conformance-issuer.v1",
        "serial": "serial-1",
        "subject": subject,
        "qualifications": [
            {
                "level": "canonical-semantic-qualified",
                "relation": "company.lock.acquire@1",
                "interpretation": "provider.cxxlens.conformance.company-lock@1",
                "toolchains": ["fixture"],
                "platforms": ["portable"],
            }
        ],
        "validity": {"not_before_epoch": 100, "not_after_epoch": 200},
        "registry_sequence": 7,
        "certificate_signature": "sha256:" + "4" * 64,
    }
    registry["certificates"] = [certificate]
    issuer = registry["issuers"][0]
    evidence = {
        "algorithm": "ed25519",
        "verdict": "verified",
        "verifier_id": "trusted.verifier.v1",
        "key_fingerprint": issuer["public_key_fingerprint"],
        "signed_subject_digest": signature_subject(subject),
        "signature_digest": certificate["certificate_signature"],
    }
    return registry, certificate, evidence


def _candidate(source: str, locator: str, *, valid: bool = True) -> dict[str, Any]:
    return {
        "source": source,
        "locator": locator,
        "provider_id": "cxxlens.conformance.company-lock",
        "provider_version": "1.0.0",
        "package_identity": "pkg:cxxlens/conformance-company-lock@1.0.0",
        "binary_digest": "sha256:" + "2" * 64,
        "trust_valid": valid,
        "rejection_code": "security.signature-mismatch",
    }


def _namespace_scenario(registry: dict[str, Any], scenario: str) -> Any:
    base = dict(kind="relation", publisher="cxxlens.project", provider_id="builtin", qualifications=["canonical-semantic-qualified"], certification_source="trusted-registry")
    if scenario == "standard-qualified":
        return namespace_authority(registry, name="cc.call_site", **base)
    if scenario == "standard-schema-only":
        return namespace_authority(registry, name="cc.call_site", **{**base, "qualifications": ["schema-conformant"]})
    if scenario == "self-asserted-canonical":
        return namespace_authority(registry, name="cc.call_site", **{**base, "certification_source": "manifest-self-assertion"})
    if scenario == "unknown-org":
        return namespace_authority(registry, name="org.acme.call", **{**base, "publisher": "org.acme"})
    owned = dict(kind="relation", publisher="acme", provider_id="scanner", qualifications=["schema-conformant"], certification_source="trusted-registry")
    if scenario == "provider-owned-exact":
        return namespace_authority(registry, name="provider.acme.scanner.observation", **owned)
    return namespace_authority(registry, name="provider.other.scanner.observation", **owned)


def _certificate_scenario(base: dict[str, Any], scenario: str) -> Any:
    registry, certificate, evidence = sample_certificate_registry(base)
    subject = sample_subject()
    required = {"level": "canonical-semantic-qualified", "relation": "company.lock.acquire@1", "interpretation": "provider.cxxlens.conformance.company-lock@1", "toolchain": "fixture", "platform": "portable"}
    if scenario == "signature-missing":
        evidence = None
    elif scenario == "signature-mismatch":
        evidence["signed_subject_digest"] = "sha256:" + "0" * 64
    elif scenario == "untrusted-issuer":
        certificate["issuer"] = "unknown"
    elif scenario == "revoked":
        registry["revocations"] = [{"certificate_id": certificate["id"], "effective_sequence": 7, "reason": "test"}]
    elif scenario == "expired":
        certificate["validity"]["not_after_epoch"] = 149
    elif scenario == "subject-mismatch":
        subject["binary_digest"] = "sha256:" + "9" * 64
    return verify_certificate(registry, certificate=certificate, expected_subject=subject, signature_evidence=evidence, trusted_epoch=150, required=required)


def _discovery_scenario(scenario: str) -> Any:
    request = {"provider_id": "cxxlens.conformance.company-lock", "provider_version": "1.0.0"}
    if scenario == "empty":
        candidates: list[dict[str, Any]] = []
    elif scenario == "path-only":
        candidates = [_candidate("path", "PATH:company-lock")]
    elif scenario == "explicit-selected":
        candidates = [_candidate("installation_manifest", "/install/provider.json"), _candidate("explicit_path", "/explicit/provider")]
    elif scenario == "installation-selected":
        candidates = [_candidate("installation_manifest", "/install/provider.json"), _candidate("system_registry", "/registry/provider.json")]
    elif scenario == "shadowing":
        candidates = [_candidate("explicit_path", "/explicit/provider"), _candidate("installation_manifest", "/install/provider.json")]
        candidates[1]["binary_digest"] = "sha256:" + "8" * 64
    elif scenario == "same-precedence-duplicate":
        candidates = [_candidate("project_config", "/project/a"), _candidate("project_config", "/project/b")]
    else:
        candidates = [_candidate("explicit_path", "/explicit/provider", valid=False), _candidate("installation_manifest", "/install/provider.json")]
    return discover(request, candidates)


def _sandbox_scenario(scenario: str) -> str:
    required = "enforced"
    achieved = {"exact": "enforced", "higher": "certified", "insufficient": "best_effort"}.get(scenario, "enforced")
    if scenario.startswith("required-"):
        required = scenario.removeprefix("required-")
    elif scenario.startswith("achieved-"):
        achieved = scenario.removeprefix("achieved-")
    evidence = {"platform": "linux", "mechanism": "namespaces-seccomp", "achieved": achieved, "policy_digest": "sha256:" + "5" * 64, "evidence_digest": "sha256:" + "6" * 64}
    return sandbox("best_effort", required, "enforced", evidence)


def _product_scenario(profile: dict[str, Any], scenario: str) -> None:
    audit = {field: "denied" if field == "network" else "value" for field in profile["product_execution"]["audit_fields"]}
    value = {"explicit_opt_in": scenario != "no-opt-in", "audit": audit}
    if scenario == "audit-missing":
        value["audit"].pop("process_tree_digest")
    authorize_product_execution(value, profile["product_execution"]["audit_fields"])


def _validation_scenario(scenario: str) -> None:
    value = {name: True for name in ["size_limit", "schema_digest", "canonical_encoding", "reference_integrity", "trust_binding"]}
    if scenario == "schema-mismatch":
        value["schema_digest"] = False
    validate_untrusted_input(value)


def _support_scenario(matrix: dict[str, Any], scenario: str) -> str:
    if scenario == "conformance-exact":
        row = matrix["entries"][2]
        required = "conformance-only"
    elif scenario == "planned-as-production":
        row = matrix["entries"][1]
        required = "production-supported"
    else:
        row = dict(matrix["entries"][2], provider_id="unknown")
        required = "conformance-only"
    request = {field: row[field] for field in matrix["tuple_identity"]}
    return support_tuple(matrix, request, required)


def execute_vector(
    vector: dict[str, Any],
    profile: dict[str, Any],
    namespaces: dict[str, Any],
    certifications: dict[str, Any],
    support: dict[str, Any],
) -> dict[str, Any]:
    operation = vector["operation"]
    scenario = vector["input"]["scenario"]
    runners: dict[str, Callable[[], Any]] = {
        "namespace": lambda: _namespace_scenario(namespaces, scenario),
        "certificate": lambda: _certificate_scenario(certifications, scenario),
        "discovery": lambda: _discovery_scenario(scenario),
        "sandbox": lambda: _sandbox_scenario(scenario),
        "product_execution": lambda: _product_scenario(profile, scenario),
        "validation": lambda: _validation_scenario(scenario),
        "support": lambda: _support_scenario(support, scenario),
    }
    accepted_codes = {
        "namespace": "security.namespace-authorized",
        "certificate": "security.certificate-valid",
        "discovery": "security.provider-selected",
        "sandbox": "security.sandbox-qualified",
        "product_execution": "security.product-execution-authorized",
        "validation": "security.untrusted-input-valid",
        "support": "security.support-tuple-qualified",
    }
    try:
        value = runners[operation]()
        result = {"decision": "accepted", "reason_code": accepted_codes[operation]}
        if operation == "discovery":
            result["value"] = value["selected_source"]
        elif operation in {"sandbox", "support"}:
            result["value"] = value
    except SecurityContractError as error:
        result = {"decision": "rejected", "reason_code": error.code}
    if result != vector["expected"]:
        fail("security.untrusted-input-invalid", f"vector {vector['id']}: {result} != {vector['expected']}")
    return result


def validate_all(root: pathlib.Path) -> tuple[dict[str, Any], list[dict[str, Any]], dict[str, int]]:
    profile = load_yaml(root / PROFILE)
    namespaces = load_yaml(root / NAMESPACES)
    certifications = load_yaml(root / CERTIFICATIONS)
    support = load_yaml(root / SUPPORT)
    vectors = load_yaml(root / VECTORS)
    documents = [
        (profile, PROFILE_SCHEMA, "security profile"),
        (namespaces, NAMESPACES_SCHEMA, "namespace registry"),
        (certifications, CERTIFICATIONS_SCHEMA, "certification registry"),
        (support, SUPPORT_SCHEMA, "support matrix"),
        (vectors, VECTORS_SCHEMA, "security vectors"),
    ]
    for value, schema_path, label in documents:
        schema_validate(value, load_yaml(root / schema_path), label)
    if profile["sandbox"]["executable_binding"] != {
        "resolve": "working-directory-aware-single-open",
        "measure": "exact-sealed-memfd-bytes",
        "execute": "verified-fd-without-path-reresolution",
        "enforced_without_binding": "forbidden",
    } or profile["sandbox"]["applied_evidence"] != {
        "digest_domain": "cxxlens.provider-sandbox-evidence.v3",
        "binds": [
            "resolved-policy-canonical-form",
            "recomputed-policy-digest",
            "measured-executable-digest",
            "achieved-assurance",
            "invocation-budget-limits",
            "exact-applied-mechanisms",
        ],
        "runtime_recompute": "required-before-report-adoption",
        "request_digest_echo": "forbidden",
        "mechanism_install_failure": "achieved-none-and-security.sandbox-insufficient",
    }:
        fail("security.sandbox-policy-mismatch", "verified executable evidence contract")
    policies = profile["sandbox"]["policy_registry"]["policies"]
    policy_ids = [policy["id"] for policy in policies]
    policy_digests = [policy["digest"] for policy in policies]
    mechanism_sets = [tuple(policy["mechanisms"]) for policy in policies]
    if (
        len(policies) != 2
        or policy_ids != sorted(policy_ids)
        or len(set(policy_ids)) != len(policy_ids)
        or len(set(policy_digests)) != len(policy_digests)
        or len(set(mechanism_sets)) != len(mechanism_sets)
        or not all(digest.startswith("semantic-v2:sha256:") for digest in policy_digests)
    ):
        fail("security.sandbox-policy-mismatch", "built-in policy registry is not canonical")
    runtime_test = (root / "tests/unit/sdk/provider_runtime_test.cpp").read_text(
        encoding="utf-8"
    )
    if any(digest.removeprefix("semantic-v2:sha256:") not in runtime_test for digest in policy_digests):
        fail(
            "security.sandbox-policy-mismatch",
            "runtime policy vectors do not bind every authority digest",
        )
    runtime_evidence = {
        "include/cxxlens/sdk/provider.hpp": (
            "class provider_selection",
            "selected_candidate() const",
            "authority_request() const",
            "result<void> validate() const",
            "struct sandbox_policy",
            "resolve_sandbox_policy",
            "sandbox_evidence_digest",
            "measured_executable_digest",
        ),
        "src/sdk/provider.cpp": (
            "provider_selection::validate() const",
            "selection-token",
            "decision-binding",
            "authority-revalidation",
            "candidate_identity_digest",
            "duplicate-canonical-candidate",
            "builtin_sandbox_policies",
            "unknown-policy",
        ),
        "src/sdk/provider_runtime.cpp": (
            "request.selection.validate()",
            "effective_sandbox",
            "security.sandbox-policy-mismatch",
            "sandbox_evidence_digest",
            "actual_mechanisms",
        ),
        "src/runtime/provider_process_adapter.cpp": (
            "resolve_sandbox_policy",
            "configure_child(invocation, *policy)",
            "security.sandbox-insufficient",
        ),
    }
    for relative, markers in runtime_evidence.items():
        text = (root / relative).read_text(encoding="utf-8")
        missing = [marker for marker in markers if marker not in text]
        if missing:
            fail(
                "security.untrusted-input-invalid",
                f"{relative} lacks execution authority markers: {missing}",
            )
    entry_keys = [(row["kind"], row["prefix"]) for row in namespaces["entries"]]
    if len(entry_keys) != len(set(entry_keys)):
        fail("security.namespace-collision", "duplicate kind/prefix")
    certificate_ids = [row["id"] for row in certifications["certificates"]]
    if len(certificate_ids) != len(set(certificate_ids)):
        fail("security.duplicate-provider", "duplicate certificate ID")
    support_keys = [tuple(row[field] for field in support["tuple_identity"]) for row in support["entries"]]
    if len(support_keys) != len(set(support_keys)):
        fail("security.duplicate-provider", "duplicate support tuple")
    vector_ids = [row["id"] for row in vectors["vectors"]]
    if len(vector_ids) != len(set(vector_ids)):
        fail("security.untrusted-input-invalid", "duplicate vector ID")
    declared = set(profile["reason_codes"])
    referenced = {row["expected"]["reason_code"] for row in vectors["vectors"]}
    if not referenced <= declared:
        fail("security.untrusted-input-invalid", f"undeclared reason codes: {sorted(referenced - declared)}")
    results = [execute_vector(row, profile, namespaces, certifications, support) for row in vectors["vectors"]]
    design = (root / "docs/design/cxxlens_next_generation_integrated_design_ja.md").read_text(encoding="utf-8")
    for marker in ("1.0.0-normative", "cxxlens.security-profile.v1", "security.provider-shadowing", "ADR 0011", "Issue #151", "ADR 0082"):
        if marker not in design:
            fail("security.untrusted-input-invalid", f"design marker missing: {marker}")
    counts = {
        "namespaces": len(namespaces["entries"]),
        "trust_anchors": len(certifications["trust_anchors"]),
        "issuers": len(certifications["issuers"]),
        "certificates": len(certifications["certificates"]),
        "support_tuples": len(support["entries"]),
        "production_certificates": sum(
            1
            for certificate in certifications["certificates"]
            if any(row["level"] == "production-supported" for row in certificate["qualifications"])
        ),
    }
    return profile, results, counts


def report(profile: dict[str, Any], results: list[dict[str, Any]], counts: dict[str, int]) -> dict[str, Any]:
    return {
        "schema": "cxxlens.security-conformance-report.v1",
        "status": "green",
        "contract_digest": digest(profile),
        "vector_count": len(results),
        "accepted": sum(row["decision"] == "accepted" for row in results),
        "rejected": sum(row["decision"] == "rejected" for row in results),
        "reason_codes": sorted({row["reason_code"] for row in results}),
        "registry_counts": counts,
    }


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    profile, results, counts = validate_all(root)
    value = report(profile, results, counts)
    schema_validate(value, load_yaml(root / REPORT_SCHEMA), "security report")
    if args.mode == "report":
        if args.output is None:
            fail("security.untrusted-input-invalid", "report requires --output")
        if subprocess.run(["git", "-C", str(root), "status", "--porcelain"], check=True, capture_output=True, text=True).stdout.strip():
            fail("security.untrusted-input-invalid", "commit-bound report requires clean worktree")
        output = args.output if args.output.is_absolute() else root / args.output
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        "security contract passed: "
        f"{len(results)} vectors, {value['accepted']} accepted, {value['rejected']} rejected, "
        f"{counts['namespaces']} namespaces, {counts['support_tuples']} support tuples, "
        f"digest {value['contract_digest']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (SecurityContractError, jsonschema.ValidationError, OSError, subprocess.SubprocessError, yaml.YAMLError) as error:
        print(f"security contract failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
