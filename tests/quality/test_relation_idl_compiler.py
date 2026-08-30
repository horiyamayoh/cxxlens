#!/usr/bin/env python3
"""Reproducibility tests for the generated public relation headers."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
COMPILER = ROOT / "tools/sdk/relation_idl_compiler.py"
RELATION_HEADERS = ROOT / "include/cxxlens/relations"


class RelationIdlCompilerTest(unittest.TestCase):
    def run_compiler(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(COMPILER), *arguments],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_committed_headers_match_registry_byte_for_byte(self) -> None:
        completed = self.run_compiler("--all", "--check")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertRegex(
            completed.stdout, r"checked \d+ generated relation headers in "
        )

    def test_all_generation_recreates_only_static_relation_headers(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-relation-idl-") as directory:
            output_dir = pathlib.Path(directory)
            completed = self.run_compiler("--all", "--output-dir", str(output_dir))
            self.assertEqual(completed.returncode, 0, completed.stderr)

            generated = {path.name for path in output_dir.glob("*.hpp")}
            committed = {path.name for path in RELATION_HEADERS.glob("*.hpp")}
            self.assertEqual(generated, committed)
            for filename in sorted(committed):
                self.assertEqual(
                    (output_dir / filename).read_bytes(),
                    (RELATION_HEADERS / filename).read_bytes(),
                    filename,
                )

    def test_check_rejects_header_drift_and_extra_headers(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-relation-idl-") as directory:
            output_dir = pathlib.Path(directory)
            generated = self.run_compiler("--all", "--output-dir", str(output_dir))
            self.assertEqual(generated.returncode, 0, generated.stderr)

            header = output_dir / "build_project.hpp"
            header.write_bytes(header.read_bytes() + b"\n")
            drift = self.run_compiler(
                "--all", "--output-dir", str(output_dir), "--check"
            )
            self.assertEqual(drift.returncode, 1)
            self.assertIn("generated relation header differs", drift.stderr)
            self.assertIn(str(header), drift.stderr)

            header.write_bytes((RELATION_HEADERS / header.name).read_bytes())
            (output_dir / "orphan.hpp").write_text("#pragma once\n", encoding="utf-8")
            extra = self.run_compiler(
                "--all", "--output-dir", str(output_dir), "--check"
            )
            self.assertEqual(extra.returncode, 1)
            self.assertIn("unexpected relation header", extra.stderr)
            self.assertIn("orphan.hpp", extra.stderr)


if __name__ == "__main__":
    unittest.main()
