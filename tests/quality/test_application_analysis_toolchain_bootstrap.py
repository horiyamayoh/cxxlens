#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import io
import json
import pathlib
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
        self.assertEqual(admitted["msvc"]["exact_version"], "19.51.36247")
        self.assertEqual(
            admitted["windows_sdk"]["exact_version"], "10.0.28000.2705"
        )
        self.assertEqual(
            admitted["windows_runner"]["label"], "windows-2025-vs2026"
        )
        self.assertEqual(admitted["runner"]["label"], "ubuntu-24.04")

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
