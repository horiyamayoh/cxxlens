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
    conformant = sorted(
        api["id"]
        for package in catalog["packages"]
        for api in package["apis"]
        if api["implementation_state"] == "conformant"
    )
    failures: list[str] = []
    if manifest["conformant_catalog_ids"] != conformant:
        failures.append("completion manifest does not own every conformant catalog API")
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
    if manifest["public_headers"] != headers:
        failures.append("public header manifest is stale")
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
        f"{len(conformant)} conformant APIs, {len(headers)} public headers"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
