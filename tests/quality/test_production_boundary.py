#!/usr/bin/env python3
"""Focused tests for the production/test kernel boundary checker."""

from __future__ import annotations

import contextlib
import io
import json
import pathlib
import tempfile
import unittest
from dataclasses import replace

from check_production_boundary import (
    CompileProfile,
    ProductionBoundaryError,
    _surface_files,
    compare_compile_profiles,
    find_forbidden_seam,
    load_compile_profile,
    main,
    parse_nm_symbols,
    read_symbols,
)


class ProductionBoundaryCheckerTest(unittest.TestCase):
    def test_product_receipt_and_digest_vocabulary_is_not_a_test_seam(self) -> None:
        allowed = (
            "cxxlens::sdk::sqlite_zero_effect_receipt::valid() const",
            "cxxlens::sdk::content_digest(std::span<std::byte const>)",
            "semantic_identity_digest",
            "normalization_receipt",
            "source_closure_provenance",
            "ordinary_projection_view",
            "provider_factory",
        )
        for value in allowed:
            with self.subTest(value=value):
                self.assertIsNone(find_forbidden_seam(value))

    def test_exact_test_seams_are_rejected(self) -> None:
        forbidden = (
            "CXXLENS_SQLITE_TEST_SUPPORT",
            "CXXLENS_STORE_FAULT_TEST_SUPPORT",
            "lib/cxxlens-test_support.a",
            "rewrite_publication_for_testing",
            "snapshot_store_test_peer",
            "sqlite_reader_test_view",
            "make_test_factory",
            "dispatch_sqlite_store_fault",
            "cxxlens::test_kernel",
        )
        for value in forbidden:
            with self.subTest(value=value):
                self.assertIsNotNone(find_forbidden_seam(value))

    def test_compile_profile_requires_identical_product_sources_and_definitions(self) -> None:
        enabled = CompileProfile(
            build=pathlib.Path("enabled"),
            build_testing=True,
            shared=False,
            sources=frozenset({"src/sdk/store.cpp"}),
            definitions_by_source=(("src/sdk/store.cpp", ("PRODUCT=1",)),),
        )
        disabled = CompileProfile(
            build=pathlib.Path("disabled"),
            build_testing=False,
            shared=False,
            sources=frozenset({"src/sdk/store.cpp"}),
            definitions_by_source=(("src/sdk/store.cpp", ("PRODUCT=1",)),),
        )
        compare_compile_profiles(enabled, disabled)

        changed_sources = CompileProfile(
            build=disabled.build,
            build_testing=False,
            shared=False,
            sources=frozenset({"src/sdk/store.cpp", "src/sdk/test_adapter.cpp"}),
            definitions_by_source=disabled.definitions_by_source,
        )
        with self.assertRaisesRegex(ProductionBoundaryError, "source set"):
            compare_compile_profiles(enabled, changed_sources)

        changed_definitions = replace(
            disabled,
            definitions_by_source=(
                ("src/sdk/store.cpp", ("CXXLENS_STORE_FAULT_TEST_SUPPORT=1",)),
            ),
        )
        with self.assertRaisesRegex(ProductionBoundaryError, "compile definitions"):
            compare_compile_profiles(enabled, changed_definitions)

    def test_compile_database_rejects_test_support_on_product_target(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source = root / "source"
            build = root / "build"
            source.mkdir()
            build.mkdir()
            source_file = source / "store.cpp"
            source_file.write_text("int store;\n", encoding="utf-8")
            (build / "CMakeCache.txt").write_text(
                "BUILD_TESTING:BOOL=ON\n"
                "CXXLENS_BUILD_SHARED:BOOL=OFF\n"
                f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\n",
                encoding="utf-8",
            )
            (build / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(build),
                            "command": (
                                "c++ -DCXXLENS_SQLITE_TEST_SUPPORT=1 "
                                f"-o CMakeFiles/cxxlens_kernel.dir/store.cpp.o -c {source_file}"
                            ),
                            "file": str(source_file),
                            "output": "CMakeFiles/cxxlens_kernel.dir/store.cpp.o",
                        }
                    ]
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ProductionBoundaryError, "test-support definitions"):
                load_compile_profile(build, expected_testing=True)

    def test_nm_parser_keeps_demangled_names_and_ignores_archive_headers(self) -> None:
        output = "\n".join(
            (
                "libcxxlens_kernel.a[store.cpp.o]:",
                "cxxlens::sdk::content_digest(std::span<std::byte const>) T 10 20",
                "cxxlens::sdk::snapshot_store::~snapshot_store() W 0 30",
                "not an nm record",
            )
        )
        self.assertEqual(
            parse_nm_symbols(output),
            frozenset(
                {
                    "cxxlens::sdk::content_digest(std::span<std::byte const>)",
                    "cxxlens::sdk::snapshot_store::~snapshot_store()",
                }
            ),
        )

    def test_nm_symbol_scan_rejects_fault_dispatcher(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            library = root / "libcxxlens_kernel.a"
            library.write_bytes(b"synthetic")
            fake_nm = root / "nm"
            fake_nm.write_text(
                "#!/usr/bin/env python3\n"
                "print('cxxlens::sdk::dispatch_sqlite_store_fault() T 10 20')\n",
                encoding="utf-8",
            )
            fake_nm.chmod(0o755)
            with self.assertRaisesRegex(ProductionBoundaryError, "fault dispatcher"):
                read_symbols(str(fake_nm), library, shared=False)

    def test_installed_surface_allows_product_safety_terms_and_rejects_test_seams(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            prefix = pathlib.Path(temporary)
            include = prefix / "include" / "cxxlens"
            exports = prefix / "lib" / "cmake" / "cxxlens"
            include.mkdir(parents=True)
            exports.mkdir(parents=True)
            header = include / "sdk.hpp"
            header.write_text(
                "struct normalization_receipt {};\n"
                "using semantic_digest = const char*;\n",
                encoding="utf-8",
            )
            (exports / "cxxlensTargets.cmake").write_text(
                "add_library(cxxlens::kernel STATIC IMPORTED)\n",
                encoding="utf-8",
            )
            self.assertEqual(len(_surface_files(prefix)), 2)

            header.write_text(
                "void rewrite_snapshot_for_testing();\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ProductionBoundaryError, "for-testing identifier"):
                _surface_files(prefix)

            header.write_text("struct normalization_receipt {};\n", encoding="utf-8")
            (exports / "cxxlensTargets.cmake").write_text(
                "add_library(cxxlens::test_kernel STATIC IMPORTED)\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ProductionBoundaryError, "test kernel target"):
                _surface_files(prefix)

    def test_end_to_end_check_accepts_identical_product_surfaces(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source = root / "source"
            source.mkdir()
            source_file = source / "store.cpp"
            source_file.write_text("int store;\n", encoding="utf-8")

            builds: list[pathlib.Path] = []
            prefixes: list[pathlib.Path] = []
            for name, testing in (("on", "ON"), ("off", "OFF")):
                build = root / f"build-{name}"
                prefix = root / f"install-{name}"
                build.mkdir()
                (prefix / "include" / "cxxlens").mkdir(parents=True)
                (prefix / "lib" / "cmake" / "cxxlens").mkdir(parents=True)
                (prefix / "lib").mkdir(exist_ok=True)
                (build / "CMakeCache.txt").write_text(
                    f"BUILD_TESTING:BOOL={testing}\n"
                    "CXXLENS_BUILD_SHARED:BOOL=OFF\n"
                    f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\n",
                    encoding="utf-8",
                )
                (build / "compile_commands.json").write_text(
                    json.dumps(
                        [
                            {
                                "directory": str(build),
                                "command": (
                                    "c++ -DPRODUCT_CONTRACT=1 "
                                    "-o CMakeFiles/cxxlens_kernel.dir/store.cpp.o "
                                    f"-c {source_file}"
                                ),
                                "file": str(source_file),
                                "output": "CMakeFiles/cxxlens_kernel.dir/store.cpp.o",
                            }
                        ]
                    ),
                    encoding="utf-8",
                )
                (build / "libcxxlens_kernel.a").write_bytes(b"synthetic")
                (prefix / "lib" / "libcxxlens_kernel.a").write_bytes(b"synthetic")
                (prefix / "include" / "cxxlens" / "sdk.hpp").write_text(
                    "struct zero_effect_receipt {};\n"
                    "using semantic_digest = const char*;\n",
                    encoding="utf-8",
                )
                (prefix / "lib" / "cmake" / "cxxlens" / "cxxlensTargets.cmake").write_text(
                    "add_library(cxxlens::kernel STATIC IMPORTED)\n",
                    encoding="utf-8",
                )
                builds.append(build)
                prefixes.append(prefix)

            fake_nm = root / "nm"
            fake_nm.write_text(
                "#!/usr/bin/env python3\n"
                "print('cxxlens::sdk::semantic_digest() T 10 20')\n"
                "print('cxxlens::sdk::zero_effect_receipt::valid() const T 30 40')\n",
                encoding="utf-8",
            )
            fake_nm.chmod(0o755)
            arguments = [
                "--build-testing-on",
                str(builds[0]),
                "--build-testing-off",
                str(builds[1]),
                "--install-testing-on",
                str(prefixes[0]),
                "--install-testing-off",
                str(prefixes[1]),
                "--nm",
                str(fake_nm),
            ]
            paths_before = {
                path.relative_to(root) for path in root.rglob("*") if path.is_file()
            }
            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                status = main(arguments)
            paths_after = {
                path.relative_to(root) for path in root.rglob("*") if path.is_file()
            }
            self.assertEqual(status, 0)
            self.assertEqual(stdout.getvalue(), "")
            self.assertEqual(stderr.getvalue(), "")
            self.assertEqual(paths_after, paths_before)

            fake_nm.write_text(
                "#!/usr/bin/env python3\n"
                "import sys\n"
                "symbol = 'changed_abi' if 'build-off' in sys.argv[-1] "
                "else 'semantic_digest'\n"
                "print(f'cxxlens::sdk::{symbol}() T 10 20')\n",
                encoding="utf-8",
            )
            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                status = main(arguments)
            self.assertEqual(status, 1)
            self.assertEqual(stdout.getvalue(), "")
            self.assertIn("changes the cxxlens_kernel global ABI", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
