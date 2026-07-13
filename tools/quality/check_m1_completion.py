#!/usr/bin/env python3
"""Validate the M1 completion manifest and integration-gate ownership."""

from __future__ import annotations

import pathlib
import re
import sys

import jsonschema
import yaml


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    manifest = yaml.safe_load((root / "schemas/cxxlens_m1_completion.yaml").read_text())
    schema = yaml.safe_load(
        (root / "schemas/cxxlens_m1_completion.schema.yaml").read_text()
    )
    jsonschema.validate(manifest, schema)
    catalog = yaml.safe_load(
        (root / "schemas/cxxlens_public_api_contract.yaml").read_text()
    )
    catalog_by_id = {
        api["id"]: api for package in catalog["packages"] for api in package["apis"]
    }
    m1_apis = [api for api in catalog_by_id.values() if api["phase"] == "M1"]
    complete_ids = sorted(
        api["id"]
        for api in m1_apis
        if api["declaration"]["status"] == "exact"
        and api["implementation_state"] == "conformant"
        and api["readiness"]["state"] == "complete"
    )
    blocked_ids = sorted(
        api["id"]
        for api in m1_apis
        if api["implementation_state"] == "unimplemented"
        and api["readiness"]["state"] == "blocked"
    )
    deferred_ids = sorted(entry["id"] for entry in manifest["deferred_catalog"])
    failures: list[str] = []
    if complete_ids != sorted(manifest["conformant_catalog_ids"]):
        failures.append("M1 exact complete catalog signatures are not mapped exactly once")
    if blocked_ids != deferred_ids:
        failures.append("M1 blocked catalog entries are not explicitly deferred")
    for api_id in complete_ids:
        api = catalog_by_id[api_id]
        if api["implementation_state"] != "conformant" or api["readiness"]["state"] != "complete":
            failures.append(f"{api_id} exact M1 signature lacks completion evidence")
    for entry in manifest["deferred_catalog"]:
        api = catalog_by_id[entry["id"]]
        if (
            api["implementation_state"] != "unimplemented"
            or api["readiness"]["state"] != "blocked"
            or entry["blocker"] not in api["readiness"]["blockers"]
        ):
            failures.append(f"{entry['id']} deferred blocker disagrees with the catalog")

    vector_catalog = sorted(
        api_id for vector in manifest["vectors"] for api_id in vector["catalog_ids"]
    )
    if vector_catalog != complete_ids:
        failures.append("M1 vectors do not map every exact complete catalog signature exactly once")
    vector_facts = sorted(
        kind for vector in manifest["vectors"] for kind in vector["fact_kinds"]
    )
    if vector_facts != sorted(manifest["fact_kinds"]):
        failures.append("M1 vectors do not map every supported fact kind exactly once")
    owners = {vector["owner_issue"] for vector in manifest["vectors"]}
    if owners != set(range(13, 23)):
        failures.append("M1 issue traceability must cover #13 through #22")

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

    for header in manifest["stable_public_headers"] + [manifest["explicit_interop_header"]]:
        if not (root / header).exists():
            failures.append(f"M1 public header is missing: {header}")
    umbrella = (root / "include/cxxlens/cxxlens.hpp").read_text(encoding="utf-8")
    for header in ("facts.hpp", "testing.hpp", "workspace.hpp"):
        if f"<cxxlens/{header}>" not in umbrella:
            failures.append(f"umbrella header is missing {header}")
    if "interop/clang.hpp" in umbrella:
        failures.append("umbrella header exposed the explicit raw Clang corridor")

    workflow = (root / ".github/workflows/quality.yml").read_text(encoding="utf-8")
    if "cxxlens-m1-acceptance" not in workflow:
        failures.append("M1 acceptance target is not required by push/PR CI")
    presets = (root / "CMakePresets.json").read_text(encoding="utf-8")
    if '"m1-acceptance"' not in presets:
        failures.append("M1 clean-checkout preset is missing")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    for required_schema in (
        "cxxlens_m1_completion.yaml",
        "cxxlens_m1_completion.schema.yaml",
        "cxxlens_m1_acceptance_report.schema.yaml",
    ):
        if required_schema not in cmake:
            failures.append(f"installed M1 schema is missing from CMake: {required_schema}")

    if failures:
        print("M1 completion check failed:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    print(
        f"validated M1 completion manifest: {len(manifest['vectors'])} vectors, "
        f"{len(complete_ids)} exact complete APIs, {len(manifest['fact_kinds'])} fact kinds, "
        f"{len(deferred_ids)} explicit deferrals"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
