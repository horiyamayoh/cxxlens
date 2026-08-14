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
    extract_tsan_selection_script,
    parse_expected,
    validate_tsan_ctest_selection,
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

    def test_tsan_excludes_only_adapter_off_native_materializer(self) -> None:
        workflow = (ROOT / ".github/workflows/nightly.yml").read_text(
            encoding="utf-8"
        )
        validate_tsan_ctest_selection(extract_tsan_selection_script(workflow))

    def test_tsan_rejects_exclude_regex_alias(self) -> None:
        continuation = "\\\n"
        command = (
            "ctest --preset tsan --parallel 1 --label-exclude quality "
            + continuation
            + "-E '^install\\.clang22-materializer-success$' "
            + continuation
            + "--output-junit ctest.xml"
        )
        with self.assertRaisesRegex(SanitizerCoverageError, "exact selection"):
            validate_tsan_ctest_selection(command)

    def test_tsan_preserves_single_quoted_continuation_semantics(self) -> None:
        continuation = "\\\n"
        command = (
            "ctest --preset tsan --parallel 1 --label-exclude quality "
            + continuation
            + "--exclude-regex '^install\\.clang22-materializer-success$"
            + continuation
            + "' "
            + continuation
            + "--output-junit ctest.xml"
        )
        with self.assertRaisesRegex(SanitizerCoverageError, "exact selection"):
            validate_tsan_ctest_selection(command)

    def test_tsan_rejects_additional_label_exclusion(self) -> None:
        continuation = "\\\n"
        command = (
            "ctest --preset tsan --parallel 1 "
            + continuation
            + "--label-exclude quality --label-exclude install "
            + continuation
            + "--exclude-regex '^install\\.clang22-materializer-success$' "
            + continuation
            + "--output-junit ctest.xml"
        )
        with self.assertRaisesRegex(SanitizerCoverageError, "exact selection"):
            validate_tsan_ctest_selection(command)

    def test_tsan_rejects_prefixed_ctest_invocations(self) -> None:
        workflow = (ROOT / ".github/workflows/nightly.yml").read_text(
            encoding="utf-8"
        )
        valid_script = extract_tsan_selection_script(workflow)
        continuation = "\\\n"
        extras = [
            "command ctest -E '^broad$'",
            "env ctest -E '^broad$'",
            "true && ctest -E '^broad$'",
            "/usr/bin/ctest -E '^broad$'",
            "ignored=$(ctest -E '^broad$')",
            "c''test -E '^broad$'",
            "c\\test -E '^broad$'",
            "c" + continuation + "test -E '^broad$'",
        ]
        for extra in extras:
            with self.subTest(extra=extra):
                with self.assertRaisesRegex(
                    SanitizerCoverageError, "exact selection"
                ):
                    validate_tsan_ctest_selection(valid_script + "\n" + extra)

    def test_tsan_rejects_an_additional_workflow_step(self) -> None:
        workflow = (ROOT / ".github/workflows/nightly.yml").read_text(
            encoding="utf-8"
        )
        mutated = workflow.replace(
            "      - name: TSan evidence\n",
            "      - name: TSan extra\n"
            "        run: ctest -E '^broad$'\n"
            "      - name: TSan evidence\n",
            1,
        )
        with self.assertRaisesRegex(
            SanitizerCoverageError, "additional CTest"
        ):
            extract_tsan_selection_script(mutated)

    def test_tsan_rejects_dynamic_ctest_in_an_additional_step(self) -> None:
        workflow = (ROOT / ".github/workflows/nightly.yml").read_text(
            encoding="utf-8"
        )
        mutated = workflow.replace(
            "      - name: TSan evidence\n",
            "      - name: TSan extra\n"
            "        run: |\n"
            "          cmd=c''test\n"
            "          \"$cmd\" -E '^broad$'\n"
            "      - name: TSan evidence\n",
            1,
        )
        with self.assertRaisesRegex(
            SanitizerCoverageError, "additional CTest"
        ):
            extract_tsan_selection_script(mutated)

    def test_tsan_rejects_nested_ctest_in_an_additional_step(self) -> None:
        workflow = (ROOT / ".github/workflows/nightly.yml").read_text(
            encoding="utf-8"
        )
        for command in (
            "sh -c 'ctest -E broad'",
            "sh -c '/usr/bin/ctest -E broad'",
            "eval 'ctest -E broad'",
            "printf 'ctest -E broad' | sh",
        ):
            with self.subTest(command=command):
                mutated = workflow.replace(
                    "      - name: TSan evidence\n",
                    "      - name: TSan extra\n"
                    f"        run: {command}\n"
                    "      - name: TSan evidence\n",
                    1,
                )
                with self.assertRaisesRegex(
                    SanitizerCoverageError, "additional CTest"
                ):
                    extract_tsan_selection_script(mutated)

    def test_tsan_rejects_unknown_dynamic_command_in_an_additional_step(self) -> None:
        workflow = (ROOT / ".github/workflows/nightly.yml").read_text(
            encoding="utf-8"
        )
        mutated = workflow.replace(
            "      - name: TSan evidence\n",
            "      - name: TSan extra\n"
            "        run: |\n"
            "          cmd=$CTEST\n"
            "          \"$cmd\" -E '^broad$'\n"
            "      - name: TSan evidence\n",
            1,
        )
        with self.assertRaisesRegex(
            SanitizerCoverageError, "dynamic shell"
        ):
            extract_tsan_selection_script(mutated)


if __name__ == "__main__":
    unittest.main()
