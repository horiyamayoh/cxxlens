#!/usr/bin/env python3
"""Run and report the M0 semantic-kernel acceptance matrix."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import tempfile
from typing import Any

import jsonschema
import yaml


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def run(command: list[str], *, cwd: pathlib.Path, environment: dict[str, str]) -> bytes:
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=environment,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return completed.stdout


def fail(vector: dict[str, Any], actual: str, expected: str, seed: int) -> None:
    raise AssertionError(
        "M0 acceptance divergence: "
        f"contract={vector['id']} catalog={','.join(vector['catalog_ids']) or 'none'} "
        f"fixture={vector['fixture']} seed={seed} actual={actual} expected={expected}"
    )


def validate_manifest(root: pathlib.Path, build: pathlib.Path, manifest: dict[str, Any]) -> None:
    schema = yaml.safe_load((root / "schemas/cxxlens_m0_completion.schema.yaml").read_text())
    jsonschema.validate(manifest, schema)
    catalog = yaml.safe_load((root / "schemas/cxxlens_public_api_contract.yaml").read_text())
    catalog_by_id = {
        api["id"]: api for package in catalog["packages"] for api in package["apis"]
    }
    conformant = manifest["conformant_catalog_ids"]
    if any(catalog_by_id[api_id]["implementation_state"] != "conformant" for api_id in conformant):
        raise AssertionError(
            "an M0 completion API is no longer conformant in the catalog"
        )
    tests = json.loads(
        subprocess.run(
            ["ctest", "--test-dir", str(build), "--show-only=json-v1"],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        ).stdout
    )
    test_names = {test["name"] for test in tests["tests"]}
    for vector in manifest["vectors"]:
        missing = set(vector["tests"]) - test_names
        if missing:
            raise AssertionError(f"{vector['id']} references missing tests: {sorted(missing)}")
        if not (root / vector["fixture"]).exists():
            raise AssertionError(f"{vector['id']} fixture is missing: {vector['fixture']}")
    headers = sorted(
        path.relative_to(root).as_posix() for path in (root / "include/cxxlens").rglob("*.hpp")
    )
    missing_headers = set(manifest["public_headers"]) - set(headers)
    if missing_headers:
        raise AssertionError(f"M0 public headers are missing: {sorted(missing_headers)}")


def compile_headers(root: pathlib.Path, compiler: str, headers: list[str]) -> None:
    source_by_header = {
        "include/cxxlens/configuration.hpp": "configuration_header_test.cpp",
        "include/cxxlens/core.hpp": "core_header_test.cpp",
        "include/cxxlens/core/evidence.hpp": "evidence_header_test.cpp",
        "include/cxxlens/core/failure.hpp": "failure_header_test.cpp",
        "include/cxxlens/core/finding.hpp": "finding_header_test.cpp",
        "include/cxxlens/core/identity.hpp": "identity_header_test.cpp",
        "include/cxxlens/core/schema.hpp": "schema_header_test.cpp",
        "include/cxxlens/cxxlens.hpp": "cxxlens_header_test.cpp",
        "include/cxxlens/source.hpp": "source_header_test.cpp",
        "include/cxxlens/testing.hpp": "testing_header_test.cpp",
    }
    for header in headers:
        source = root / "tests/public_headers" / source_by_header[header]
        subprocess.run(
            [
                compiler,
                "-std=c++23",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Werror",
                f"-I{root / 'include'}",
                "-fsyntax-only",
                str(source),
            ],
            check=True,
        )


def matrix(args: argparse.Namespace, manifest: dict[str, Any]) -> tuple[int, dict[str, str]]:
    axes = manifest["matrix"]
    baseline: bytes | None = None
    baseline_digest = ""
    executions = 0
    artifacts: dict[str, str] = {}
    vector = next(item for item in manifest["vectors"] if item["id"] == "M0-INTEGRATION")
    config_forward = (
        "schema: cxxlens.config.v1\n"
        "execution: {memory_budget_mb: 512}\n"
        "output: {deterministic: true}\n"
    )
    config_reverse = (
        "output: {deterministic: true}\n"
        "execution:\n  memory_budget_mb: 512\n"
        "schema: cxxlens.config.v1\n"
    )
    with tempfile.TemporaryDirectory(prefix="cxxlens-m0-acceptance-") as directory:
        temporary = pathlib.Path(directory)
        for jobs in axes["jobs"]:
            for seed in axes["scheduler_seeds"]:
                for order in axes["orders"]:
                    for root_index, semantic_root in enumerate(axes["roots"]):
                        for repeat in range(axes["repeats"]):
                            cwd = temporary / f"r-{root_index}-{order}-{repeat}"
                            cwd.mkdir(parents=True, exist_ok=True)
                            config = cwd / "config.yaml"
                            config.write_text(
                                config_reverse if order == "reverse" else config_forward,
                                encoding="utf-8",
                            )
                            environment = os.environ.copy()
                            environment.update(
                                {
                                    "LC_ALL": "C",
                                    "CXXLENS_TEST_JOBS": str(jobs),
                                    "CXXLENS_TEST_ORDER": order,
                                    "CXXLENS_TEST_SEED": str(seed),
                                }
                            )
                            outputs = {
                                "identity": run([args.identity, "--emit"], cwd=cwd, environment=environment),
                                "evidence": run([args.evidence, "--emit"], cwd=cwd, environment=environment),
                                "finding": run([args.finding, "--emit"], cwd=cwd, environment=environment),
                                "serialization": run([args.serialization], cwd=cwd, environment=environment),
                                "configuration": run([args.configuration, str(config)], cwd=cwd, environment=environment),
                                "fixture": run([args.fixture, semantic_root, order], cwd=cwd, environment=environment),
                            }
                            combined = b"".join(
                                name.encode("utf-8") + b"\0" + outputs[name]
                                for name in sorted(outputs)
                            )
                            actual_digest = digest(combined)
                            if baseline is None:
                                baseline = combined
                                baseline_digest = actual_digest
                                artifacts = {name: digest(value) for name, value in sorted(outputs.items())}
                            elif combined != baseline:
                                fail(vector, actual_digest, baseline_digest, seed)
                            executions += 1
    return executions, artifacts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--report", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", required=True)
    for name in ("identity", "evidence", "finding", "serialization", "configuration", "fixture"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args()
    args.root = args.root.resolve()
    args.build = args.build.resolve()
    manifest = yaml.safe_load(args.manifest.read_text(encoding="utf-8"))
    validate_manifest(args.root, args.build, manifest)
    compile_headers(args.root, args.compiler, manifest["public_headers"])
    executions, artifacts = matrix(args, manifest)
    report = {
        "schema": "cxxlens.m0-acceptance-report.v1",
        "status": "passed",
        "manifest": manifest["schema"],
        "matrix_executions": executions,
        "vectors": [
            {
                "id": vector["id"],
                "status": "passed",
                "fixture": vector["fixture"],
                "reproduction_seed": vector["reproduction_seed"],
            }
            for vector in manifest["vectors"]
        ],
        "artifact_digests": artifacts,
    }
    report_schema = yaml.safe_load(
        (args.root / "schemas/cxxlens_m0_acceptance_report.schema.yaml").read_text()
    )
    jsonschema.validate(report, report_schema)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n")
    print(
        f"M0 acceptance passed: {len(report['vectors'])} vectors, "
        f"{executions} matrix executions, report={args.report}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
