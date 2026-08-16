#!/usr/bin/env python3
"""Positive and fail-closed sanitizer coverage tests."""

from __future__ import annotations

import json
import os
import pathlib
import shutil
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

    def test_unix_asan_exact_clang_boundary_is_shared_and_fail_closed(self) -> None:
        source = (ROOT / "cmake/CxxlensClangTargets.cmake").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "if(UNIX AND CXXLENS_ENABLE_ASAN)\n"
            "    set(_cxxlens_use_shared_clang_cpp TRUE)",
            source,
        )
        asan_selector_start = source.index(
            "if(UNIX AND CXXLENS_ENABLE_ASAN)\n"
            "    set(_cxxlens_use_shared_clang_cpp TRUE)"
        )
        asan_selector_end = source.index(
            "  elseif(CXXLENS_BUILD_SHARED",
            asan_selector_start,
        )
        asan_selector = source[asan_selector_start:asan_selector_end]
        self.assertNotIn(
            "CXXLENS_BUILD_SHARED",
            asan_selector,
            "ASan boundary selection must not depend on the package shared/static mode",
        )
        shared_branch_start = source.index(
            "  if(_cxxlens_use_shared_clang_cpp)", asan_selector_end
        )
        shared_branch_end = source.index(
            "  else()\n    target_link_libraries(${target} PRIVATE ${_cxxlens_clang22_components})",
            shared_branch_start,
        )
        asan_boundary = source[shared_branch_start:shared_branch_end]
        self.assertIn("if(NOT TARGET clang-cpp)", asan_boundary)
        self.assertIn("if(NOT TARGET LLVM)", asan_boundary)
        self.assertIn("get_target_property(_cxxlens_clang_cpp_type clang-cpp TYPE)", asan_boundary)
        self.assertIn(
            'if(NOT _cxxlens_clang_cpp_type STREQUAL "SHARED_LIBRARY")',
            asan_boundary,
        )
        self.assertIn("get_target_property(_cxxlens_llvm_type LLVM TYPE)", asan_boundary)
        self.assertIn(
            'if(NOT _cxxlens_llvm_type STREQUAL "SHARED_LIBRARY")',
            asan_boundary,
        )
        self.assertIn(
            "target_link_libraries(${target} PRIVATE clang-cpp LLVM)",
            asan_boundary,
        )
        self.assertIn(
            "set(CXXLENS_CLANG22_ASAN_SHARED_BOUNDARY\n"
            "          TRUE",
            asan_boundary,
        )
        self.assertIn(
            "set_property(TARGET ${target} PROPERTY CXXLENS_CLANG22_ASAN_SHARED_BOUNDARY TRUE)",
            asan_boundary,
        )

    def test_normal_static_clang_boundary_keeps_explicit_components(self) -> None:
        source = (ROOT / "cmake/CxxlensClangTargets.cmake").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "else()\n"
            "    target_link_libraries(${target} PRIVATE ${_cxxlens_clang22_components})",
            source,
        )
        self.assertIn(
            "elseif(CXXLENS_BUILD_SHARED AND UNIX\n"
            "         AND target STREQUAL \"cxxlens_clang22_provider_sdk\")",
            source,
        )

    def test_ubsan_provider_and_spool_vtables_match_exact_boundary(self) -> None:
        source = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        normalized = " ".join(source.split())
        self.assertNotIn(
            "src/sdk/provider.cpp PROPERTIES COMPILE_OPTIONS \"-fno-rtti\"",
            normalized,
        )
        self.assertIn(
            "if(CXXLENS_ENABLE_UBSAN AND NOT MSVC) set_source_files_properties( "
            "src/llvm/clang22/materialization_io.cpp "
            "src/llvm/clang22/materialization_task_spool.cpp PROPERTIES "
            "COMPILE_OPTIONS \"-frtti\") endif()",
            normalized,
        )

    def test_installed_asan_profile_is_explicit_and_keeps_normal_budget(self) -> None:
        success = (ROOT / "tests/install/clang22_materializer_success_test.py").read_text(
            encoding="utf-8"
        )
        negative = (ROOT / "tests/install/clang22_materializer_negative_test.py").read_text(
            encoding="utf-8"
        )
        tests_cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        profile = 'CXXLENS_ASAN_INSTALLED_QUALIFICATION") == "1"'
        self.assertIn(profile, success)
        self.assertIn(profile, negative)
        self.assertIn("CXXLENS_ASAN_INSTALLED_QUALIFICATION=1", tests_cmake)
        self.assertNotIn('os.environ.get("ASAN_OPTIONS")', success)
        self.assertNotIn('os.environ.get("ASAN_OPTIONS")', negative)
        self.assertIn("ASAN_ADDRESS_SPACE_BYTES = (1 << 63) - 1", success)
        self.assertIn("ASAN_ADDRESS_SPACE_BYTES = (1 << 63) - 1", negative)
        self.assertIn("ASAN_SUBPROCESS_BUDGET = 1024", success)
        self.assertIn("ASAN_SUBPROCESS_BUDGET = 1024", negative)
        self.assertIn('task["budget"]["subprocesses"] = ASAN_SUBPROCESS_BUDGET', success)
        self.assertIn('task["budget"]["subprocesses"] = ASAN_SUBPROCESS_BUDGET', negative)

    def test_clang22_boundary_configure_and_link_graph_regressions(self) -> None:
        if os.name == "nt":
            self.skipTest("the exact Clang 22 ASan shared-boundary contract is UNIX-only")
        components = (
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
        )

        def run_fixture(clang_cpp_kind: str | None, asan: bool) -> tuple[int, str, str]:
            with tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                package_root = root / "package"
                llvm_dir = package_root / "llvm"
                clang_dir = package_root / "clang"
                library_dir = package_root / "lib"
                include_dir = package_root / "include"
                for directory in (llvm_dir, clang_dir, library_dir, include_dir):
                    directory.mkdir(parents=True)
                (library_dir / "libclangBasic.a").touch()
                (root / "probe.cpp").write_text(
                    "int cxxlens_probe() { return 0; }\n", encoding="utf-8"
                )
                (root / "worker.cpp").write_text(
                    "int main() { return 0; }\n", encoding="utf-8"
                )
                if clang_cpp_kind == "shared":
                    fake_source = root / "fake_clang_cpp.cpp"
                    fake_source.write_text(
                        'extern "C" int cxxlens_fake_clang_cpp() { return 0; }\n',
                        encoding="utf-8",
                    )
                    fake_llvm_source = root / "fake_llvm.cpp"
                    fake_llvm_source.write_text(
                        'extern "C" int cxxlens_fake_llvm() { return 0; }\n',
                        encoding="utf-8",
                    )
                    compiler = os.environ.get("CXX") or shutil.which("c++")
                    if compiler is None:
                        return 1, "no C++ compiler available for shared-boundary fixture", ""
                    shared_result = subprocess.run(
                        [
                            compiler,
                            "-shared",
                            "-fPIC",
                            str(fake_source),
                            "-o",
                            str(library_dir / "libclang-cpp.so"),
                        ],
                        check=False,
                        capture_output=True,
                        text=True,
                    )
                    if shared_result.returncode != 0:
                        return (
                            shared_result.returncode,
                            shared_result.stdout + shared_result.stderr,
                            "",
                        )
                    llvm_result = subprocess.run(
                        [
                            compiler,
                            "-shared",
                            "-fPIC",
                            str(fake_llvm_source),
                            "-o",
                            str(library_dir / "libLLVM.so"),
                        ],
                        check=False,
                        capture_output=True,
                        text=True,
                    )
                    if llvm_result.returncode != 0:
                        return (
                            llvm_result.returncode,
                            llvm_result.stdout + llvm_result.stderr,
                            "",
                        )
                (llvm_dir / "LLVMConfig.cmake").write_text(
                    "\n".join(
                        (
                            "set(LLVM_FOUND TRUE)",
                            "set(LLVM_VERSION_MAJOR 22)",
                            "set(LLVM_PACKAGE_VERSION 22.1.0)",
                            "set(LLVM_CMAKE_DIR \"${CMAKE_CURRENT_LIST_DIR}\")",
                            "set(LLVM_LIBRARY_DIRS \"${CMAKE_CURRENT_LIST_DIR}/../lib\")",
                            "set(LLVM_INCLUDE_DIRS \"${CMAKE_CURRENT_LIST_DIR}/../include\")",
                            "add_library(LLVM SHARED IMPORTED GLOBAL)",
                            "set_target_properties(LLVM PROPERTIES IMPORTED_LOCATION \"${CMAKE_CURRENT_LIST_DIR}/../lib/libLLVM.so\")",
                        )
                        + tuple(
                            f"add_library({component} INTERFACE IMPORTED GLOBAL)"
                            for component in ("LLVMOption", "LLVMSupport")
                        )
                    )
                    + "\n",
                    encoding="utf-8",
                )
                (llvm_dir / "LLVMConfigVersion.cmake").write_text(
                    "set(PACKAGE_VERSION 22.1.0)\n"
                    "set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
                    "set(PACKAGE_VERSION_EXACT TRUE)\n",
                    encoding="utf-8",
                )
                clang_config = [
                    "set(Clang_FOUND TRUE)",
                    "set(CLANG_INCLUDE_DIRS \"${CMAKE_CURRENT_LIST_DIR}/../include\")",
                ]
                clang_config.extend(
                    f"add_library({component} INTERFACE IMPORTED GLOBAL)"
                    for component in components
                    if component.startswith("clang")
                )
                if clang_cpp_kind is not None:
                    suffix = "so" if clang_cpp_kind == "shared" else "a"
                    library = library_dir / f"libclang-cpp.{suffix}"
                    library.touch()
                    clang_config.extend(
                        (
                            f"add_library(clang-cpp {clang_cpp_kind.upper()} IMPORTED GLOBAL)",
                            f"set_target_properties(clang-cpp PROPERTIES IMPORTED_LOCATION \"{library}\")",
                        )
                    )
                (clang_dir / "ClangConfig.cmake").write_text(
                    "\n".join(clang_config) + "\n", encoding="utf-8"
                )
                (root / "CMakeLists.txt").write_text(
                    "\n".join(
                        (
                            "cmake_minimum_required(VERSION 3.25)",
                            "project(clang22_boundary_fixture LANGUAGES CXX)",
                            "set(CXXLENS_CLANG_ADAPTER ON CACHE STRING \"\")",
                            f"set(CXXLENS_ENABLE_ASAN {'ON' if asan else 'OFF'} CACHE BOOL \"\")",
                            "set(CXXLENS_ENABLE_UBSAN OFF CACHE BOOL \"\")",
                            "set(CXXLENS_BUILD_SHARED OFF CACHE BOOL \"\")",
                            "add_library(cxxlens_clang22_provider_sdk STATIC probe.cpp)",
                            f"include(\"{(ROOT / 'cmake/CxxlensClangTargets.cmake').as_posix()}\")",
                            "cxxlens_configure_clang22(cxxlens_clang22_provider_sdk)",
                            "get_target_property(_cxxlens_boundary cxxlens_clang22_provider_sdk CXXLENS_CLANG22_ASAN_SHARED_BOUNDARY)",
                            "if(CXXLENS_ENABLE_ASAN AND NOT _cxxlens_boundary)",
                            '  message(FATAL_ERROR "ASan target boundary property was not recorded")',
                            "endif()",
                            "if(NOT CXXLENS_ENABLE_ASAN AND _cxxlens_boundary)",
                            '  message(FATAL_ERROR "normal target boundary property was not cleared")',
                            "endif()",
                            "add_executable(cxxlens-clang-worker-22 worker.cpp)",
                            "target_link_libraries(cxxlens-clang-worker-22 PRIVATE cxxlens_clang22_provider_sdk)",
                        )
                    )
                    + "\n",
                    encoding="utf-8",
                )
                build = root / "build"
                result = subprocess.run(
                    [
                        "cmake",
                        "-S",
                        str(root),
                        "-B",
                        str(build),
                        "-G",
                        "Ninja",
                        "-DLLVM_DIR=" + str(llvm_dir),
                    ],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                cache = (
                    (build / "CMakeCache.txt").read_text(encoding="utf-8")
                    if (build / "CMakeCache.txt").is_file()
                    else ""
                )
                graph = (
                    (build / "build.ninja").read_text(encoding="utf-8")
                    if (build / "build.ninja").is_file()
                    else ""
                )
                output = result.stdout + result.stderr
                if result.returncode == 0 and clang_cpp_kind == "shared" and asan:
                    built = subprocess.run(
                        [
                            "cmake",
                            "--build",
                            str(build),
                            "--target",
                            "cxxlens-clang-worker-22",
                        ],
                        check=False,
                        capture_output=True,
                        text=True,
                    )
                    output += built.stdout + built.stderr
                    if built.returncode != 0:
                        return built.returncode, output, cache + graph
                return result.returncode, output, cache + graph

        code, output, generated = run_fixture("shared", asan=True)
        self.assertEqual(code, 0, output)
        self.assertIn(
            "CXXLENS_CLANG22_ASAN_SHARED_BOUNDARY:INTERNAL=TRUE", generated
        )
        self.assertIn("libclang-cpp.so", generated)
        self.assertIn("libLLVM.so", generated)
        self.assertNotIn("libclangBasic.a", generated)

        for kind, expected in (
            (None, "requires the packaged clang-cpp shared target"),
            ("static", "requires clang-cpp to be a shared library target"),
        ):
            code, output, _ = run_fixture(kind, asan=True)
            self.assertNotEqual(code, 0, output)
            self.assertIn(expected, " ".join(output.split()))

        code, output, generated = run_fixture("static", asan=False)
        self.assertEqual(code, 0, output)
        self.assertIn(
            "CXXLENS_CLANG22_ASAN_SHARED_BOUNDARY:INTERNAL=FALSE", generated
        )
        self.assertIn("CXXLENS_CLANG22_EXPLICIT_COMPONENTS:INTERNAL=", generated)

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
