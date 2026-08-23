#!/usr/bin/env python3
"""Focused product tests for the SDK doctor resolver and its projections."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any


USE_CASE = "cxxlens.clang22.materialize-and-query.v1"


def run(executable: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [executable, *arguments], capture_output=True, text=True, check=False
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"check_sdk_doctor_product: {message}")


def valid_project(*, provider: bool = True) -> dict[str, Any]:
    project: dict[str, Any] = {
        "project_id": "project.example",
        "catalog_id": "catalog.example",
        "catalog_digest": "catalog-digest",
        "logical_root": "project://example",
        "environment_digest": "environment-digest",
        "source_input": {
            "source_snapshot_id": "snapshot.example",
            "compilation_database_id": "compdb.example",
        },
        "store": {"backend": "memory", "format": "cxxlens.snapshot.v3"},
    }
    if provider:
        project["provider"] = {
            "provider_id": "provider.example",
            "provider_version": "2.0.0",
            "protocol_major": 2,
            "protocol_minor": 0,
            "offered_relations": ["cc.call_site.v1", "cc.entity.v1"],
            "features": ["task-source-closure-v2"],
            "interpretation_domains": ["cc.canonical-1"],
            "sandbox_minimum": "enforced",
        }
    return {
        "schema": "cxxlens.sdk-doctor-project.v1",
        "document_version": "1.0.0",
        "project": project,
    }


def write_project(directory: pathlib.Path, name: str, document: Any) -> pathlib.Path:
    path = directory / name
    path.write_text(json.dumps(document, ensure_ascii=True, separators=(",", ":")), encoding="utf-8")
    return path


def check_proved_and_deterministic(executable: str, directory: pathlib.Path) -> None:
    first = write_project(directory, "first.json", valid_project())
    reordered = valid_project()
    reordered["project"] = {
        "store": reordered["project"]["store"],
        "provider": reordered["project"]["provider"],
        "source_input": reordered["project"]["source_input"],
        "environment_digest": reordered["project"]["environment_digest"],
        "logical_root": reordered["project"]["logical_root"],
        "catalog_digest": reordered["project"]["catalog_digest"],
        "catalog_id": reordered["project"]["catalog_id"],
        "project_id": reordered["project"]["project_id"],
    }
    second = write_project(directory, "second.json", reordered)
    first_run = run(executable, "missing", "--project", str(first), "--use-case", USE_CASE)
    second_run = run(executable, "missing", "--project", str(second), "--use-case", USE_CASE)
    require(first_run.returncode == 0, f"proved resolver exited {first_run.returncode}: {first_run.stderr}")
    require(second_run.returncode == 0, f"reordered resolver exited {second_run.returncode}: {second_run.stderr}")
    require(first_run.stdout == second_run.stdout, "JSON projection depends on input object order")
    report = json.loads(first_run.stdout)
    require(report["schema"] == "cxxlens.sdk-doctor-resolution.v1", "resolution schema mismatch")
    require(report["result"]["state"] == "proved", "valid context was not proved")
    require(report["missing"] == [], "proved context unexpectedly has missing capabilities")
    require(report["completion_plan"] == [], "proved context unexpectedly has a completion plan")
    require(report["preserved_semantics"]["unresolved"] == [], "proved context has unresolved capabilities")
    markdown_one = run(
        executable,
        "missing",
        "--project",
        str(first),
        "--use-case",
        USE_CASE,
        "--format",
        "markdown",
    )
    markdown_two = run(
        executable,
        "missing",
        "--project",
        str(first),
        "--use-case",
        USE_CASE,
        "--format",
        "markdown",
    )
    require(markdown_one.returncode == 0 and markdown_two.returncode == 0, "markdown projection failed")
    require(markdown_one.stdout == markdown_two.stdout, "markdown projection is not deterministic")
    require(markdown_one.stdout.startswith("# cxxlens SDK capability diagnosis\n"), "markdown heading missing")


def check_missing_and_completion_plan(executable: str, directory: pathlib.Path) -> None:
    missing_provider = write_project(directory, "missing-provider.json", valid_project(provider=False))
    completed = run(executable, "missing", "--project", str(missing_provider), "--use-case", USE_CASE)
    require(completed.returncode == 1, f"missing provider exit code was {completed.returncode}")
    report = json.loads(completed.stdout)
    require(report["result"]["state"] == "partial", "missing provider did not remain partial")
    require(report["result"]["reason_code"] == "doctor.missing-capability", "missing provider reason changed")
    missing_ids = {item["capability_id"] for item in report["missing"]}
    require("provider.protocol.v2" in missing_ids, "provider protocol was not diagnosed")
    plan_ids = {item["unlocks"] for item in report["completion_plan"]}
    require("provider.protocol.v2" in plan_ids, "completion plan omits provider protocol")
    require(report["preserved_semantics"]["unresolved"], "unknown capability was collapsed")

    # Completion steps are emitted in the capability DAG's declared order.  A
    # consumer can therefore apply the plan without reimplementing a planner
    # or depending on input-object ordering.
    path_positions = {
        item["id"]: position for position, item in enumerate(report["capability_path"])
    }
    plan_positions = [path_positions[item["unlocks"]] for item in report["completion_plan"]]
    require(plan_positions == sorted(plan_positions), "completion plan is not dependency ordered")
    for item in report["completion_plan"]:
        unlock_position = path_positions[item["unlocks"]]
        require(
            all(path_positions[dependency] < unlock_position for dependency in item["requires"]),
            f"completion step {item['unlocks']} depends on a later capability",
        )

    unsupported_store = valid_project()
    unsupported_store["project"]["store"] = {"backend": "filesystem", "format": "old.snapshot"}
    unsupported = write_project(directory, "unsupported-store.json", unsupported_store)
    completed = run(executable, "missing", "--project", str(unsupported), "--use-case", USE_CASE)
    require(completed.returncode == 1, "unsupported store should not be successful")
    report = json.loads(completed.stdout)
    store = next(item for item in report["capability_path"] if item["id"] == "store.snapshot.v3")
    require(store["state"] == "disproved", "unsupported store was not disproved")
    require(store["reason_code"] == "doctor.unsupported-tuple", "unsupported store reason changed")

    unsupported_protocol = valid_project()
    unsupported_protocol["project"]["provider"]["protocol_major"] = 1
    protocol_path = write_project(directory, "unsupported-protocol.json", unsupported_protocol)
    completed = run(executable, "missing", "--project", str(protocol_path), "--use-case", USE_CASE)
    require(completed.returncode == 1, "unsupported protocol should not be successful")
    report = json.loads(completed.stdout)
    require(report["result"]["state"] == "disproved", "unsupported protocol was not disproved")
    protocol = next(
        item for item in report["capability_path"] if item["id"] == "provider.protocol.v2"
    )
    require(
        protocol["state"] == "disproved" and protocol["reason_code"] == "doctor.unsupported-tuple",
        "unsupported protocol reason changed",
    )
    closure = next(
        item for item in report["capability_path"] if item["id"] == "provider.source-closure.v1"
    )
    require(
        closure["state"] == "disproved"
        and closure["reason_code"] == "doctor.disproved-dependency",
        "disproved dependency was collapsed to unknown",
    )


def check_strict_and_fault_inputs(executable: str, directory: pathlib.Path) -> None:
    duplicate = (
        '{"schema":"cxxlens.sdk-doctor-project.v1","document_version":"1.0.0",'
        '"project":{"project_id":"one","project_id":"two"}}'
    )
    duplicate_path = directory / "duplicate.json"
    duplicate_path.write_text(duplicate, encoding="utf-8")
    completed = run(executable, "missing", "--project", str(duplicate_path), "--use-case", USE_CASE)
    require(completed.returncode == 2, "duplicate project field did not fail closed")
    require("doctor.project-invalid" in completed.stderr and "duplicate-key" in completed.stderr, "duplicate reason missing")

    unknown = valid_project()
    unknown["project"]["unexpected"] = True
    unknown_path = write_project(directory, "unknown.json", unknown)
    completed = run(executable, "missing", "--project", str(unknown_path), "--use-case", USE_CASE)
    require(completed.returncode == 2, "unknown project field did not fail closed")
    require("unknown-field" in completed.stderr, "unknown-field reason missing")

    invalid_utf8 = directory / "invalid-utf8.json"
    invalid_utf8.write_bytes(
        b'{"schema":"cxxlens.sdk-doctor-project.v1","document_version":"1.0.0",'
        b'"project":{"project_id":"\xff"}}'
    )
    completed_bytes = subprocess.run(
        [executable, "missing", "--project", str(invalid_utf8), "--use-case", USE_CASE],
        capture_output=True,
        check=False,
    )
    require(completed_bytes.returncode == 2, "invalid UTF-8 did not fail closed")
    require(b"invalid-utf8" in completed_bytes.stderr, "invalid UTF-8 reason missing")

    completed = run(executable, "missing", "--use-case", USE_CASE)
    require(completed.returncode == 2 and "doctor.project-required" in completed.stderr, "missing project option was not typed")

    oversized = directory / "oversized.json"
    oversized.write_text("{" + "\"x\":" + ("\"value\"," * 250000)[:-1] + "}", encoding="utf-8")
    completed = run(executable, "missing", "--project", str(oversized), "--use-case", USE_CASE)
    require(completed.returncode == 2, "oversized project was not rejected")
    require("doctor.project-invalid" in completed.stderr and "byte-limit" in completed.stderr,
            "oversized project did not report the resource bound")

    too_deep = directory / "too-deep.json"
    too_deep.write_text("[" * 65 + "0" + "]" * 65, encoding="utf-8")
    completed = run(executable, "missing", "--project", str(too_deep), "--use-case", USE_CASE)
    require(completed.returncode == 2, "deep project was not rejected")
    require("doctor.project-invalid" in completed.stderr and "depth-limit" in completed.stderr,
            "deep project did not report the parser resource bound")


def check_relation_presence(executable: str) -> None:
    for legacy_command in ("inspect", "doctor", "query-ir", "provider-manifest"):
        completed = run(executable, legacy_command)
        require(
            completed.returncode == 2 and completed.stdout == "",
            f"legacy command {legacy_command} was still accepted",
        )

    completed = run(
        executable,
        "relation-presence",
        "build.project.v1",
        "cc.call_site.v1",
        "source.span.v1",
    )
    require(completed.returncode == 0, f"relation presence exited {completed.returncode}: {completed.stderr}")
    report = json.loads(completed.stdout)
    require(report["schema"] == "cxxlens.sdk-doctor-relation-presence.v1", "relation schema mismatch")
    require(report["state"] == "proved" and report["missing"] == 0, "known relations were not proved")

    completed = run(executable, "relation-presence", "cc.call_site.v1", "cc.does_not_exist.v1")
    require(completed.returncode == 1, "unknown relation should return incomplete")
    report = json.loads(completed.stdout)
    absent = next(item for item in report["components"] if item["id"] == "cc.does_not_exist.v1")
    require(absent["state"] == "unknown" and absent["reason_code"] == "sdk.relation-not-found", "unknown relation reason changed")

    completed = run(executable, "relation-presence", "cc.call_site.v2")
    require(completed.returncode == 1, "relation major mismatch should be incomplete")
    require(json.loads(completed.stdout)["components"][0]["reason_code"] == "sdk.relation-major-mismatch", "major mismatch reason changed")

    completed = run(executable, "relation-presence", "cc.call_site.v1", "cc.call_site.v1")
    require(completed.returncode == 0 and json.loads(completed.stdout)["requested"] == 2, "duplicate relation requests were not projected independently")

    completed = run(executable, "relation-presence", "cc.call_site.vX")
    require(completed.returncode == 2 and completed.stdout == "", "malformed relation ID did not fail closed")
    require("doctor.relation-request-invalid" in completed.stderr, "malformed relation reason missing")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cxxlens-sdk-doctor-product-") as raw_directory:
        directory = pathlib.Path(raw_directory)
        check_proved_and_deterministic(args.executable, directory)
        check_missing_and_completion_plan(args.executable, directory)
        check_strict_and_fault_inputs(args.executable, directory)
    check_relation_presence(args.executable)
    return 0


if __name__ == "__main__":
    sys.exit(main())
