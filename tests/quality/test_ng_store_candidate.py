#!/usr/bin/env python3
"""Negative tests for the source-private bounded Store candidate binding."""

from __future__ import annotations

import pathlib
import shutil
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))
from check_ng_store_candidate import (  # noqa: E402
    BUILD,
    HEADER,
    SOURCE,
    TEST,
    TEST_BUILD,
    StoreCandidateError,
    validate,
)


class StoreCandidateTest(unittest.TestCase):
    def copied_root(self, temporary: str) -> pathlib.Path:
        root = pathlib.Path(temporary)
        for relative in (HEADER, SOURCE, TEST, BUILD, TEST_BUILD):
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        return root

    def test_repository_binding_is_valid(self) -> None:
        validate(ROOT)

    def test_projection_comparator_token_cannot_disappear(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / SOURCE
            path.write_text(
                path.read_text(encoding="utf-8").replace("full-byte-mismatch", "digest-only"),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(StoreCandidateError, "candidate source"):
                validate(root)

    def test_constant_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / HEADER
            path.write_text(
                path.read_text(encoding="utf-8").replace("28'321'546U", "28'321'545U"),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(StoreCandidateError, "bounded constant drift"):
                validate(root)

    def test_test_registration_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / TEST_BUILD
            path.write_text(
                path.read_text(encoding="utf-8").replace(
                    "adapter.clang22-materialization-store-candidate", "adapter.removed"
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(StoreCandidateError, "candidate test registration"):
                validate(root)


if __name__ == "__main__":
    unittest.main()
