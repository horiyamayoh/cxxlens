#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import pathlib
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


if __name__ == "__main__":
    unittest.main()
