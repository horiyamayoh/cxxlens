#!/usr/bin/env python3
"""CI policy regression tests."""

from __future__ import annotations

import pathlib
import sys
import unittest
from collections.abc import Callable, Iterator
from contextlib import contextmanager

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ci_supply_chain import CiSupplyChainError, check  # noqa: E402


class CiWorkflowTest(unittest.TestCase):
    @contextmanager
    def _temporary_release(self, transform: Callable[[str], str]) -> Iterator[None]:
        workflow = ROOT / ".github/workflows/release.yml"
        original = workflow.read_text(encoding="utf-8")
        workflow.write_text(transform(original), encoding="utf-8")
        try:
            yield
        finally:
            workflow.write_text(original, encoding="utf-8")

    def test_current_workflows_are_test_only(self) -> None:
        check(ROOT)

    def test_obsolete_artifact_upload_is_denied(self) -> None:
        workflow = ROOT / ".github/workflows/quality.yml"
        original = workflow.read_text(encoding="utf-8")
        try:
            workflow.write_text(original + "\nuses: actions/upload-artifact@deadbeef\n", encoding="utf-8")
            with self.assertRaises(CiSupplyChainError):
                check(ROOT)
        finally:
            workflow.write_text(original, encoding="utf-8")

    def test_every_release_test_job_is_required(self) -> None:
        with self._temporary_release(lambda text: text.replace("static-analysis", "static_check")):
            with self.assertRaises(CiSupplyChainError):
                check(ROOT)

    def test_package_must_wait_for_maximum_scale(self) -> None:
        def remove_scale(text: str) -> str:
            return text.replace("stress-and-repeat, maximum-scale, real-projects", "stress-and-repeat, real-projects", 1)

        with self._temporary_release(remove_scale):
            with self.assertRaises(CiSupplyChainError):
                check(ROOT)

    def test_release_cannot_write_runner_report(self) -> None:
        with self._temporary_release(
            lambda text: text + '\n          run: echo --output "$RUNNER_TEMP/report.json"\n'
        ):
            with self.assertRaises(CiSupplyChainError):
                check(ROOT)

    def test_release_contract_and_header_jobs_must_run_direct_checks(self) -> None:
        with self._temporary_release(
            lambda text: text.replace(
                "cmake --build --preset docs --target cxxlens-quality",
                "cmake --build --preset docs --target cxxlens-quality-removed",
                1,
            )
        ):
            with self.assertRaises(CiSupplyChainError):
                check(ROOT)
        with self._temporary_release(
            lambda text: text.replace("g++ -std=c++23", "cc -std=c++23")
        ):
            with self.assertRaises(CiSupplyChainError):
                check(ROOT)

    def test_release_cannot_bypass_failed_test_job(self) -> None:
        with self._temporary_release(lambda text: text + "\n  if: ${{ always() }}\n"):
            with self.assertRaises(CiSupplyChainError):
                check(ROOT)

    def test_scale_and_real_projects_cover_static_and_shared(self) -> None:
        def remove_matrix(text: str) -> str:
            return text.replace('        shared: ["OFF", "ON"]\n    steps:', '        shared: ["OFF"]\n    steps:', 2)

        with self._temporary_release(remove_matrix):
            with self.assertRaises(CiSupplyChainError):
                check(ROOT)

    def test_real_projects_selects_both_ctest_labels(self) -> None:
        with self._temporary_release(
            lambda text: text.replace(
                "'^(install|integration)$'", "'^(install|integration)\\\\.'", 1
            )
        ):
            with self.assertRaises(CiSupplyChainError):
                check(ROOT)


if __name__ == "__main__":
    unittest.main()
