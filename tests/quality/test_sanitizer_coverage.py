#!/usr/bin/env python3
"""Positive and fail-closed sanitizer coverage tests."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_sanitizer_coverage import (  # noqa: E402
    SanitizerCoverageError,
    parse_expected,
    validate_contract,
    validate_database,
)


class SanitizerCoverageTest(unittest.TestCase):
    def setUp(self) -> None:
        validate_contract(ROOT)

    def make_database(self, flag: str) -> pathlib.Path:
        temporary = tempfile.NamedTemporaryFile(
            mode="w", suffix=".json", encoding="utf-8", delete=False
        )
        self.addCleanup(pathlib.Path(temporary.name).unlink, missing_ok=True)
        json.dump(
            [
                {
                    "directory": str(ROOT),
                    "file": str(ROOT / "tests/canary/sanitizer_canary.cpp"),
                    "arguments": ["clang++", flag, "-c", "sanitizer_canary.cpp"],
                }
            ],
            temporary,
        )
        temporary.close()
        return pathlib.Path(temporary.name)

    def test_exact_combined_instrumentation_is_accepted(self) -> None:
        database = self.make_database("-fsanitize=address,undefined")
        self.assertEqual(validate_database(database, {"address", "undefined"}), 1)

    def test_missing_object_instrumentation_is_rejected(self) -> None:
        database = self.make_database("-fno-omit-frame-pointer")
        with self.assertRaisesRegex(SanitizerCoverageError, "sanitizer set differs"):
            validate_database(database, {"thread"})

    def test_sanitizer_leak_into_normal_build_is_rejected(self) -> None:
        database = self.make_database("-fsanitize=address")
        with self.assertRaisesRegex(SanitizerCoverageError, "sanitizer set differs"):
            validate_database(database, set())

    def test_mixed_thread_configuration_is_rejected_before_generation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build_directory = pathlib.Path(temporary)
            result = subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(ROOT),
                    "-B",
                    str(build_directory),
                    "-DBUILD_TESTING=OFF",
                    "-DCXXLENS_BUILD_QUALITY_TOOLS=OFF",
                    "-DCXXLENS_ENABLE_ASAN=ON",
                    "-DCXXLENS_ENABLE_TSAN=ON",
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertFalse((build_directory / "build.ninja").exists())
            self.assertFalse((build_directory / "Makefile").exists())
        self.assertNotEqual(result.returncode, 0)

    def test_parser_rejects_mixed_thread_set(self) -> None:
        with self.assertRaisesRegex(SanitizerCoverageError, "cannot be mixed"):
            parse_expected("address,thread")


if __name__ == "__main__":
    unittest.main()
