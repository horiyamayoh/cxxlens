#!/usr/bin/env python3
"""Run the installed-package M2 flagship production-path acceptance matrix."""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
from typing import Any

import jsonschema
import yaml


HEADER = """#pragma once
struct Base { virtual int step(); };
struct Derived final : Base { int step() override; };
struct Other { int step(); };
#define INVOKE_STEP(value) (value).step()
"""
MAIN = """#include "api.hpp"
int invoke(Base& base, Derived& derived, Other& other) {
  return base.step() + derived.step() + other.step();
}
int macro_invoke(Base& base) { return INVOKE_STEP(base); }
"""
SUPPORT = """#include "api.hpp"
int Base::step() { return MODE; }
int Derived::step() { return MODE + 1; }
int Other::step() { return MODE + 2; }
"""
BROKEN = '#include "api.hpp"\nint broken( { return 0; }\n'


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def run(
    command: list[str],
    *,
    cwd: pathlib.Path,
    seed: int = 0,
    path_prefix: pathlib.Path | None = None,
) -> bytes:
    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "CXXLENS_TEST_SEED": str(seed)})
    if path_prefix is not None:
        inherited_path = environment.get("PATH", "")
        environment["PATH"] = (
            os.pathsep.join((str(path_prefix), inherited_path))
            if inherited_path
            else str(path_prefix)
        )
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=environment,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        stderr = completed.stderr.decode("utf-8", errors="replace")
        stdout = completed.stdout.decode("utf-8", errors="replace")
        rendered = " ".join(command)
        raise RuntimeError(
            f"command failed ({completed.returncode}): {rendered}\n"
            f"stdout:\n{stdout}\nstderr:\n{stderr}"
        )
    return completed.stdout


def cmake_cache_value(build: pathlib.Path, key: str) -> str | None:
    prefix = f"{key}:PATH="
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix):
            return line.removeprefix(prefix)
    return None


def build_installed_example(
    root: pathlib.Path,
    build: pathlib.Path,
    source: pathlib.Path,
    compiler: str,
    temporary: pathlib.Path,
) -> tuple[pathlib.Path, pathlib.Path]:
    prefix = temporary / "prefix"
    example_build = temporary / "example-build"
    run(["cmake", "--install", str(build), "--prefix", str(prefix)], cwd=root)
    command = [
        "cmake",
        "-S",
        str(source),
        "-B",
        str(example_build),
        f"-DCMAKE_PREFIX_PATH={prefix}",
        f"-DCMAKE_CXX_COMPILER={compiler}",
    ]
    for key in ("LLVM_DIR", "Clang_DIR"):
        value = cmake_cache_value(build, key)
        if value:
            command.append(f"-D{key}={value}")
    run(command, cwd=temporary)
    run(["cmake", "--build", str(example_build)], cwd=temporary)
    executable = example_build / "cxxlens-m2-flagship"
    if not executable.exists():
        raise AssertionError("installed flagship example executable is missing")
    worker = prefix / "bin" / "cxxlens-frontend-worker"
    if not worker.is_file() or not os.access(worker, os.X_OK):
        raise AssertionError("installed frontend worker executable is missing")
    return executable, worker.parent


