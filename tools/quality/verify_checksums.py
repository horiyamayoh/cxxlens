#!/usr/bin/env python3
"""Generate or check the current next-generation design package checksums."""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = pathlib.Path("docs/design/SHA256SUMS")
PACKAGE_PATHS = (
    "docs/design/README.md",
    "docs/design/cxxlens_next_generation_integrated_design_ja.md",
    "docs/design/adr/README.md",
    "docs/design/adr/0002-semantic-relation-platform.md",
    "docs/design/adr/0003-versioned-relation-kernel.md",
    "docs/design/adr/0005-product-boundary-release-compatibility.md",
    "docs/design/adr/0006-ng0-relation-and-claim-envelope.md",
    "docs/design/adr/0007-logical-query-algebra.md",
    "docs/design/adr/0008-truth-guarantee-provenance-algebra.md",
    "docs/design/adr/0009-snapshot-identity-publication-series.md",
    "docs/design/adr/0010-provider-wire-streaming-atomicity.md",
    "docs/design/adr/0011-provider-trust-certification-discovery.md",
    "docs/design/adr/0012-author-sdk-surface.md",
    "docs/design/adr/0013-ng-sqlite-physical-store.md",
    "docs/design/adr/0014-deterministic-reference-query-runtime.md",
    "docs/design/adr/0015-process-provider-runtime-clang22-normalizer.md",
    "docs/design/adr/0091-distribution-1-production-qualification.md",
    "docs/design/adr/0092-exact-public-callable-inventory.md",
    "docs/design/adr/0093-implementation-learning-design-feedback.md",
    "docs/design/adr/0094-risk-tiered-goal-authorization.md",
    "docs/design/adr/0095-production-scope-closure.md",
    "docs/design/adr/0096-clang22-installed-materialization-boundary.md",
    "docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md",
    "docs/design/catalogs/README.md",
    "schemas/cxxlens_ng_claim_envelope.schema.yaml",
    "schemas/cxxlens_ng_relation_registry.yaml",
    "schemas/cxxlens_ng_relation_registry.schema.yaml",
    "schemas/cxxlens_ng_relation_conformance_vectors.yaml",
    "schemas/cxxlens_ng_relation_conformance_vectors.schema.yaml",
    "schemas/cxxlens_ng_relation_conformance_report.schema.yaml",
    "schemas/cxxlens_ng_logical_query_contract.yaml",
    "schemas/cxxlens_ng_logical_query_contract.schema.yaml",
    "schemas/cxxlens_ng_logical_query_ir.schema.yaml",
    "schemas/cxxlens_ng_query_conformance_vectors.yaml",
    "schemas/cxxlens_ng_query_conformance_vectors.schema.yaml",
    "schemas/cxxlens_ng_query_conformance_report.schema.yaml",
    "schemas/cxxlens_ng_semantic_guarantee_contract.yaml",
    "schemas/cxxlens_ng_semantic_guarantee_contract.schema.yaml",
    "schemas/cxxlens_ng_semantic_metadata.schema.yaml",
    "schemas/cxxlens_ng_semantic_conformance_vectors.yaml",
    "schemas/cxxlens_ng_semantic_conformance_vectors.schema.yaml",
    "schemas/cxxlens_ng_semantic_conformance_report.schema.yaml",
    "schemas/cxxlens_ng_snapshot_store_contract.yaml",
    "schemas/cxxlens_ng_snapshot_store_contract.schema.yaml",
    "schemas/cxxlens_ng_sqlite_store_contract.yaml",
    "schemas/cxxlens_ng_sqlite_store_contract.schema.yaml",
    "schemas/cxxlens_ng_snapshot_manifest.schema.yaml",
    "schemas/cxxlens_ng_store_conformance_vectors.yaml",
    "schemas/cxxlens_ng_store_conformance_vectors.schema.yaml",
    "schemas/cxxlens_ng_store_conformance_report.schema.yaml",
    "schemas/cxxlens_ng_provider_protocol.yaml",
    "schemas/cxxlens_ng_provider_protocol.schema.yaml",
    "schemas/cxxlens_ng_provider_manifest.schema.yaml",
    "schemas/cxxlens_ng_provider_task.schema.yaml",
    "schemas/cxxlens_ng_provider_conformance_vectors.yaml",
    "schemas/cxxlens_ng_provider_conformance_vectors.schema.yaml",
    "schemas/cxxlens_ng_provider_conformance_report.schema.yaml",
    "schemas/cxxlens_ng_provider_fuzz_corpus.yaml",
    "schemas/cxxlens_ng_provider_fuzz_corpus.schema.yaml",
    "schemas/cxxlens_ng_portable_provider_task_contract.yaml",
    "schemas/cxxlens_ng_portable_provider_task_contract.schema.yaml",
    "schemas/cxxlens_ng_provider_runtime_contract.yaml",
    "schemas/cxxlens_ng_provider_runtime_contract.schema.yaml",
    "schemas/cxxlens_ng_provider_execution_report.schema.yaml",
    "schemas/cxxlens_ng_clang22_materialization_contract.yaml",
    "schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml",
    "schemas/cxxlens_ng_clang22_materialization_request.schema.yaml",
    "schemas/cxxlens_ng_clang22_materialization_report.schema.yaml",
    "schemas/cxxlens_ng_clang22_materializer_occurrence_manifest.schema.yaml",
    "schemas/cxxlens_ng_public_api_catalog.yaml",
    "schemas/cxxlens_ng_public_api_catalog.schema.yaml",
    "schemas/cxxlens_ng_public_callable_inventory.yaml",
    "schemas/cxxlens_ng_public_callable_inventory.schema.yaml",
    "schemas/cxxlens_ng_public_callable_inventory_report.schema.yaml",
    "schemas/cxxlens_ng_acceptance_manifest.yaml",
    "schemas/cxxlens_ng_acceptance_manifest.schema.yaml",
    "schemas/cxxlens_ng_quality_ownership.yaml",
    "schemas/cxxlens_ng_quality_ownership.schema.yaml",
    "schemas/cxxlens_ng_quality_evidence.schema.yaml",
    "schemas/cxxlens_ng_design_feedback_record.schema.yaml",
    "schemas/cxxlens_ng_install_artifact_manifest.schema.yaml",
    "schemas/cxxlens_ng_foundation_completion_manifest.yaml",
    "schemas/cxxlens_ng_foundation_completion_manifest.schema.yaml",
    "schemas/cxxlens_ng_foundation_audit_report.schema.yaml",
    "schemas/cxxlens_ng_foundation_completion_report.schema.yaml",
    "schemas/cxxlens_ng_security_profile.yaml",
    "schemas/cxxlens_ng_security_profile.schema.yaml",
    "schemas/cxxlens_ng_namespace_registry.yaml",
    "schemas/cxxlens_ng_namespace_registry.schema.yaml",
    "schemas/cxxlens_ng_provider_certification_registry.yaml",
    "schemas/cxxlens_ng_provider_certification_registry.schema.yaml",
    "schemas/cxxlens_ng_provider_support_matrix.yaml",
    "schemas/cxxlens_ng_provider_support_matrix.schema.yaml",
    "schemas/cxxlens_ng_security_conformance_vectors.yaml",
    "schemas/cxxlens_ng_security_conformance_vectors.schema.yaml",
    "schemas/cxxlens_ng_security_conformance_report.schema.yaml",
    "schemas/cxxlens_ng_release_bundle.yaml",
    "schemas/cxxlens_ng_release_bundle.schema.yaml",
    "schemas/cxxlens_ng_release_qualification.yaml",
    "schemas/cxxlens_ng_release_qualification.schema.yaml",
    "schemas/cxxlens_ng_release_qualification_report.schema.yaml",
    "schemas/cxxlens_ng_release_qualification_evaluation_report.schema.yaml",
    "schemas/cxxlens_ng_production_scope_closure.yaml",
    "schemas/cxxlens_ng_production_scope_closure.schema.yaml",
    "schemas/cxxlens_ng_production_scope_closure_report.schema.yaml",
    "schemas/cxxlens_ng_compatibility_request.schema.yaml",
    "schemas/cxxlens_ng_compatibility_report.schema.yaml",
    "schemas/cxxlens_asset_migration_policy.yaml",
    "schemas/cxxlens_asset_migration_ledger.json",
)


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def render(root: pathlib.Path) -> str:
    design = root / "docs/design"
    rows = []
    for relative in PACKAGE_PATHS:
        path = root / relative
        if not path.is_file():
            raise FileNotFoundError(f"design package input is missing: {relative}")
        relative_to_manifest = pathlib.PurePath(os.path.relpath(path, design)).as_posix()
        rows.append(f"{digest(path)}  {relative_to_manifest}")
    return "\n".join(rows) + "\n"


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("generate", "check"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    expected = render(root)
    manifest = root / MANIFEST
    if args.mode == "generate":
        manifest.write_text(expected, encoding="utf-8")
    elif manifest.read_text(encoding="utf-8") != expected:
        print("design package checksum inventory is stale; run generate mode", file=sys.stderr)
        return 1
    print(f"verified {len(PACKAGE_PATHS)} design package checksums")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OSError as error:
        print(f"design package checksum failure: {error}", file=sys.stderr)
        raise SystemExit(1) from error
