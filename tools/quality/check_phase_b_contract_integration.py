#!/usr/bin/env python3
"""Fail-closed Phase B integration gate for all public API contract surfaces."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
from typing import Any

import jsonschema
import yaml


MANIFEST_PATH = "schemas/cxxlens_phase_b_contract_integration.yaml"
SCHEMA_PATH = "schemas/cxxlens_phase_b_contract_integration.schema.yaml"
SIGNATURE_NAME = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")
FORBIDDEN_ORDINARY = (
    re.compile(r"\bclang::"),
    re.compile(r"\bllvm::"),
    re.compile(r"#\s*include\s*[<\"](?:clang|llvm)/"),
    re.compile(r"#\s*include\s*\"(?:\.\./|detail/)"),
    re.compile(r"\busing\s+namespace\b"),
    re.compile(r"^\s*#\s*define\b", re.MULTILINE),
)


class IntegrationError(ValueError):
    """A stable Phase B integration invariant failure."""


def fail(message: str) -> None:
    raise IntegrationError(message)


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail(f"expected mapping: {path}")
    return value


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value).encode("utf-8")).hexdigest()


def signature_digest(value: str) -> str:
    return "sha256:" + hashlib.sha256(value.encode("utf-8")).hexdigest()


def file_digest(path: pathlib.Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def registry_ids(rows: list[Any]) -> set[str]:
    return {row if isinstance(row, str) else row["id"] for row in rows}


def expanded_header(root: pathlib.Path, path: pathlib.Path, seen: set[pathlib.Path] | None = None) -> str:
    seen = set() if seen is None else seen
    resolved = path.resolve()
    if resolved in seen:
        return ""
    seen.add(resolved)
    text = path.read_text(encoding="utf-8")
    parts = [text]
    for include in re.findall(r"#\s*include\s*<((?:cxxlens)/[^>]+)>", text):
        child = root / "include" / include
        if child.is_file():
            parts.append(expanded_header(root, child, seen))
    return "\n".join(parts)


def git_identity(root: pathlib.Path) -> dict[str, Any]:
    def git(*arguments: str) -> str:
        result = subprocess.run(
            ["git", *arguments], cwd=root, check=False, text=True, capture_output=True
        )
        if result.returncode != 0:
            fail(f"git {' '.join(arguments)} failed: {result.stderr.strip()}")
        return result.stdout.strip()

    return {
        "commit": git("rev-parse", "HEAD"),
        "tree": git("rev-parse", "HEAD^{tree}"),
        "worktree_clean": not bool(git("status", "--porcelain")),
    }


def validate_metadata(root: pathlib.Path) -> tuple[dict[str, Any], dict[str, Any]]:
    manifest = load_yaml(root / MANIFEST_PATH)
    schema = load_yaml(root / SCHEMA_PATH)
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(manifest)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"integration manifest schema validation failed: {error.message}")

    for path in [*manifest["authorities"].values(), *manifest["evidence_inputs"]]:
        if not (root / path).is_file():
            fail(f"integration evidence is missing: {path}")

    catalog = load_yaml(root / manifest["authorities"]["catalog"])
    candidates = load_yaml(root / manifest["authorities"]["candidates"])
    high_risk = load_yaml(root / manifest["authorities"]["high_risk"])
    ownership = load_yaml(root / manifest["authorities"]["ownership"])
    conventions = load_yaml(root / manifest["authorities"]["conventions"])

    packages = catalog["packages"]
    apis = [(package, api) for package in packages for api in package["apis"]]
    atomic_units = [api["atomic_unit"]["id"] for _, api in apis]
    expected_counts = manifest["counts"]
    actual_counts = {
        "packages": len(packages),
        "apis": len(apis),
        "atomic_units": len(set(atomic_units)),
    }
    if actual_counts != expected_counts or len(atomic_units) != len(set(atomic_units)):
        fail(f"catalog count/exactly-once mismatch: {actual_counts}")

    public_root = root / "include/cxxlens"
    actual_headers = {
        path.relative_to(root).as_posix() for path in public_root.rglob("*.hpp")
    }
    declared_legacy_headers = set(manifest["public_headers"]["ordinary"]) | {
        manifest["public_headers"]["explicit_interop"]
    }
    ng_catalog = load_yaml(root / "schemas/cxxlens_ng_public_api_catalog.yaml")
    declared_ng_headers = {
        header
        for package in ng_catalog["packages"]
        for header in package["headers"]
    } | {header for entry in ng_catalog["entries"] for header in entry["headers"]}
    if declared_legacy_headers & declared_ng_headers:
        fail("legacy and next-generation public header inventories overlap")
    declared_headers = declared_legacy_headers | declared_ng_headers
    if actual_headers != declared_headers:
        fail(
            "public header inventory drift: "
            f"missing={sorted(actual_headers - declared_headers)}, "
            f"stale={sorted(declared_headers - actual_headers)}"
        )

    umbrella_text = (root / manifest["public_headers"]["umbrella"]).read_text(
        encoding="utf-8"
    )
    if "<cxxlens/interop/clang.hpp>" in umbrella_text:
        fail("explicit interop must not enter the LLVM-free umbrella")
    for package in packages:
        header = package["header"]
        if package["id"] != "interop" and header not in umbrella_text:
            fail(f"umbrella does not aggregate package {package['id']}: {header}")

    for path_text in manifest["public_headers"]["ordinary"]:
        text = (root / path_text).read_text(encoding="utf-8")
        for pattern in FORBIDDEN_ORDINARY:
            if pattern.search(text):
                fail(f"ordinary public header boundary violation: {path_text}: {pattern.pattern}")

    candidate_contracts = {
        row["api_id"]: (group, row)
        for group in candidates["groups"]
        for row in group["api_contracts"]
    }
    if len(candidates["groups"]) != 9 or len(candidate_contracts) != 124:
        fail("candidate authority must contain exactly 9 groups and 124 APIs")
    high_risk_fingerprints = {
        row["issue"]: row["fingerprint"] for row in high_risk["candidate_snapshot"]
    }

    header_cache: dict[str, str] = {}
    trace_rows: list[dict[str, Any]] = []
    package_examples = {row["package"]: row for row in manifest["package_examples"]}
    if set(package_examples) != {package["id"] for package in packages}:
        fail("package examples do not cover all catalog packages exactly once")

    error_ids = registry_ids(catalog["registries"]["error_codes"])
    fact_ids = registry_ids(catalog["registries"]["fact_kinds"])
    capability_ids = registry_ids(catalog["registries"]["capabilities"])
    ownership_ids = {
        "public_types": {row["id"] for row in ownership["public_types"]},
        "shared_components": {row["id"] for row in ownership["shared_components"]},
        "providers": {row["id"] for row in ownership["providers"]},
        "schemas": {row["id"] for row in ownership["schemas"]},
    }
    if any(len(values) != len(set(values)) for values in ownership_ids.values()):
        fail("ownership registries contain duplicate IDs")

    candidate_issues = {group["issue"]: group for group in candidates["groups"]}
    for issue, group in candidate_issues.items():
        if high_risk_fingerprints.get(issue) != group["candidate_fingerprint"]:
            fail(f"high-risk fingerprint drift for {issue}")
        expected_doc = f"docs/design/package_contract_{issue.removeprefix('#')}.md"
        if group["normative_document"] != expected_doc or not (root / expected_doc).is_file():
            fail(f"package normative document drift for {issue}")

    for package, api in apis:
        api_id = api["id"]
        declaration = api["declaration"]
        if declaration["status"] != "exact":
            fail(f"{api_id}: declaration is not exact")
        if not declaration["source"].startswith("include/cxxlens/"):
            fail(f"{api_id}: declaration is not installed: {declaration['source']}")
        if declaration["signature_fingerprint"] != signature_digest(declaration["signature"]):
            fail(f"{api_id}: signature fingerprint mismatch")
        source = root / declaration["source"]
        if not source.is_file():
            fail(f"{api_id}: declaration source is missing")
        if declaration["source"] not in header_cache:
            header_cache[declaration["source"]] = expanded_header(root, source)
        header_text = header_cache[declaration["source"]]
        members = [row.strip() for row in declaration["signature"].split(";") if row.strip()]
        for member in members:
            match = SIGNATURE_NAME.search(member)
            if not match or not re.search(rf"\b{re.escape(match.group(1))}\s*\(", header_text):
                fail(f"{api_id}: declaration member is absent from public header: {member}")

        if api_id not in candidate_contracts:
            fail(f"{api_id}: no candidate trace contract")
        group, contract = candidate_contracts[api_id]
        if any(
            contract["declaration"][field] != declaration[field]
            for field in ("source", "signature", "signature_fingerprint")
        ):
            fail(f"{api_id}: catalog/candidate declaration drift")
        if not contract["traceability"]["doxygen_obligation"]:
            fail(f"{api_id}: Doxygen obligation is empty")
        trace = contract["traceability"]["requirements_or_use_cases"]
        if not trace:
            fail(f"{api_id}: requirement/use-case trace is empty")
        for error_id in api.get("errors", []):
            if error_id not in error_ids:
                fail(f"{api_id}: dangling error registry reference: {error_id}")
        requirements = api.get("requires", {})
        for fact_id in requirements.get("facts", []):
            if fact_id not in fact_ids:
                fail(f"{api_id}: dangling fact registry reference: {fact_id}")
        for capability_id in requirements.get("capabilities", []):
            if capability_id not in capability_ids:
                fail(f"{api_id}: dangling capability registry reference: {capability_id}")
        for key, known in ownership_ids.items():
            dangling = set(contract["ownership_refs"][key]) - known
            if dangling:
                fail(f"{api_id}: dangling {key}: {sorted(dangling)}")
        acceptance = contract["acceptance"]
        if set(acceptance) != {"positive", "negative", "ambiguous"}:
            fail(f"{api_id}: incomplete acceptance scenarios")

        trace_rows.append(
            {
                "api_id": api_id,
                "package": package["id"],
                "issue": group["issue"],
                "requirements_or_use_cases": trace,
                "declaration": declaration["source"],
                "schemas": contract["ownership_refs"]["schemas"],
                "doxygen": group["normative_document"],
                "example": package_examples[package["id"]]["source"],
                "compile_test": "phase-b-header-and-example-matrix",
                "install_test": "tests/install/run_install_test.cmake.in",
                "atomic_unit": api["atomic_unit"]["id"],
            }
        )

    for example in manifest["package_examples"]:
        source = root / example["source"]
        if not source.is_file():
            fail(f"missing package example: {source}")
        text = source.read_text(encoding="utf-8")
        if example["header"] not in text:
            fail(f"{example['package']}: example does not include catalog header")
        for scenario in example["scenarios"]:
            if scenario not in text:
                fail(f"{example['package']}: example omits {scenario} scenario")

    if (
        conventions["contract_state"] not in {"candidate", "frozen"}
        or high_risk["gate"]["state"] != "validated"
    ):
        fail("Phase B integration requires candidate/frozen conventions and validated #52 gate")
    package_states = {package["contract"]["state"] for package in packages}
    if package_states not in ({"candidate"}, {"frozen"}):
        fail("Phase B integration rejects mixed or unsupported contract states")
    if package_states == {"frozen"} and any(
        package["contract"]["transition_issue"] != "#54" for package in packages
    ):
        fail("frozen package contracts must be authorized by #54")

    return manifest, {
        "counts": actual_counts,
        "traceability": trace_rows,
        "registry_counts": {
            "errors": len(error_ids),
            "facts": len(fact_ids),
            "capabilities": len(capability_ids),
            "public_types": len(ownership_ids["public_types"]),
            "shared_components": len(ownership_ids["shared_components"]),
            "providers": len(ownership_ids["providers"]),
            "schemas": len(ownership_ids["schemas"]),
        },
        "high_risk_semantic_digest": high_risk["evidence"]["semantic_digest"],
    }


def compiler_info(compiler: str) -> dict[str, str]:
    result = subprocess.run([compiler, "--version"], check=False, text=True, capture_output=True)
    if result.returncode != 0:
        fail(f"compiler is unavailable: {compiler}: {result.stderr.strip()}")
    first_line = result.stdout.splitlines()[0]
    lower = first_line.lower()
    if "clang" in lower:
        family = "Clang"
    elif "gcc" in lower or "g++" in lower:
        family = "GCC"
    else:
        fail(f"unsupported compiler family: {first_line}")
    return {"path": os.path.realpath(compiler), "family": family, "version": first_line}


def compile_matrix(
    root: pathlib.Path,
    manifest: dict[str, Any],
    include_dir: pathlib.Path,
    compilers: list[str],
) -> dict[str, Any]:
    infos = [compiler_info(compiler) for compiler in compilers]
    if {info["family"] for info in infos} != {"GCC", "Clang"}:
        fail("compile matrix requires at least one GCC and one Clang compiler")
    headers = [*manifest["public_headers"]["ordinary"], manifest["public_headers"]["explicit_interop"]]
    includes = [path.removeprefix("include/") for path in manifest["public_headers"]["ordinary"]]
    examples = [root / row["source"] for row in manifest["package_examples"]]
    jobs: list[tuple[str, str, str]] = []
    with tempfile.TemporaryDirectory(prefix="cxxlens-phase-b-") as temporary:
        temporary_root = pathlib.Path(temporary)
        for index, header in enumerate(headers):
            source = temporary_root / f"header-{index}.cpp"
            source.write_text(f"#include <{header.removeprefix('include/')}>\nint main(){{return 0;}}\n")
            for compiler in compilers:
                jobs.append((compiler, str(source), "syntax"))

        permutations = {
            "forward": includes,
            "reverse": list(reversed(includes)),
            "repeated": [manifest["public_headers"]["umbrella"].removeprefix("include/")] * 2,
        }
        for name, order in permutations.items():
            source = temporary_root / f"order-{name}.cpp"
            source.write_text("".join(f"#include <{header}>\n" for header in order) + "int main(){return 0;}\n")
            for compiler in compilers:
                jobs.append((compiler, str(source), "syntax"))

        for example in examples:
            for compiler in compilers:
                jobs.append((compiler, str(example), "link"))

        def run(job: tuple[str, str, str]) -> tuple[tuple[str, str, str], subprocess.CompletedProcess[str]]:
            compiler, source, mode = job
            command = [
                compiler,
                "-std=c++23",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Werror",
                f"-I{include_dir}",
                source,
            ]
            if mode == "syntax":
                command.insert(-1, "-fsyntax-only")
            else:
                command.extend(["-o", str(temporary_root / (pathlib.Path(source).stem + "-" + pathlib.Path(compiler).name))])
            return job, subprocess.run(command, check=False, text=True, capture_output=True)

        with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, len(jobs))) as executor:
            for job, result in executor.map(run, jobs):
                if result.returncode != 0:
                    fail(
                        f"compile matrix failed ({job[0]}, {job[2]}, {job[1]}):\n"
                        f"{result.stdout}{result.stderr}"
                    )

    return {
        "include_dir": str(include_dir.resolve()),
        "compilers": infos,
        "header_translation_units": len(headers) * len(compilers),
        "include_order_translation_units": 3 * len(compilers),
        "linked_package_examples": len(examples) * len(compilers),
    }


def build_report(
    root: pathlib.Path,
    manifest: dict[str, Any],
    metadata: dict[str, Any],
    matrix: dict[str, Any] | None,
) -> dict[str, Any]:
    inputs = sorted(
        set(manifest["evidence_inputs"])
        | set(manifest["public_headers"]["ordinary"])
        | {manifest["public_headers"]["explicit_interop"]}
        | {row["source"] for row in manifest["package_examples"]}
        | set(manifest["authorities"].values())
    )
    input_digests = {path: file_digest(root / path) for path in inputs}
    return {
        "schema": manifest["report"]["schema"],
        "result": "green",
        "source_identity": git_identity(root),
        "input_digest": digest(input_digests),
        "input_digests": input_digests,
        "counts": metadata["counts"],
        "registry_counts": metadata["registry_counts"],
        "high_risk_semantic_digest": metadata["high_risk_semantic_digest"],
        "traceability_complete": len(metadata["traceability"]) == 124,
        "traceability": metadata["traceability"],
        "compile_matrix": matrix,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--include-dir", type=pathlib.Path)
    parser.add_argument("--compiler", action="append", default=[])
    parser.add_argument("--metadata-only", action="store_true")
    parser.add_argument("--require-clean", action="store_true")
    parser.add_argument("--report", type=pathlib.Path)
    args = parser.parse_args()

    root = args.root.resolve()
    manifest, metadata = validate_metadata(root)
    matrix = None
    if not args.metadata_only:
        if not args.compiler:
            fail("compile matrix requires --compiler for GCC and Clang")
        include_dir = (args.include_dir or root / "include").resolve()
        if not include_dir.is_dir():
            fail(f"include directory is missing: {include_dir}")
        matrix = compile_matrix(root, manifest, include_dir, args.compiler)
    report = build_report(root, manifest, metadata, matrix)
    if args.require_clean and not report["source_identity"]["worktree_clean"]:
        fail("commit-bound report requires a clean worktree")
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    matrix_summary = "metadata-only" if matrix is None else (
        f"{matrix['header_translation_units']} header TUs, "
        f"{matrix['include_order_translation_units']} order TUs, "
        f"{matrix['linked_package_examples']} linked examples"
    )
    print(
        "validated Phase B contract integration: "
        f"{metadata['counts']['packages']} packages, {metadata['counts']['apis']} APIs, "
        f"{metadata['counts']['atomic_units']} atomic units; {matrix_summary}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (IntegrationError, OSError, yaml.YAMLError) as error:
        print(f"Phase B contract integration failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
