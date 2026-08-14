#!/usr/bin/env python3
"""Focused tests for the Clang 22 scale-evidence boundary."""

from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "install"))
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import clang22_materializer_scale_test as scale  # noqa: E402
import check_ng_clang22_materialization_scale as checker  # noqa: E402


class NgClang22MaterializationScaleTests(unittest.TestCase):
    def test_authority_constants_are_limit_adjacent(self) -> None:
        self.assertEqual(scale.RAW_INPUT_LIMIT_BYTES, 1 << 30)
        self.assertEqual(scale.MAXIMUM_TASK_INPUT_BYTES, 64 << 20)
        self.assertEqual(scale.MAXIMUM_TASK_COUNT, 4096)
        self.assertEqual(scale.MAXIMUM_AGGREGATE_SOURCE_BYTES, 512 << 20)
        self.assertEqual(scale.SOURCE_CHUNK_BYTES, 16 << 20)
        self.assertEqual(
            len(scale.distinct_small_source(0)),
            len(scale.distinct_small_source(4095)),
        )
        self.assertEqual(
            scale.padded_source(scale.SOURCE_CHUNK_BYTES),
            scale.padded_source(scale.SOURCE_CHUNK_BYTES),
        )
        self.assertEqual(len(scale.padded_source(scale.SOURCE_CHUNK_BYTES)), 16 << 20)

    def test_checker_rejects_non_pass_process_observation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            input_path = root / "input"
            input_path.write_bytes(b"fixture\n")
            process = scale.run_process(
                [
                    sys.executable,
                    "-c",
                    "import sys; sys.stdin.buffer.read(); sys.stdout.write('ok\\n')",
                ],
                input_path,
                root / "run",
                fragmented=False,
                kind="admission",
                artifact_root=root,
            )
            process["observation"] = "driver-error"
            with self.assertRaises(checker.ScaleEvidenceError):
                checker.check_process(
                    root,
                    root / "evidence.json",
                    process,
                    expected="pass",
                    installed=False,
                    scenario_id="one-task",
                )

    def test_wait4_runner_records_fragmented_input_and_peak_rss(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            input_path = root / "input"
            input_path.write_bytes(b"fixture\n")
            output = scale.run_process(
                [
                    sys.executable,
                    "-c",
                    "import sys; sys.stdin.buffer.read(); sys.stdout.write('ok\\n')",
                ],
                input_path,
                root / "run",
                fragmented=True,
                kind="admission",
                artifact_root=root,
            )
            self.assertEqual(output["actual_exit_status"], 0)
            self.assertEqual(output["observation"], "driver-ok")
            self.assertGreater(output["peak_rss_bytes"], 0)

    def test_valid_raw_boundary_fixture_uses_only_json_whitespace(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "request.json"
            size, _digest = scale.write_padded_json(path, {"ok": True}, 4096)
            self.assertEqual(size, 4096)
            self.assertEqual(path.read_bytes().rstrip(), b'{"ok":true}')

    def test_retained_input_artifact_path_is_relative_to_evidence_root(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "work" / "request.json"
            source.parent.mkdir()
            source.write_bytes(b'{"request":true}')
            report = root / "evidence.json"
            artifact = scale.copy_if_requested(source, report, True)
            self.assertIsNotNone(artifact)
            assert artifact is not None
            self.assertEqual(artifact["path"], "inputs/request.json")
            self.assertEqual(checker.artifact_bytes(report, artifact), source.read_bytes())

    def test_raw_limit_rejection_requires_exact_driver_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            process_root = root / "raw-limit.process"
            process_root.mkdir()
            stdout_path = process_root / "stdout"
            stderr_path = process_root / "stderr"
            stdout_path.write_bytes(
                b"materialization.request-invalid|input-limit|maximum-bytes\n"
            )
            stderr_path.write_bytes(b"")
            stdout_size, stdout_digest = scale.digest_file(stdout_path)
            stderr_size, stderr_digest = scale.digest_file(stderr_path)
            process = {
                "status": "expected-rejection",
                "observation": "driver-error",
                "actual_exit_status": 1,
                "stdout_byte_count": stdout_size,
                "stdout_sha256": stdout_digest,
                "stderr_byte_count": stderr_size,
                "stderr_sha256": stderr_digest,
                "parsed_response_count": 0,
                "peak_rss_bytes": 1,
                "stdout_artifact": {
                    "path": "raw-limit.process/stdout",
                    "byte_count": stdout_size,
                    "sha256": stdout_digest,
                },
                "stderr_artifact": {
                    "path": "raw-limit.process/stderr",
                    "byte_count": stderr_size,
                    "sha256": stderr_digest,
                },
            }
            checker.check_process(
                root,
                root / "evidence.json",
                process,
                expected="reject",
                installed=False,
                scenario_id="raw-request-limit-plus-one",
            )
            stdout_path.write_bytes(b"materialization.request-invalid|json|syntax\n")
            with self.assertRaises(checker.ScaleEvidenceError):
                checker.check_process(
                    root,
                    root / "evidence.json",
                    process,
                    expected="reject",
                    installed=False,
                    scenario_id="raw-request-limit-plus-one",
                )


if __name__ == "__main__":
    unittest.main()
