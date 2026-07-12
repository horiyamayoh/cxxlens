#!/usr/bin/env python3
"""Enforce issue #21 incremental workspace provisioning evidence."""

from __future__ import annotations

import pathlib
import sys

import yaml


root = pathlib.Path(sys.argv[1])
catalog = yaml.safe_load((root / "schemas/cxxlens_public_api_contract.yaml").read_text())
workspace = next(package for package in catalog["packages"] if package["id"] == "workspace")
apis = {api["id"]: api for api in workspace["apis"]}
failures: list[str] = []
for api_id in ("API-WS-005", "API-WS-006", "API-WS-007"):
    if apis[api_id]["implementation_state"] != "conformant":
        failures.append(f"{api_id} is not conformant")
    if apis[api_id]["readiness"]["state"] != "complete":
        failures.append(f"{api_id} is not complete")

implementation = (root / "src/workspace/provisioning.cpp").read_text()
tests = (root / "tests/unit/workspace/provisioning_test.cpp").read_text()
for required in (
    "cxxlens.fact-requirement-input.v1",
    "coverage_state::unresolved",
    "reduce_observations",
    "transaction.value()->stage",
    "transaction.value()->validate",
    "transaction.value()->commit",
    "core.capability-unavailable",
    "workspace.frontend-unavailable",
):
    if required not in implementation:
        failures.append(f"provisioning invariant branch missing: {required}")
for fixture in (
    "warm ensure scheduled frontend work",
    "source change did not invalidate only its two variant tasks",
    "one variant failure did not preserve successful facts",
    "cancellation corrupted the prior immutable snapshot",
    "mid-operation cancellation committed a partial snapshot",
    "deadline exhaustion corrupted the prior immutable snapshot",
    "budget exhaustion corrupted the prior immutable snapshot",
    "unsupported capability became an empty success",
    "invalid worker/reducer input committed a snapshot",
    "SQLite warm reopen did not restore facts and coverage without parsing",
    "root relocation invalidated or changed a compatible SQLite snapshot",
    "jobs 1/2/8 or relocated root changed semantic facts/coverage",
    "memory and SQLite backends changed semantic facts/coverage",
    "configuration change did not invalidate exactly the dependent closure",
    "one command change did not invalidate exactly its variant closure",
    "real frontend/extractor/reducer/store provisioning pipeline failed",
    "unavailable production frontend became empty success",
):
    if fixture not in tests:
        failures.append(f"provisioning acceptance fixture missing: {fixture}")
for schema in (
    "cxxlens_fact_requirement.schema.yaml",
    "cxxlens_coverage_delta.schema.yaml",
    "cxxlens_provisioning_trace.schema.yaml",
    "cxxlens_workspace_doctor.schema.yaml",
    "cxxlens_capabilities.schema.yaml",
    "cxxlens_incremental_provisioning_contract.yaml",
):
    if not (root / "schemas" / schema).is_file():
        failures.append(f"provisioning schema missing: {schema}")

if failures:
    print("provisioning contract check failed:\n" + "\n".join(failures), file=sys.stderr)
    raise SystemExit(1)
print("validated incremental requirements, coverage, transaction, doctor, and API evidence")
