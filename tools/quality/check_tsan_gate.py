#!/usr/bin/env python3
"""Validate the nightly ThreadSanitizer gate and suppression ledger."""

from __future__ import annotations

import datetime as dt
import json
import pathlib
import re
import sys

import yaml


root = pathlib.Path(sys.argv[1])
presets = json.loads((root / "CMakePresets.json").read_text(encoding="utf-8"))
tsan = next(
    (item for item in presets["configurePresets"] if item.get("name") == "tsan"),
    None,
)
if tsan is None:
    raise SystemExit("tsan configure preset is missing")
cache = tsan.get("cacheVariables", {})
if cache.get("CXXLENS_ENABLE_TSAN") != "ON":
    raise SystemExit("tsan preset does not enable ThreadSanitizer")
if cache.get("CXXLENS_CLANG_ADAPTER") != "OFF":
    raise SystemExit("mandatory tsan gate must explicitly disable the Clang adapter")

workflow = (root / ".github/workflows/nightly.yml").read_text(encoding="utf-8")
for marker in (
    "thread-sanitizer-foundation:",
    "cmake --preset tsan",
    "cmake --build --preset tsan",
    "sanitizer\\.tsan-detection",
    "-E '^sanitizer\\.tsan-detection$'",
    "-L concurrency --repeat until-fail:5",
    "actions/upload-artifact@v4",
    "if: always()",
    "tests/tsan/tsan.supp",
    "handle_segv=0",
):
    if marker not in workflow:
        raise SystemExit(f"nightly TSan gate marker is missing: {marker}")

tests_cmake = (root / "tests/CMakeLists.txt").read_text(encoding="utf-8")
for test_name in (
    "unit.runtime-ports",
    "unit.fact-store",
    "unit.scheduler",
    "unit.provisioning",
):
    match = re.search(
        rf"set_tests_properties\(\s*{re.escape(test_name)}\s+PROPERTIES.*?"
        rf"LABELS\s+\"([^\"]*)\"",
        tests_cmake,
        re.DOTALL,
    )
    if match is None or "concurrency" not in match.group(1).split(";"):
        raise SystemExit(f"{test_name} is not selected by the concurrency label")

for marker in (
    "fixtures/tsan_race_fixture.cpp",
    "verify_tsan_detection.py",
    "cxxlens_enable_sanitizers(${cxxlens_test_target})",
):
    if marker not in tests_cmake:
        raise SystemExit(f"instrumented TSan test seam is missing: {marker}")

acceptance_markers = {
    "tests/unit/workspace/scheduler_test.cpp": (
        "{1U, 2U, 8U}",
        "duplicate work was not coalesced to one worker",
        "blocking worker did not return within bounded deadline grace",
        "real worker signal was not converted to structured parse.crashed evidence",
    ),
    "tests/unit/runtime/runtime_ports_test.cpp": (
        "test_concurrent_process_launch",
        "timeout/cancellation left a live descendant",
    ),
    "tests/unit/store/fact_store_test.cpp": (
        "std::barrier start{9}",
        "concurrent reader observed a partial snapshot",
    ),
    "tests/unit/workspace/provisioning_test.cpp": (
        "{1U, 2U, 8U}",
        "jobs 1/2/8 or relocated root changed semantic facts/coverage",
    ),
}
for relative, markers in acceptance_markers.items():
    source = (root / relative).read_text(encoding="utf-8")
    for marker in markers:
        if marker not in source:
            raise SystemExit(f"TSan regression seam is missing from {relative}: {marker}")

fixture = (root / "tests/fixtures/tsan_race_fixture.cpp").read_text(encoding="utf-8")
verifier = (root / "tests/tsan/verify_tsan_detection.py").read_text(encoding="utf-8")
if "++shared_counter" not in fixture or "WARNING: ThreadSanitizer: data race" not in verifier:
    raise SystemExit("intentional race detection self-test is incomplete")

suppression_file = root / "tests/tsan/tsan.supp"
active_lines = {
    line.strip()
    for line in suppression_file.read_text(encoding="utf-8").splitlines()
    if line.strip() and not line.lstrip().startswith("#")
}
ledger = yaml.safe_load(
    (root / "tests/tsan/suppressions.yaml").read_text(encoding="utf-8")
)
if ledger.get("schema") != "cxxlens.tsan-suppressions.v1":
    raise SystemExit("TSan suppression ledger schema is invalid")
records = ledger.get("suppressions")
if not isinstance(records, list):
    raise SystemExit("TSan suppression ledger must contain a list")

declared_lines: set[str] = set()
required = {"id", "type", "target", "library", "reason", "expires", "reverify"}
for record in records:
    if not isinstance(record, dict) or not required.issubset(record):
        raise SystemExit("every TSan suppression requires complete review metadata")
    if not all(isinstance(record[field], str) and record[field].strip() for field in required):
        raise SystemExit(f"TSan suppression {record.get('id', '<unknown>')} has empty metadata")
    if "*" in record["target"] or record["target"] in {".*", "all"}:
        raise SystemExit(f"TSan suppression {record['id']} is broader than a symbol/library")
    try:
        expiry = dt.date.fromisoformat(record["expires"])
    except ValueError as error:
        raise SystemExit(f"TSan suppression {record['id']} has an invalid expiry") from error
    if expiry < dt.date.today():
        raise SystemExit(f"TSan suppression {record['id']} expired on {expiry.isoformat()}")
    declared_lines.add(f"{record['type']}:{record['target']}")

if active_lines != declared_lines:
    raise SystemExit("TSan suppression file and metadata ledger do not match exactly")

print(
    "validated LLVM-independent nightly TSan gate, concurrency matrix, "
    f"detection self-test, and {len(records)} reviewed suppressions"
)
