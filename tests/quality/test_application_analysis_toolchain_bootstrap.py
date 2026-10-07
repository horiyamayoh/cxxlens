#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import io
import json
import pathlib
import shutil
import tarfile
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/ci/bootstrap_application_analysis_toolchain.py"
LOCK = ROOT / "tools/ci/application-analysis-toolchains.lock.json"


def load_module():
    specification = importlib.util.spec_from_file_location(
        "cxxlens_application_analysis_toolchain_bootstrap", SCRIPT
    )
    if specification is None or specification.loader is None:
        raise RuntimeError("could not load application-analysis bootstrap")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


class ApplicationAnalysisToolchainBootstrapTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.bootstrap = load_module()
        cls.lock = json.loads(LOCK.read_text(encoding="utf-8"))

    def write_lock(self, directory: pathlib.Path, value: dict) -> pathlib.Path:
        destination = directory / "lock.json"
        destination.write_text(json.dumps(value), encoding="utf-8")
        return destination

    def test_tracked_lock_is_accepted(self) -> None:
        admitted = self.bootstrap.load_lock()
        self.assertEqual(admitted["gcc"]["exact_version"], "16.2.0")
        self.assertEqual(
            admitted["clang_gcc_replay"]["exact_version"], "23.1.0"
        )
        self.assertEqual(admitted["clang_cl_replay"]["exact_version"], "23.1.0")
        self.assertEqual(admitted["msvc"]["version_series"], "19.51")
        self.assertEqual(
            admitted["windows_sdk"]["exact_version"], "10.1.26100.8249"
        )
        self.assertEqual(admitted["windows_sdk"]["kit_version"], "10.0.26100.0")
        self.assertEqual(
            admitted["windows_runner"]["label"], "windows-2025-vs2026"
        )
        self.assertEqual(admitted["runner"]["label"], "ubuntu-24.04")
        self.assertEqual(admitted["clang22_original"]["exact_version"], "22.1.0")

    def test_malformed_source_identity_and_build_recipe_drift_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            directory = pathlib.Path(raw_directory)
            wrong_digest = json.loads(json.dumps(self.lock))
            wrong_digest["gcc"]["source_sha512"] = "0" * 127
            with self.assertRaisesRegex(
                self.bootstrap.ToolchainError, "source SHA-512"
            ):
                self.bootstrap.load_lock(self.write_lock(directory, wrong_digest))

            wrong_recipe = json.loads(json.dumps(self.lock))
            wrong_recipe["gcc"]["configure_arguments"].append("--enable-multilib")
            with self.assertRaisesRegex(
                self.bootstrap.ToolchainError, "configure lock differs"
            ):
                self.bootstrap.load_lock(self.write_lock(directory, wrong_recipe))

            unknown_field = json.loads(json.dumps(self.lock))
            unknown_field["gcc"]["automatic_minor_upgrade"] = True
            with self.assertRaisesRegex(
                self.bootstrap.ToolchainError, "unknown GCC toolchain lock field"
            ):
                self.bootstrap.load_lock(self.write_lock(directory, unknown_field))

            wrong_clang_digest = json.loads(json.dumps(self.lock))
            wrong_clang_digest["clang_gcc_replay"]["asset_sha256"] = "0" * 63
            with self.assertRaisesRegex(
                self.bootstrap.ToolchainError, "asset SHA-256"
            ):
                self.bootstrap.load_lock(
                    self.write_lock(directory, wrong_clang_digest)
                )

            wrong_clang_asset = json.loads(json.dumps(self.lock))
            wrong_clang_asset["clang_gcc_replay"]["asset_url"] = (
                "https://example.invalid/LLVM.tar.xz"
            )
            with self.assertRaisesRegex(
                self.bootstrap.ToolchainError, "asset authority differs"
            ):
                self.bootstrap.load_lock(
                    self.write_lock(directory, wrong_clang_asset)
                )

            wrong_clang_cl_asset = json.loads(json.dumps(self.lock))
            wrong_clang_cl_asset["clang_cl_replay"]["asset_sha256"] = "0" * 64
            with self.assertRaisesRegex(
                self.bootstrap.ToolchainError, "Clang-cl replay lock differs"
            ):
                self.bootstrap.load_lock(
                    self.write_lock(directory, wrong_clang_cl_asset)
                )

            wrong_msvc = json.loads(json.dumps(self.lock))
            wrong_msvc["msvc"]["toolset_version"] = "14.52"
            with self.assertRaisesRegex(
                self.bootstrap.ToolchainError, "MSVC toolchain lock differs"
            ):
                self.bootstrap.load_lock(self.write_lock(directory, wrong_msvc))

    def test_original_clang_patch_archive_and_checksum_drift_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            directory = pathlib.Path(raw_directory)
            for field, replacement in [
                ("exact_version", "22.1.8"),
                ("asset_url", "https://example.invalid/LLVM-22.1.0.tar.xz"),
                ("asset_sha256", "0" * 64),
                ("asset_archive_bytes", 1),
            ]:
                with self.subTest(field=field):
                    wrong = json.loads(json.dumps(self.lock))
                    wrong["clang22_original"][field] = replacement
                    with self.assertRaisesRegex(
                        self.bootstrap.ToolchainError, "original Clang 22 toolchain lock differs"
                    ):
                        self.bootstrap.load_lock(self.write_lock(directory, wrong))

    def test_original_clang_and_llvm_patch_mismatch_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            prefix = pathlib.Path(raw_directory)
            for relative in ["bin/clang++", "lib/cmake/llvm/LLVMConfig.cmake", "lib/cmake/clang/ClangConfig.cmake"]:
                path = prefix / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("test input", encoding="utf-8")
            with mock.patch.object(self.bootstrap, "run", side_effect=["22.1.8", "x86_64-unknown-linux-gnu"]):
                with self.assertRaisesRegex(self.bootstrap.ToolchainError, "original Clang 22 version differs"):
                    self.bootstrap.verify_clang22_original(prefix, self.lock)
            with mock.patch.object(self.bootstrap, "verify_clang_archive"), mock.patch.object(self.bootstrap, "run", return_value="22.1.8"):
                with self.assertRaisesRegex(self.bootstrap.ToolchainError, "original LLVM 22 version differs"):
                    self.bootstrap.verify_clang22_original(prefix, self.lock)

    def make_original_tools(self, prefix: pathlib.Path) -> None:
        (prefix / "bin").mkdir(parents=True)
        for name in ["clang", "clang++", "clang-format", "clang-tidy", "run-clang-tidy", "llvm-config", "llvm-symbolizer"]:
            path = prefix / "bin" / name
            path.write_text("original compiler tool", encoding="utf-8")
            path.chmod(0o755)

    def test_ci_selectors_choose_exact_tools_and_cmake_packages(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            directory = pathlib.Path(raw_directory)
            prefix = directory / "original LLVM"
            selectors = directory / "selectors"
            self.make_original_tools(prefix)
            environment = directory / "github-env"
            path_file = directory / "github-path"
            apt = directory / "apt-bin"
            apt.mkdir()
            wrong_compiler = apt / "clang++-22"
            wrong_compiler.write_text("wrong patch", encoding="utf-8")
            wrong_compiler.chmod(0o755)
            with mock.patch.dict(self.bootstrap.os.environ, {"GITHUB_ENV": str(environment), "GITHUB_PATH": str(path_file)}):
                self.bootstrap.bind_clang22_ci_environment(prefix, selectors)
                self.bootstrap.bind_clang22_ci_environment(prefix, selectors)
            selected = shutil.which("clang++-22", path=f"{selectors}:{prefix / 'bin'}:{apt}")
            self.assertEqual(pathlib.Path(selected).resolve(), prefix / "bin/clang++")
            self.assertEqual((selectors / "clang-format-22").resolve(), prefix / "bin/clang-format")
            self.assertEqual((selectors / "llvm-symbolizer-22").resolve(), prefix / "bin/llvm-symbolizer")
            self.assertIn(f"LLVM_DIR={prefix / 'lib/cmake/llvm'}\n", environment.read_text())
            self.assertIn(f"Clang_DIR={prefix / 'lib/cmake/clang'}\n", environment.read_text())
            self.assertEqual(path_file.read_text().splitlines()[:2], [str(selectors), str(prefix / "bin")])

    def test_ci_selectors_reject_missing_tools_and_conflicts_before_writing(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            directory = pathlib.Path(raw_directory)
            prefix = directory / "original"
            selectors = directory / "selectors"
            environment = directory / "github-env"
            path_file = directory / "github-path"
            self.make_original_tools(prefix)
            (prefix / "bin/clang-tidy").unlink()
            with mock.patch.dict(self.bootstrap.os.environ, {"GITHUB_ENV": str(environment), "GITHUB_PATH": str(path_file)}):
                with self.assertRaisesRegex(self.bootstrap.ToolchainError, "tool is missing"):
                    self.bootstrap.bind_clang22_ci_environment(prefix, selectors)
                self.assertFalse(selectors.exists())
                self.assertFalse(environment.exists())
                (prefix / "bin/clang-tidy").write_text("tool", encoding="utf-8")
                selectors.mkdir()
                (selectors / "clang++-22").symlink_to(prefix / "bin/clang")
                with self.assertRaisesRegex(self.bootstrap.ToolchainError, "selector conflicts"):
                    self.bootstrap.bind_clang22_ci_environment(prefix, selectors)
                self.assertFalse(environment.exists())
                self.assertFalse(path_file.exists())

    def test_archive_checksum_mismatch_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            archive = pathlib.Path(raw_directory) / "archive"
            archive.write_bytes(b"not-gcc")
            with self.assertRaisesRegex(
                self.bootstrap.ToolchainError, "checksum mismatch"
            ):
                self.bootstrap.verify_file(
                    archive, "sha512", self.lock["gcc"]["source_sha512"], "GCC source"
                )
            with self.assertRaisesRegex(
                self.bootstrap.ToolchainError, "checksum mismatch"
            ):
                self.bootstrap.verify_file(
                    archive,
                    "sha256",
                    self.lock["clang_gcc_replay"]["asset_sha256"],
                    "Clang GCC replay asset",
                )

    def test_download_declared_size_mismatch_fails_before_body_read(self) -> None:
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.headers = {"Content-Length": "9"}
        lock = json.loads(json.dumps(self.lock))
        lock["gcc"]["source_archive_bytes"] = 8
        with tempfile.TemporaryDirectory() as raw_directory:
            destination = pathlib.Path(raw_directory) / "gcc.tar.xz"
            with mock.patch.object(
                self.bootstrap.urllib.request, "urlopen", return_value=response
            ), self.assertRaisesRegex(
                self.bootstrap.ToolchainError, "declared byte count mismatch"
            ):
                self.bootstrap.download_source(destination, lock)
        response.read.assert_not_called()

    def test_clang_download_declared_size_mismatch_fails_before_body_read(self) -> None:
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.headers = {"Content-Length": "9"}
        lock = json.loads(json.dumps(self.lock))
        lock["clang_gcc_replay"]["asset_archive_bytes"] = 8
        with tempfile.TemporaryDirectory() as raw_directory:
            destination = pathlib.Path(raw_directory) / "clang.tar.xz"
            with mock.patch.object(
                self.bootstrap.urllib.request, "urlopen", return_value=response
            ), self.assertRaisesRegex(
                self.bootstrap.ToolchainError, "declared byte count mismatch"
            ):
                self.bootstrap.download_clang_gcc_replay(destination, lock)
        response.read.assert_not_called()

    def test_clang_archive_with_an_unexpected_root_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            directory = pathlib.Path(raw_directory)
            archive = directory / "clang.tar.xz"
            with tarfile.open(archive, mode="w:xz") as output:
                member = tarfile.TarInfo("unexpected-root/bin/clang++")
                payload = b"not-a-compiler"
                member.size = len(payload)
                output.addfile(member, io.BytesIO(payload))
            lock = json.loads(json.dumps(self.lock))
            lock["clang_gcc_replay"]["asset_archive_bytes"] = archive.stat().st_size
            with mock.patch.object(
                self.bootstrap, "load_lock", return_value=lock
            ), mock.patch.object(self.bootstrap, "assert_runner"), mock.patch.object(
                self.bootstrap, "verify_file"
            ):
                with self.assertRaisesRegex(
                    self.bootstrap.ToolchainError, "archive root mismatch"
                ):
                    self.bootstrap.install_clang_gcc_replay(
                        directory / "prefix", archive
                    )

    def test_clang_archive_traversal_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            directory = pathlib.Path(raw_directory)
            archive = directory / "clang.tar.xz"
            archive_root = self.lock["clang_gcc_replay"]["archive_root"]
            with tarfile.open(archive, mode="w:xz") as output:
                member = tarfile.TarInfo(f"{archive_root}/../escaped")
                payload = b"must-not-escape"
                member.size = len(payload)
                output.addfile(member, io.BytesIO(payload))
            lock = json.loads(json.dumps(self.lock))
            lock["clang_gcc_replay"]["asset_archive_bytes"] = archive.stat().st_size
            with mock.patch.object(
                self.bootstrap, "load_lock", return_value=lock
            ), mock.patch.object(self.bootstrap, "assert_runner"), mock.patch.object(
                self.bootstrap, "verify_file"
            ):
                with self.assertRaisesRegex(
                    self.bootstrap.ToolchainError, "could not extract"
                ):
                    self.bootstrap.install_clang_gcc_replay(
                        directory / "prefix", archive
                    )
            self.assertFalse((directory / "escaped").exists())


if __name__ == "__main__":
    unittest.main()
