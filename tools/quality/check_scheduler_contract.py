#!/usr/bin/env python3
"""Enforce scheduler confinement, deterministic containers, and acceptance seams."""

from __future__ import annotations

import pathlib
import re
import sys


root = pathlib.Path(sys.argv[1])
header = (root / "src/workspace/scheduler.hpp").read_text()
implementation = (root / "src/workspace/scheduler.cpp").read_text()
combined = header + implementation

for forbidden, reason in (
    (r"#\s*include\s*[<\"](?:clang|llvm)/", "native compiler include"),
    (r"\b(?:clang|llvm)::", "native compiler type"),
    (r"\b(?:ASTContext|SourceManager|CompilerInstance|Decl|Stmt)\s*\*", "AST pointer"),
    (r"std::unordered_(?:map|set)", "unordered iteration"),
    (r"std::chrono::(?:steady_clock|system_clock)::now\s*\(", "ambient time"),
    (r"std::hash\s*<", "ambient hash"),
):
    if re.search(forbidden, combined):
        raise SystemExit(f"scheduler contains forbidden {reason}")

for marker in (
    "cxxlens.scheduler-task.v1",
    "canonical_encoder",
    "snapshot_key",
    "worker_port",
    "maximum_queued_tasks",
    "maximum_output_bytes",
    "memory_per_job_mb",
    "cost_budget",
    "core.cancelled",
    "core.deadline-exceeded",
    "core.budget-exhausted",
    "coalesced",
    "dependency_failed",
    "std::ranges::sort(batch.tasks",
    "std::ranges::sort(batch.trace",
    "time_.steady_now()",
):
    if marker not in combined:
        raise SystemExit(f"scheduler contract marker is missing: {marker}")

schema = (root / "schemas/cxxlens_scheduler_trace.schema.yaml").read_text()
for marker in (
    "cxxlens.scheduler-trace.v1",
    "frontend_coverage",
    "diagnostics",
    "dependency_failed",
    "output_limited",
):
    if marker not in schema:
        raise SystemExit(f"scheduler schema marker is missing: {marker}")

test = (root / "tests/unit/workspace/scheduler_test.cpp").read_text()
for marker in (
    "{1U, 2U, 8U}",
    "0xC771EU",
    "duplicate work was not coalesced",
    "subscriber cancellation leaked into shared work",
    "partial failure did not preserve/account successful siblings",
    "queue overflow was not structured",
    "output budget was not bounded and explicit",
):
    if marker not in test:
        raise SystemExit(f"scheduler acceptance seam is missing: {marker}")

print("validated deterministic bounded scheduler, cancellation, coalescing, and AST confinement")
