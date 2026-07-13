#!/usr/bin/env python3
"""Enforce the exact-major Clang adapter, lifetime, and link boundaries."""

from __future__ import annotations

import pathlib
import re
import sys


root = pathlib.Path(sys.argv[1])
include_pattern = re.compile(r"^\s*#\s*include\s*[<\"](?:clang|llvm)/", re.MULTILINE)
allowed_roots = (
    root / "src/llvm/clang22",
    root / "include/cxxlens/interop",
)
for path in list((root / "src").rglob("*.cpp")) + list((root / "src").rglob("*.hpp")) + list(
    (root / "include").rglob("*.hpp")
):
    if include_pattern.search(path.read_text()) and not any(path.is_relative_to(base) for base in allowed_roots):
        raise SystemExit(f"Clang/LLVM include escaped adapter allowlist: {path.relative_to(root)}")

common = "\n".join(path.read_text() for path in (root / "src/llvm/common").rglob("*.*"))
if include_pattern.search(common) or re.search(r"\b(?:clang|llvm)::", common):
    raise SystemExit("Clang-free frontend ports expose native compiler types")

ordinary = "\n".join(
    path.read_text()
    for path in (root / "include/cxxlens").rglob("*.hpp")
    if not path.is_relative_to(root / "include/cxxlens/interop")
)
if re.search(r"\b(?:clang|llvm)::", ordinary) or include_pattern.search(ordinary):
    raise SystemExit("ordinary public headers expose Clang/LLVM")

cmake = (root / "cmake/CxxlensClangTargets.cmake").read_text()
expected_components = {
    "LLVMOption",
    "LLVMSupport",
    "clangAST",
    "clangBasic",
    "clangDriver",
    "clangFrontend",
    "clangFrontendTool",
    "clangIndex",
    "clangLex",
    "clangOptions",
    "clangSerialization",
    "clangTooling",
    "clangToolingCore",
}
missing = sorted(component for component in expected_components if component not in cmake)
if missing:
    raise SystemExit(f"explicit Clang link map is incomplete: {missing}")
if re.search(r"(^|\s)clang-cpp($|\s)", cmake):
    raise SystemExit("monolithic clang-cpp link hides component closure")
for marker in (
    "CXXLENS_CLANG_ADAPTER",
    "LLVM_CMAKE_DIR",
    "_cxxlens_clang_basic_library",
    "LLVM_VERSION_MAJOR EQUAL 22",
    "CXXLENS_HAS_CLANG22=0",
    "CXXLENS_HAS_CLANG22=1",
):
    if marker not in cmake:
        raise SystemExit(f"missing exact-major capability marker: {marker}")

adapter = (root / "src/llvm/clang22/frontend_job.cpp").read_text()
port = (root / "src/llvm/common/frontend_port.hpp").read_text()
if "std::vector<facts::observation_record> observations" not in port:
    raise SystemExit("frontend batch is not connected to the detached observation contract")
for marker in (
    "borrowed_lifetime_token",
    "lifetime.retire()",
    "OverlayFileSystem",
    "InMemoryFileSystem",
    "batch.validate()",
    "core.capability-unavailable",
    "parse.timeout",
    "parse.crashed",
):
    if marker not in adapter:
        raise SystemExit(f"frontend adapter contract marker is missing: {marker}")
if "co_await" in adapter or "co_yield" in adapter:
    raise SystemExit("borrowed frontend callback may not suspend")

schema = (root / "schemas/cxxlens_frontend_batch.schema.yaml").read_text()
if "cxxlens_observation.schema.yaml" not in schema or "llvm_major: {const: 22}" not in schema:
    raise SystemExit("frontend batch does not validate exact-major observations")

worker = (root / "src/workspace/frontend_scheduler_worker.cpp").read_text()
ipc = (root / "src/llvm/common/frontend_worker_ipc.cpp").read_text()
worker_contract = (root / "schemas/cxxlens_frontend_worker_ipc.contract.yaml").read_text()
isolation_test = (root / "tests/unit/workspace/scheduler_test.cpp").read_text()
for marker in (
    "argv_process_adapter",
    "encode_worker_request",
    "decode_worker_response",
    '"parse.crashed"',
    '"parse.timeout"',
    '"core.cancelled"',
):
    if marker not in worker:
        raise SystemExit(f"frontend worker isolation marker is missing: {marker}")
for marker in (
    "CXXLFWQ1",
    "CXXLFWR1",
    "worker_ipc_version",
    "maximum_message_bytes",
    "input.remaining() != 0U",
    "batch.validate()",
):
    if marker not in ipc:
        raise SystemExit(f"frontend IPC fail-closed marker is missing: {marker}")
for marker in (
    "cxxlens.frontend-worker-ipc.v1",
    "anonymous_memfd_stdin",
    "temporary_artifacts: forbidden",
):
    if marker not in worker_contract:
        raise SystemExit(f"frontend worker contract marker is missing: {marker}")
for marker in (
    "--block",
    "--crash",
    "worker-signal",
    "coverage.succeeded == 1U",
):
    if marker not in isolation_test:
        raise SystemExit(f"frontend process acceptance fixture is missing: {marker}")

catalog = (root / "schemas/cxxlens_public_api_contract.yaml").read_text()
for api in ("API-INT-001", "API-INT-002"):
    position = catalog.find(f"- id: {api}")
    if position < 0 or "implementation_state: conformant" not in catalog[position : position + 1800]:
        raise SystemExit(f"{api} is not catalogued as conformant")

print("validated exact Clang 22 discovery, IPC isolation, VFS, and borrowed lifetime boundaries")