def prepare_fixture(
    root: pathlib.Path,
    order: str,
    compiler: str,
    partial: bool = False,
) -> None:
    root.mkdir(parents=True, exist_ok=True)
    files = [("api.hpp", HEADER), ("main.cpp", MAIN), ("support.cpp", SUPPORT)]
    if partial:
        files.append(("broken.cpp", BROKEN))
    if order == "reverse":
        files.reverse()
    for name, content in files:
        (root / name).write_text(content, encoding="utf-8")
    sources = ["main.cpp", "support.cpp"] + (["broken.cpp"] if partial else [])
    commands: list[dict[str, Any]] = []
    for source, (variant, mode) in itertools.product(
        sources, (("mode-one", "1"), ("mode-two", "2"))
    ):
        commands.append(
            {
                "directory": str(root),
                "file": str(root / source),
                "output": str(root / f"{source}.{variant}.o"),
                "arguments": [
                    compiler,
                    "-std=c++23",
                    f"-DMODE={mode}",
                    "-I",
                    str(root),
                    "-c",
                    str(root / source),
                ],
            }
        )
    if order == "reverse":
        commands.reverse()
    (root / "compile_commands.json").write_text(
        json.dumps(commands, ensure_ascii=False, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )


def validate_manifest(root: pathlib.Path, build: pathlib.Path, manifest: dict[str, Any]) -> None:
    schema = yaml.safe_load((root / "schemas/cxxlens_m2_completion.schema.yaml").read_text())
    jsonschema.validate(manifest, schema)
    catalog = yaml.safe_load((root / "schemas/cxxlens_public_api_contract.yaml").read_text())
    exact_m2 = sorted(
        api["id"]
        for package in catalog["packages"]
        for api in package["apis"]
        if api["phase"] == "M2" and api["declaration"]["status"] == "exact"
    )
    # Exact milestone coverage comprises both implemented and explicitly
    # deferred catalog entries; the completion audit validates each partition.
    manifest_coverage = sorted(
        manifest["conformant_catalog_ids"]
        + [entry["id"] for entry in manifest["deferred_catalog"]]
    )
    if exact_m2 != manifest_coverage:
        raise AssertionError("M2 exact catalog signature coverage changed")
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


def validate_public_documents(root: pathlib.Path, output: bytes) -> list[dict[str, Any]]:
    documents = [json.loads(line) for line in output.decode().splitlines()]
    schema_names = [
        "cxxlens_search_report.schema.yaml",
        "cxxlens_search_report.schema.yaml",
        "cxxlens_explanation.schema.yaml",
        "cxxlens_explanation.schema.yaml",
        "cxxlens_agent_task_card.schema.yaml",
    ]
    if len(documents) != len(schema_names):
        raise AssertionError("installed example emitted an unexpected artifact count")
    for document, name in zip(documents, schema_names, strict=True):
        schema = yaml.safe_load((root / "schemas" / name).read_text())
        jsonschema.Draft202012Validator(schema).validate(document)
    return documents


def run_matrix(
    temporary: pathlib.Path,
    manifest: dict[str, Any],
    example: pathlib.Path,
    scheduler: str,
    compiler: str,
    worker_directory: pathlib.Path,
) -> tuple[int, bytes, bytes]:
    axes = manifest["matrix"]
    baseline: bytes | None = None
    baseline_example = b""
    baseline_scheduler = b""
    executions = 0
    dimensions = itertools.product(
        axes["jobs"],
        axes["scheduler_seeds"],
        axes["orders"],
        axes["roots"],
        range(axes["repeats"]),
        axes["backends"],
    )
    for jobs, seed, order, root_name, repeat, backend in dimensions:
        case = temporary / f"j{jobs}-s{seed}-{order}-r{repeat}-{backend}" / root_name
        prepare_fixture(case, order, compiler)
        scheduler_output = run(
            [scheduler, "--emit", str(jobs), str(seed), order], cwd=case, seed=seed
        )
        for cache_state in axes["cache_states"]:
            if cache_state == "cold" and backend == "sqlite":
                shutil.rmtree(case / ".cxxlens-cache", ignore_errors=True)
            example_output = run(
                [str(example), str(case), backend, str(jobs), "normal", "json"],
                cwd=case,
                seed=seed,
                path_prefix=worker_directory,
            )
            combined = b"example\0" + example_output + b"scheduler\0" + scheduler_output
            if baseline is None:
                baseline = combined
                baseline_example = example_output
                baseline_scheduler = scheduler_output
            elif combined != baseline:
                raise AssertionError(
                    "M2 installed semantic divergence: "
                    f"jobs={jobs} seed={seed} order={order} root={root_name} "
                    f"repeat={repeat} backend={backend} cache={cache_state} "
                    f"actual={digest(combined)} expected={digest(baseline)}"
                )
            executions += 1
    if executions != axes["expected_executions"]:
        raise AssertionError("M2 matrix did not execute every declared combination")
    return executions, baseline_example, baseline_scheduler


def performance_trace(
    root: pathlib.Path,
    integration: str,
    limits: dict[str, int],
    worker_directory: pathlib.Path,
) -> tuple[dict[str, int], bytes]:
    output = run(
        [integration, "--emit", "1", "forward"],
        cwd=root,
        path_prefix=worker_directory,
    )
    documents = [json.loads(line) for line in output.decode().splitlines()]
    if len(documents) != 5:
        raise AssertionError("M2 integration trace emitted an unexpected artifact count")
    cold, warm, query = documents[2], documents[3], documents[4]
    performance = {
        "cold_scheduled_tasks": cold["scheduled"],
        "warm_scheduled_tasks": warm["scheduled"],
        "fact_candidates": query["accounting"]["considered"],
        "refinements": query["refinements_requested"],
    }
    comparisons = {
        "cold_scheduled_tasks": "maximum_cold_scheduled_tasks",
        "warm_scheduled_tasks": "maximum_warm_scheduled_tasks",
        "fact_candidates": "maximum_fact_candidates",
        "refinements": "maximum_refinements",
    }
    for field, limit in comparisons.items():
        if performance[field] > limits[limit]:
            raise AssertionError(f"M2 performance trace exceeded {limit}")
    return performance, output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--report", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--integration", required=True)
    parser.add_argument("--scheduler", required=True)
    parser.add_argument("--example-source", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.root = args.root.resolve()
    args.build = args.build.resolve()
    build_worker = args.build / "cxxlens-frontend-worker"
    if not build_worker.is_file() or not os.access(build_worker, os.X_OK):
        raise AssertionError("build-tree frontend worker executable is missing")
    args.integration = str(pathlib.Path(args.integration).resolve())
    args.scheduler = str(pathlib.Path(args.scheduler).resolve())
    manifest = yaml.safe_load(args.manifest.read_text(encoding="utf-8"))
    validate_manifest(args.root, args.build, manifest)
    with tempfile.TemporaryDirectory(prefix="cxxlens-m2-acceptance-") as directory:
        temporary = pathlib.Path(directory)
        example, installed_worker_directory = build_installed_example(
            args.root, args.build, args.example_source.resolve(), args.compiler, temporary
        )
        executions, installed_output, scheduler_output = run_matrix(
            temporary,
            manifest,
            example,
            args.scheduler,
            args.compiler,
            installed_worker_directory,
        )
        documents = validate_public_documents(args.root, installed_output)
        if len(documents[0]["matches"]) != 2 or documents[0]["guarantee"] == "exact_within_coverage":
            raise AssertionError("installed flagship result count or open-world guarantee changed")

        markdown_root = temporary / "markdown" / "workspace-a"
        prepare_fixture(markdown_root, "forward", args.compiler)
        markdown = run(
            [str(example), str(markdown_root), "memory", "1", "normal", "markdown"],
            cwd=markdown_root,
            path_prefix=installed_worker_directory,
        )
        if b"Matches: 2" not in markdown or b"search.open-world-virtual-target" not in markdown:
            raise AssertionError("installed Markdown projection disagrees with canonical JSON")

        partial_root = temporary / "partial" / "workspace-a"
        prepare_fixture(partial_root, "forward", args.compiler, partial=True)
        partial_output = run(
            [str(example), str(partial_root), "memory", "2", "partial", "json"],
            cwd=partial_root,
            path_prefix=installed_worker_directory,
        )
        partial_documents = validate_public_documents(args.root, partial_output)
        if partial_documents[0]["coverage"]["complete"]:
            raise AssertionError("one-TU failure became complete coverage")

        performance, integration_output = performance_trace(
            args.root,
            args.integration,
            manifest["performance_limits"],
            args.build,
        )
    report = {
        "schema": "cxxlens.m2-acceptance-report.v1",
        "status": "passed",
        "manifest": manifest["schema"],
        "catalog_api_count": len(manifest["conformant_catalog_ids"]),
        "matrix_executions": executions,
        "vectors": [
            {"id": vector["id"], "status": "passed", "fixture": vector["fixture"]}
            for vector in manifest["vectors"]
        ],
        "performance": performance,
        "artifact_digests": dict(
            sorted(
                {
                    "installed_json": digest(installed_output),
                    "installed_markdown": digest(markdown),
                    "integration_trace": digest(integration_output),
                    "partial_json": digest(partial_output),
                    "scheduler": digest(scheduler_output),
                }.items()
            )
        ),
    }
    report_schema = yaml.safe_load(
        (args.root / "schemas/cxxlens_m2_acceptance_report.schema.yaml").read_text()
    )
    jsonschema.validate(report, report_schema)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        f"M2 acceptance passed: {len(report['vectors'])} vectors, "
        f"{executions} installed matrix executions, report={args.report}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
