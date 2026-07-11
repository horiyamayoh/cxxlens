#!/usr/bin/env python3
"""Validate the M0 completion manifest and acceptance ownership statically."""

from __future__ import annotations

import pathlib
import re
import sys

import jsonschema
import yaml


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    manifest = yaml.safe_load((root / "schemas/cxxlens_m0_completion.yaml").read_text())
    schema = yaml.safe_load((root / "schemas/cxxlens_m0_completion.schema.yaml").read_text())
    jsonschema.validate(manifest, schema)
    catalog = yaml.safe_load((root / "schemas/cxxlens_public_api_contract.yaml").read_text())
    catalog_by_id = {
        api["id"]: api for package in catalog["packages"] for api in package["apis"]
    }
    conformant = manifest["conformant_catalog_ids"]
    failures: list[str] = []
    if any(catalog_by_id[api_id]["implementation_state"] != "conformant" for api_id in conformant):
        failures.append("an M0 completion API is no longer conformant in the catalog")
    vector_catalog = sorted(
        catalog_id for vector in manifest["vectors"] for catalog_id in vector["catalog_ids"]
    )
    if vector_catalog != conformant:
        failures.append("completion vectors do not map every conformant catalog API exactly once")
    requirements = {
        requirement for vector in manifest["vectors"] for requirement in vector["requirements"]
    }
    missing_qr = {f"QR-{index:03d}" for index in range(1, 11)} - requirements
    if missing_qr:
        failures.append(f"M0 QR-001..010 traceability is incomplete: {sorted(missing_qr)}")
    headers = sorted(
        path.relative_to(root).as_posix() for path in (root / "include/cxxlens").rglob("*.hpp")
    )
    missing_headers = set(manifest["public_headers"]) - set(headers)
    if missing_headers:
        failures.append(f"M0 public headers are missing: {sorted(missing_headers)}")
    tests_cmake = (root / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    test_names = set(re.findall(r"\bNAME\s+([a-z0-9.-]+)", tests_cmake))
    test_names.update(
        f"public-api.{header}-header" for header in ("core", "source", "cxxlens")
    )
    for vector in manifest["vectors"]:
        missing = set(vector["tests"]) - test_names
        if missing:
            failures.append(f"{vector['id']} references missing tests: {sorted(missing)}")
        if not (root / vector["fixture"]).exists():
            failures.append(f"{vector['id']} fixture is missing: {vector['fixture']}")
    workflow = (root / ".github/workflows/quality.yml").read_text(encoding="utf-8")
    if "cxxlens-m0-acceptance" not in workflow:
        failures.append("M0 acceptance target is not required by PR CI")
    aggregator = (root / "include/cxxlens/cxxlens.hpp").read_text(encoding="utf-8")
    for header in ("configuration.hpp", "core.hpp", "source.hpp", "testing.hpp"):
        if f"<cxxlens/{header}>" not in aggregator:
            failures.append(f"umbrella header is missing {header}")
    if failures:
        print("M0 completion check failed:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    print(
        f"validated M0 completion manifest: {len(manifest['vectors'])} vectors, "
        f"{len(conformant)} conformant APIs, {len(manifest['public_headers'])} public headers"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
