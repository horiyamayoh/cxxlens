#!/usr/bin/env python3
"""Validate M0-M2 traceability and the installed flagship integration gate."""

from __future__ import annotations

import math
import pathlib
import re
import sys

import jsonschema
import yaml


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    manifest = yaml.safe_load((root / "schemas/cxxlens_m2_completion.yaml").read_text())
    schema = yaml.safe_load((root / "schemas/cxxlens_m2_completion.schema.yaml").read_text())
    jsonschema.validate(manifest, schema)
    catalog = yaml.safe_load((root / "schemas/cxxlens_public_api_contract.yaml").read_text())
    catalog_by_id = {
        api["id"]: api for package in catalog["packages"] for api in package["apis"]
    }
    m2_apis = [api for api in catalog_by_id.values() if api["phase"] == "M2"]
    complete_ids = sorted(
        api["id"]
        for api in m2_apis
        if api["declaration"]["status"] == "exact"
        and api["implementation_state"] == "conformant"
        and api["readiness"]["state"] == "complete"
    )
    blocked_ids = sorted(
        api["id"]
        for api in m2_apis
        if api["implementation_state"] == "unimplemented"
        and api["readiness"]["state"] == "blocked"
    )
    deferred_ids = sorted(entry["id"] for entry in manifest["deferred_catalog"])
    failures: list[str] = []
    if complete_ids != sorted(manifest["conformant_catalog_ids"]):
        failures.append("M2 exact complete catalog signatures are not mapped exactly once")
    if blocked_ids != deferred_ids:
        failures.append("M2 blocked catalog entries are not explicitly deferred")
    for api_id in complete_ids:
        api = catalog_by_id[api_id]
        if api["implementation_state"] != "conformant" or api["readiness"]["state"] != "complete":
            failures.append(f"{api_id} exact M2 signature lacks completion evidence")
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
        failures.append("M2 vectors do not map every exact complete catalog signature exactly once")
    vector_by_api = {
        api_id: vector for vector in manifest["vectors"] for api_id in vector["catalog_ids"]
    }
    for api_id in complete_ids:
        missing = set(catalog_by_id[api_id].get("requirements", [])) - set(
            vector_by_api[api_id]["requirements"]
        )
        if missing:
            failures.append(f"{api_id} requirements are missing from its M2 vector: {sorted(missing)}")

    foundation_owners: set[int] = set()
    for path in manifest["foundation_manifests"]:
        full_path = root / path
        if not full_path.exists():
            failures.append(f"foundation manifest is missing: {path}")
            continue
        foundation = yaml.safe_load(full_path.read_text())
        foundation_owners.update(vector["owner_issue"] for vector in foundation["vectors"])
    owners = foundation_owners | {vector["owner_issue"] for vector in manifest["vectors"]}
    if owners != set(range(2, 27)):
        failures.append("M0-M2 issue traceability must cover #2 through #26 exactly")

    axes = manifest["matrix"]
    calculated_executions = (
        math.prod(
            len(axes[name])
            for name in ("jobs", "scheduler_seeds", "orders", "roots", "backends", "cache_states")
        )
        * axes["repeats"]
    )
    if calculated_executions != axes["expected_executions"]:
        failures.append("M2 matrix execution count does not equal its declared Cartesian axes")

    tests_cmake = (root / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    m0_dependencies = re.search(
        r"add_dependencies\(\s*cxxlens-m0-test-binaries(?P<body>.*?)\)",
        tests_cmake,
        re.DOTALL,
    )
    cumulative_non_adapter_targets = {
        "cxxlens-public-header-explain",
        "cxxlens-public-header-search",
        "cxxlens-public-header-select",
        "cxxlens-query-process",
        "cxxlens-search-process",
        "cxxlens-selector-process",
        "cxxlens-unit-fact-reducer",
        "cxxlens-unit-fact-store",
        "cxxlens-unit-provisioning",
        "cxxlens-unit-query-engine",
        "cxxlens-unit-search",
        "cxxlens-unit-selectors",
    }
    if m0_dependencies is None or not cumulative_non_adapter_targets.issubset(
        set(m0_dependencies.group("body").split())
    ):
        failures.append("M0 acceptance does not build every non-adapter test binary")
    for milestone, dependency in (("m1", "m0"), ("m2", "m1")):
        marker = (
            f"add_dependencies(cxxlens-{milestone}-test-binaries "
            f"cxxlens-{dependency}-test-binaries)"
        )
        if marker not in tests_cmake:
            failures.append(
                f"{milestone.upper()} acceptance does not build cumulative "
                f"{dependency.upper()} test binaries"
            )
    test_names = set(re.findall(r"\bNAME\s+([a-z0-9.-]+)", tests_cmake))
    test_names.update(
        f"public-api.{header}-header" for header in ("cxxlens", "explain", "search")
    )
    for vector in manifest["vectors"]:
        missing = set(vector["tests"]) - test_names
        if missing:
            failures.append(f"{vector['id']} references missing tests: {sorted(missing)}")
        if not (root / vector["fixture"]).exists():
            failures.append(f"{vector['id']} fixture is missing: {vector['fixture']}")

    for header in manifest["stable_public_headers"]:
        if not (root / header).exists():
            failures.append(f"M2 public header is missing: {header}")
    umbrella = (root / "include/cxxlens/cxxlens.hpp").read_text(encoding="utf-8")
    for header in ("explain.hpp", "search.hpp", "select.hpp"):
        if f"<cxxlens/{header}>" not in umbrella:
            failures.append(f"umbrella header is missing {header}")
    if "interop/clang.hpp" in umbrella:
        failures.append("umbrella header exposed the explicit raw Clang corridor")

    example = (root / "examples/m2-flagship/main.cpp").read_text(encoding="utf-8")
    for required in ("<cxxlens/cxxlens.hpp>", "workspace::open", "search::calls", "why_not_matched"):
        if required not in example:
            failures.append(f"installed M2 example is missing public operation: {required}")
    if "detail::" in example or "src/" in example:
        failures.append("installed M2 example uses an internal/test-only semantic shortcut")

    workflow = (root / ".github/workflows/quality.yml").read_text(encoding="utf-8")
    if "cxxlens-m2-acceptance" not in workflow:
        failures.append("M2 acceptance target is not required by push/PR CI")
    presets = (root / "CMakePresets.json").read_text(encoding="utf-8")
    if '"m2-acceptance"' not in presets:
        failures.append("M2 clean-checkout preset is missing")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    for required_artifact in (
        "cxxlens_m2_completion.yaml",
        "cxxlens_m2_completion.schema.yaml",
        "cxxlens_m2_acceptance_report.schema.yaml",
        "examples/m2-flagship",
    ):
        if required_artifact not in cmake:
            failures.append(f"installed M2 artifact is missing from CMake: {required_artifact}")

    if failures:
        print("M2 completion check failed:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    print(
        f"validated M2 completion manifest: {len(manifest['vectors'])} vectors, "
        f"{len(complete_ids)} exact complete APIs, {len(deferred_ids)} explicit deferrals, "
        f"{axes['expected_executions']} installed matrix executions"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
