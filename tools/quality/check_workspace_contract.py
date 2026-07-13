#!/usr/bin/env python3
"""Enforce M1 workspace catalog ownership and safety invariants."""

from __future__ import annotations

import pathlib
import sys

import yaml


root = pathlib.Path(sys.argv[1])
catalog = yaml.safe_load((root / "schemas/cxxlens_public_api_contract.yaml").read_text())
workspace = next(package for package in catalog["packages"] if package["id"] == "workspace")
apis = {api["id"]: api for api in workspace["apis"]}
failures: list[str] = []
for api_id in ("API-WS-001", "API-WS-002", "API-WS-003", "API-WS-004", "API-WS-008"):
    if apis[api_id]["implementation_state"] != "conformant":
        failures.append(f"{api_id} is not conformant")
    if apis[api_id]["declaration"]["source"] != "include/cxxlens/workspace.hpp":
        failures.append(f"{api_id} does not own an exact workspace header signature")

header = (root / "include/cxxlens/workspace.hpp").read_text()
implementation = (root / "src/workspace/catalog.cpp").read_text()
path_mapping = (root / "src/workspace/semantic_path.hpp").read_text()
provisioning = (root / "src/workspace/provisioning.cpp").read_text()
tests = (root / "tests/unit/workspace/workspace_catalog_test.cpp").read_text()
for forbidden in ("clang/", "llvm/", "clang::", "llvm::"):
    if forbidden in header:
        failures.append(f"workspace public header leaks {forbidden}")
for forbidden in ("std::system", "popen(", "execl(", "CreateProcess"):
    if forbidden in implementation:
        failures.append(f"workspace implementation can execute a process: {forbidden}")
for required in (
    'field(object, "arguments")',
    "expand_response_files",
    "unsafe_driver_flags",
    "duplicate-command",
    "cxxlens.workspace-snapshot.v2",
    "cxxlens.workspace-cache.v2",
    "infer_project_root",
    "source-outside-project-root",
):
    if required not in implementation:
        failures.append(f"workspace invariant branch missing: {required}")
for fixture in (
    "arguments and equivalent command normalized differently",
    "variants were not preserved",
    "shell metacharacters executed",
    "plugin flag accepted",
    "stale source was not detected",
    "header inference evidence missing",
):
    if fixture not in tests:
        failures.append(f"workspace acceptance fixture missing: {fixture}")
for shared in (implementation, provisioning):
    if "workspace_paths::semantic_path" not in shared:
        failures.append("catalog and provisioning do not share canonical semantic paths")
if "filename()" in path_mapping:
    failures.append("canonical semantic paths contain a basename fallback")
for fixture in (
    "default root collapsed same-basename sources",
    "default root identity changed after relocation",
    "external generated source was accepted",
    "inferred root evidence missing",
):
    if fixture not in tests:
        failures.append(f"workspace root regression fixture missing: {fixture}")
for schema in ("cxxlens_analysis_scope.schema.yaml", "cxxlens_workspace_context.schema.yaml"):
    if not (root / "schemas" / schema).is_file():
        failures.append(f"workspace schema missing: {schema}")

if failures:
    print("workspace contract check failed:\n" + "\n".join(failures), file=sys.stderr)
    raise SystemExit(1)
print("validated workspace catalog, security, snapshot and API ownership contract")
