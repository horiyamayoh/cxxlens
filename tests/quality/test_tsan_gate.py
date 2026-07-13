#!/usr/bin/env python3
"""Negative tests for the ThreadSanitizer gate validator."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
CHECKER = ROOT / "tools/quality/check_tsan_gate.py"
INPUTS = (
    ".github/workflows/nightly.yml",
    "CMakePresets.json",
    "tests/CMakeLists.txt",
    "tests/fixtures/tsan_race_fixture.cpp",
    "tests/tsan/suppressions.yaml",
    "tests/tsan/tsan.supp",
    "tests/tsan/verify_tsan_detection.py",
    "tests/unit/runtime/runtime_ports_test.cpp",
    "tests/unit/store/fact_store_test.cpp",
    "tests/unit/workspace/provisioning_test.cpp",
    "tests/unit/workspace/scheduler_test.cpp",
)


def run(root: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["python3", str(CHECKER), str(root)],
        check=False,
        capture_output=True,
        text=True,
    )


baseline = run(ROOT)
if baseline.returncode != 0:
    raise SystemExit(baseline.stdout + baseline.stderr)

with tempfile.TemporaryDirectory(prefix="cxxlens-tsan-gate-") as directory:
    fixture = pathlib.Path(directory)
    for relative in INPUTS:
        destination = fixture / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, destination)

    presets = fixture / "CMakePresets.json"
    presets.write_text(
        presets.read_text(encoding="utf-8").replace(
            '"CXXLENS_CLANG_ADAPTER": "OFF"',
            '"CXXLENS_CLANG_ADAPTER": "AUTO"',
            1,
        ),
        encoding="utf-8",
    )
    if run(fixture).returncode == 0:
        raise SystemExit("validator accepted an AUTO Clang adapter in the mandatory TSan gate")

    shutil.copy2(ROOT / "CMakePresets.json", presets)
    (fixture / "tests/tsan/tsan.supp").write_text("race:*\n", encoding="utf-8")
    if run(fixture).returncode == 0:
        raise SystemExit("validator accepted an unreviewed blanket suppression")

    shutil.copy2(ROOT / "tests/tsan/tsan.supp", fixture / "tests/tsan/tsan.supp")
    tests_cmake = fixture / "tests/CMakeLists.txt"
    tests_cmake.write_text(
        tests_cmake.read_text(encoding="utf-8").replace(
            "transaction;coverage;determinism;conformance;concurrency",
            "transaction;coverage;determinism;conformance",
            1,
        ),
        encoding="utf-8",
    )
    if run(fixture).returncode == 0:
        raise SystemExit("validator accepted a fact-store test outside the concurrency gate")

print("validated negative TSan gate and suppression-policy cases")
