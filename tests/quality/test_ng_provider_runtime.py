#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_provider_runtime as provider_runtime  # noqa: E402


class NgProviderRuntimeTests(unittest.TestCase):
    def test_repository_contract(self) -> None:
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/quality/check_ng_provider_runtime.py"),
                "check",
                "--root",
                str(ROOT),
            ],
            check=True,
        )

    def test_task_codec_requires_exactly_one_task_v3_marker(self) -> None:
        codec = (ROOT / "src/llvm/clang22/provider_task_v3.cpp").read_text(
            encoding="utf-8"
        )
        provider_runtime.validate_task_codec_markers(codec)
        for drift in (
            codec.replace(provider_runtime.WORKER_TASK_CODEC_V3, "task-v3-missing"),
            codec + "\n" + provider_runtime.WORKER_TASK_CODEC_V3,
        ):
            with self.assertRaisesRegex(
                provider_runtime.ContractError, "exactly one installed task.v3"
            ):
                provider_runtime.validate_task_codec_markers(drift)

    def test_task_codec_rejects_legacy_task_v2_alias(self) -> None:
        codec = (ROOT / "src/llvm/clang22/provider_task_v3.cpp").read_text(
            encoding="utf-8"
        )
        with self.assertRaisesRegex(
            provider_runtime.ContractError, "task.v2 codec remains adoptable"
        ):
            provider_runtime.validate_task_codec_markers(
                codec + "\n" + provider_runtime.LEGACY_WORKER_TASK_CODEC_V2
            )


if __name__ == "__main__":
    unittest.main()
