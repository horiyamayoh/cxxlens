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
    "docs/design/adr/0001-frontend-worker-isolation.md",
    "docs/design/adr/0002-semantic-relation-platform.md",
    "docs/design/adr/0003-versioned-relation-kernel.md",
    "docs/design/adr/0004-legacy-contract-reset.md",
    "docs/design/adr/0005-product-boundary-release-compatibility.md",
    "docs/design/adr/0006-ng0-relation-and-claim-envelope.md",
    "docs/design/adr/0007-logical-query-algebra.md",
    "docs/design/catalogs/README.md",
    "schemas/cxxlens_ng_authority_transition.yaml",
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
    "schemas/cxxlens_ng_provider_protocol.yaml",
    "schemas/cxxlens_ng_public_api_catalog.yaml",
    "schemas/cxxlens_ng_acceptance_manifest.yaml",
    "schemas/cxxlens_ng_security_profile.yaml",
    "schemas/cxxlens_ng_release_bundle.yaml",
    "schemas/cxxlens_ng_release_bundle.schema.yaml",
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
