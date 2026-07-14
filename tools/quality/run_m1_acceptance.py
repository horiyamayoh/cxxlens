#!/usr/bin/env python3
"""Run and report the exact-Clang22 M1 production-path acceptance matrix."""

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


def run(
    command: list[str], *, cwd: pathlib.Path, path_prefix: pathlib.Path | None = None
) -> bytes:
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
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


def validate_manifest(root: pathlib.Path, build: pathlib.Path, manifest: dict[str, Any]) -> None:
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
    exact_m1 = sorted(
        api["id"]
        for api in catalog_by_id.values()
        if api["phase"] == "M1" and api["declaration"]["status"] == "exact"
    )
    # Exact milestone coverage comprises both implemented and explicitly
    # deferred catalog entries; the completion audit validates each partition.
    manifest_coverage = sorted(
        manifest["conformant_catalog_ids"]
        + [entry["id"] for entry in manifest["deferred_catalog"]]
    )
    if exact_m1 != manifest_coverage:
        raise AssertionError("M1 exact catalog signature coverage changed")
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


def run_matrix(
    temporary: pathlib.Path,
    manifest: dict[str, Any],
    integration: str,
    scheduler: str,
    worker_directory: pathlib.Path,
) -> tuple[int, dict[str, str]]:
    axes = manifest["matrix"]
    baseline: bytes | None = None
    baseline_digest = ""
    executions = 0
    artifact_digests: dict[str, str] = {}
    for jobs in axes["jobs"]:
        for seed in axes["scheduler_seeds"]:
            for order in axes["orders"]:
                for root_name in axes["roots"]:
                    for repeat in range(axes["repeats"]):
                        cwd = temporary / root_name / f"repeat-{repeat}"
                        cwd.mkdir(parents=True, exist_ok=True)
                        integration_output = run(
                            [integration, "--emit", str(jobs), order],
                            cwd=cwd,
                            path_prefix=worker_directory,
                        )
                        scheduler_output = run(
                            [scheduler, "--emit", str(jobs), str(seed), order], cwd=cwd
                        )
                        combined = (
                            b"integration\0"
                            + integration_output
                            + b"scheduler\0"
                            + scheduler_output
                        )
                        actual_digest = digest(combined)
                        if baseline is None:
                            baseline = combined
                            baseline_digest = actual_digest
                            artifact_digests = {
                                "integration": digest(integration_output),
                                "scheduler": digest(scheduler_output),
                            }
                        elif combined != baseline:
                            raise AssertionError(
                                "M1 semantic divergence: "
                                f"jobs={jobs} seed={seed} order={order} root={root_name} "
                                f"repeat={repeat} actual={actual_digest} expected={baseline_digest}"
                            )
                        executions += 1
    return executions, artifact_digests


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--report", type=pathlib.Path, required=True)
    parser.add_argument("--integration", required=True)
    parser.add_argument("--scheduler", required=True)
    parser.add_argument("--provisioning", required=True)
    args = parser.parse_args()
    args.root = args.root.resolve()
    args.build = args.build.resolve()
    worker = args.build / "cxxlens-frontend-worker"
    if not worker.is_file() or not os.access(worker, os.X_OK):
        raise AssertionError("build-tree frontend worker executable is missing")
    args.integration = str(pathlib.Path(args.integration).resolve())
    args.scheduler = str(pathlib.Path(args.scheduler).resolve())
    args.provisioning = str(pathlib.Path(args.provisioning).resolve())
    manifest = yaml.safe_load(args.manifest.read_text(encoding="utf-8"))
    validate_manifest(args.root, args.build, manifest)
    with tempfile.TemporaryDirectory(prefix="cxxlens-m1-acceptance-") as directory:
        temporary = pathlib.Path(directory)
        executions, artifacts = run_matrix(
            temporary, manifest, args.integration, args.scheduler, args.build
        )
        provisioning = run(
            [args.provisioning, "--emit"], cwd=temporary, path_prefix=args.build
        )
        artifacts["provisioning"] = digest(provisioning)
    report = {
        "schema": "cxxlens.m1-acceptance-report.v1",
        "status": "passed",
        "manifest": manifest["schema"],
        "catalog_api_count": len(manifest["conformant_catalog_ids"]),
        "fact_kind_count": len(manifest["fact_kinds"]),
        "matrix_executions": executions,
        "vectors": [
            {"id": vector["id"], "status": "passed", "fixture": vector["fixture"]}
            for vector in manifest["vectors"]
        ],
        "artifact_digests": dict(sorted(artifacts.items())),
    }
    report_schema = yaml.safe_load(
        (args.root / "schemas/cxxlens_m1_acceptance_report.schema.yaml").read_text()
    )
    jsonschema.validate(report, report_schema)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        f"M1 acceptance passed: {len(report['vectors'])} vectors, "
        f"{executions} matrix executions, report={args.report}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
