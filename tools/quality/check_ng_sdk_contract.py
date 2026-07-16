#!/usr/bin/env python3
"""Validate the implemented next-generation author SDK contract."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys
import tempfile
from typing import Any

import jsonschema
import yaml

import check_ng_query_contract as query_contract


ROOT = pathlib.Path(__file__).resolve().parents[2]
CATALOG = pathlib.Path("schemas/cxxlens_ng_public_api_catalog.yaml")
SCHEMA = pathlib.Path("schemas/cxxlens_ng_public_api_catalog.schema.yaml")
EXPECTED_PATHS = {
    "generated-typed-query",
    "runtime-dynamic-query",
    "portable-provider",
    "clang22-native-provider",
    "high-level-recipe",
}
EXPECTED_IMPLEMENTED_ENTRIES = {
    "public.common",
    "public.project-catalog",
    "public.relation-static",
    "public.relation-dynamic",
    "public.snapshot-store",
    "public.logical-query",
    "public.provider-sdk",
    "public.native-provider-sdk",
    "public.recipe-foundation",
}
FORBIDDEN_ORDINARY = (
    re.compile(r"\bclang::"),
    re.compile(r"\bllvm::"),
    re.compile(r"#\s*include\s*[<\"](?:clang|llvm)/"),
)


class SdkContractError(ValueError):
    """Stable SDK catalog or implementation violation."""


def fail(message: str) -> None:
    raise SdkContractError(message)


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail(f"expected mapping: {path}")
    return value


def schema_validate(document: dict[str, Any], schema: dict[str, Any]) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"SDK catalog schema validation failed: {error.message}")


def unique_rows(rows: list[dict[str, Any]], label: str) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for row in rows:
        identifier = row["id"]
        if identifier in result:
            fail(f"duplicate {label} ID: {identifier}")
        result[identifier] = row
    return result


def validate_catalog(root: pathlib.Path, catalog: dict[str, Any]) -> None:
    schema_validate(catalog, load_yaml(root / SCHEMA))
    paths = unique_rows(catalog["author_paths"], "author path")
    if set(paths) != EXPECTED_PATHS:
        fail(f"author path set differs: {sorted(paths)}")
    entries = unique_rows(catalog["entries"], "public entry")
    implemented = {
        identifier
        for identifier, entry in entries.items()
        if entry["status"] == "implemented"
    }
    if not EXPECTED_IMPLEMENTED_ENTRIES.issubset(implemented):
        fail(
            "implemented SDK entry set is incomplete: "
            f"{sorted(EXPECTED_IMPLEMENTED_ENTRIES - implemented)}"
        )
    if entries.get("public.recipe", {}).get("owner_issue") != "#73":
        fail("flagship recipe ownership must remain with Issue #73")
    for path in paths.values():
        if path["entry"] not in entries:
            fail(f"author path references unknown entry: {path['entry']}")
        if path.get("negative_mode") not in {"compile-fail", "runtime-rejection"}:
            fail(f"author path has no exact negative mode: {path['id']}")
    for entry in entries.values():
        for dependency in entry.get("depends_on", []):
            if dependency not in entries:
                fail(f"public entry has dangling dependency: {entry['id']} -> {dependency}")

    referenced_paths: set[str] = set()
    for package in catalog["packages"]:
        referenced_paths.update(package["headers"])
    for path in paths.values():
        referenced_paths.update(
            path[field]
            for field in ("implementation", "positive_example", "negative_example", "harness")
        )
    for entry in entries.values():
        referenced_paths.update(entry["headers"])
        referenced_paths.update(entry["implementation_evidence"])
    missing = sorted(path for path in referenced_paths if not (root / path).is_file())
    if missing:
        fail(f"SDK catalog evidence is missing: {missing}")

    emitted_codes: set[str] = set()
    for source in [
        *sorted((root / "src/sdk").glob("*.cpp")),
        root / "src/llvm/clang22/provider_sdk.cpp",
    ]:
        emitted_codes.update(
            re.findall(
                r'"((?:sdk|provider|native|recipe)\.[a-z0-9._-]+)"',
                source.read_text(encoding="utf-8"),
            )
        )
    catalog_codes = {
        code
        for entry in entries.values()
        if entry["status"] == "implemented"
        for code in entry["errors"]
    }
    if missing_codes := sorted(emitted_codes - catalog_codes):
        fail(f"implemented SDK error codes are absent from the catalog: {missing_codes}")


def validate_boundaries(root: pathlib.Path) -> None:
    ordinary_roots = [root / "include/cxxlens/sdk", root / "include/cxxlens/relations"]
    violations: list[str] = []
    for ordinary_root in ordinary_roots:
        for header in sorted(ordinary_root.rglob("*.hpp")):
            for number, line in enumerate(header.read_text(encoding="utf-8").splitlines(), 1):
                if any(pattern.search(line) for pattern in FORBIDDEN_ORDINARY):
                    violations.append(f"{header.relative_to(root)}:{number}")
    for source in sorted((root / "src/sdk").glob("*.cpp")):
        for number, line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
            if any(pattern.search(line) for pattern in FORBIDDEN_ORDINARY):
                violations.append(f"{source.relative_to(root)}:{number}")
    if violations:
        fail(f"ordinary SDK leaks LLVM/Clang: {violations}")

    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    provider_block = cmake.split("add_library(\n  cxxlens_provider_sdk", 1)
    if len(provider_block) != 2:
        fail("independent cxxlens_provider_sdk target is missing")
    provider_block = provider_block[1].split("add_library(\n  cxxlens", 1)[0]
    if re.search(r"\b(?:LLVM|Clang|cxxlens::cxxlens)\b", provider_block):
        fail("ordinary provider SDK target has a forbidden link/config dependency")
    for marker in (
        "EXPORT cxxlensProviderSDKTargets",
        "cxxlensProviderSDKConfig.cmake",
        "EXPORT cxxlensClang22ProviderSDKTargets",
    ):
        if marker not in cmake:
            fail(f"SDK install package marker is missing: {marker}")

    extension_text = "\n".join(
        path.read_text(encoding="utf-8")
        for directory in ordinary_roots
        for path in sorted(directory.rglob("*.hpp"))
    )
    for pattern in (
        r"enum\s+class\s+relation_(?:id|kind|type)",
        r"enum\s+class\s+provider_(?:id|kind|type)",
        r"switch\s*\([^)]*(?:relation|provider)",
    ):
        if re.search(pattern, extension_text):
            fail(f"central relation/provider dispatch is forbidden: {pattern}")


def run(command: list[str], *, expect_success: bool, label: str) -> None:
    completed = subprocess.run(command, check=False, text=True, capture_output=True)
    succeeded = completed.returncode == 0
    if succeeded != expect_success:
        detail = completed.stderr.strip() or completed.stdout.strip()
        fail(f"{label} {'unexpectedly succeeded' if succeeded else 'failed'}: {detail}")


def validate_generation_and_negatives(root: pathlib.Path, compiler: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cxxlens-sdk-contract-") as directory:
        temporary = pathlib.Path(directory)
        generated = temporary / "cc_call_site.hpp"
        run(
            [
                sys.executable,
                str(root / "tools/sdk/relation_idl_compiler.py"),
                "--registry",
                str(root / "schemas/cxxlens_ng_relation_registry.yaml"),
                "--relation",
                "cc.call_site",
                "--output",
                str(generated),
            ],
            expect_success=True,
            label="relation IDL generation",
        )
        generated_text = generated.read_text(encoding="utf-8")
        registry = load_yaml(root / "schemas/cxxlens_ng_relation_registry.yaml")
        relation = next(row for row in registry["relations"] if row["name"] == "cc.call_site")
        for marker in [relation["descriptor_id"], *(row["id"] for row in relation["columns"])]:
            if marker not in generated_text:
                fail(f"generated relation tag omitted registry identity: {marker}")
        source = temporary / "generated_test.cpp"
        source.write_text(
            '#include "cc_call_site.hpp"\n'
            "int main(){return cxxlens::cc::relations::call_site::descriptor().validate()?0:1;}\n",
            encoding="utf-8",
        )
        run(
            [compiler, "-std=c++23", "-fsyntax-only", f"-I{root / 'include'}", str(source)],
            expect_success=True,
            label="generated relation tag syntax check",
        )

        for relative in (
            "examples/sdk/negative/generated_unknown_column.cpp",
            "examples/sdk/negative/native_pointer_escape.cpp",
        ):
            run(
                [
                    compiler,
                    "-std=c++23",
                    "-fsyntax-only",
                    f"-I{root / 'include'}",
                    str(root / relative),
                ],
                expect_success=False,
                label=f"compile-fail example {relative}",
            )


def validate_scaffold(root: pathlib.Path, compiler: str, executable: str) -> None:
    manifest_schema = load_yaml(root / "schemas/cxxlens_ng_provider_manifest.schema.yaml")
    with tempfile.TemporaryDirectory(prefix="cxxlens-provider-scaffold-") as directory:
        temporary = pathlib.Path(directory)
        for provider_class, expected in (
            ("portable", "cxxlens::provider_sdk"),
            ("clang22-native", "cxxlens::clang22_provider_sdk"),
        ):
            output = temporary / provider_class
            run(
                [
                    executable,
                    str(output),
                    f"company.test.{provider_class.replace('-', '_')}",
                    provider_class,
                    "company.test.relation",
                ],
                expect_success=True,
                label=f"{provider_class} provider scaffold",
            )
            document = json.loads((output / "provider-manifest.json").read_text(encoding="utf-8"))
            try:
                jsonschema.Draft202012Validator(manifest_schema).validate(document)
            except jsonschema.ValidationError as error:
                fail(f"generated provider manifest schema validation failed: {error.message}")
            cmake = (output / "CMakeLists.txt").read_text(encoding="utf-8")
            if expected not in cmake:
                fail(f"{provider_class} scaffold links the wrong SDK target")
            run(
                [
                    compiler,
                    "-std=c++23",
                    "-fsyntax-only",
                    f"-I{root / 'include'}",
                    str(output / "src/main.cpp"),
                ],
                expect_success=True,
                label=f"{provider_class} scaffold source syntax check",
            )


def validate_cpp_query_ir(root: pathlib.Path, executable: str) -> None:
    completed = subprocess.run(
        [executable, "query-ir"], check=False, text=True, capture_output=True
    )
    if completed.returncode != 0:
        fail(f"C++ logical query IR export failed: {completed.stderr.strip()}")
    try:
        document = json.loads(completed.stdout)
        query_contract.validate_ir(
            document,
            load_yaml(root / "schemas/cxxlens_ng_logical_query_ir.schema.yaml"),
            load_yaml(root / "schemas/cxxlens_ng_logical_query_contract.yaml"),
            load_yaml(root / "schemas/cxxlens_ng_relation_registry.yaml"),
        )
    except (json.JSONDecodeError, query_contract.QueryContractError) as error:
        fail(f"C++ logical query IR differs from the accepted authority: {error}")


def validate_cpp_provider_manifest(root: pathlib.Path, executable: str) -> None:
    completed = subprocess.run(
        [executable, "provider-manifest"],
        check=False,
        text=True,
        capture_output=True,
    )
    if completed.returncode != 0:
        fail(f"C++ provider manifest export failed: {completed.stderr.strip()}")
    try:
        document = json.loads(completed.stdout)
        jsonschema.Draft202012Validator(
            load_yaml(root / "schemas/cxxlens_ng_provider_manifest.schema.yaml")
        ).validate(document)
    except (json.JSONDecodeError, jsonschema.ValidationError) as error:
        fail(f"C++ provider manifest differs from the accepted authority: {error}")


def validate(
    root: pathlib.Path, compiler: str, scaffold: str, doctor: str
) -> dict[str, Any]:
    catalog = load_yaml(root / CATALOG)
    validate_catalog(root, catalog)
    validate_boundaries(root)
    validate_generation_and_negatives(root, compiler)
    validate_scaffold(root, compiler, scaffold)
    validate_cpp_query_ir(root, doctor)
    validate_cpp_provider_manifest(root, doctor)
    return catalog


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--scaffold", required=True)
    parser.add_argument("--doctor", required=True)
    return parser.parse_args()


def main() -> int:
    args = arguments()
    catalog = validate(args.root.resolve(), args.compiler, args.scaffold, args.doctor)
    print(
        "NG author SDK contract check passed: "
        f"{len(catalog['author_paths'])} author paths, {len(catalog['entries'])} catalog entries"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (SdkContractError, OSError, subprocess.SubprocessError, yaml.YAMLError) as error:
        print(f"NG author SDK contract check failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
