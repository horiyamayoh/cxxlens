#!/usr/bin/env python3
"""Focused tests for the Clang 22 scale-evidence boundary."""

from __future__ import annotations

import json
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

    def test_failed_installed_process_does_not_claim_a_success_receipt(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            stdout = root / "stdout"
            stdout.write_text(
                '{"response_kind":"compact_failure","result":"failed",'
                '"task_results":null}',
                encoding="utf-8",
            )
            process = {"status": "failed"}
            scale.attach_installed_input_transfer_receipt(process, stdout)
            self.assertNotIn("input_transfer", process)

    def test_installed_success_receipt_is_bound_to_the_producer_task_result(self) -> None:
        receipt = {
            "protocol_version": "1.1.0",
            "required_feature": "task-input-chunks-v1",
            "task_input_codec": "cxxlens.clang22.task.v3",
            "logical_input_bytes": 1,
            "logical_input_digest": "sha256:" + "a" * 64,
            "canonical_chunk_bytes": 1 << 20,
            "chunk_count": 1,
            "ordered_chunk_payload_digest_set_digest": "semantic-v2:sha256:" + "b" * 64,
        }
        response = {
            "response_kind": "detailed",
            "result": "passed",
            "task_results": [{"input_transfer": receipt}],
        }
        with tempfile.TemporaryDirectory() as directory:
            stdout = pathlib.Path(directory) / "stdout"
            stdout.write_text(json.dumps(response), encoding="utf-8")
            process = {"status": "passed"}
            scale.attach_installed_input_transfer_receipt(process, stdout)
            self.assertEqual(
                process["input_transfer"],
                checker.input_transfer_receipt_from_response(response, "one-task"),
            )

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

    def test_run_marker_binds_terminal_scenario_results_to_the_tree(self) -> None:
        marker = scale.new_run_marker(
            ROOT,
            ROOT / "build" / "driver",
            ROOT / "evidence.json",
        )
        marker["status"] = "passed"
        marker["exit_status"] = 0
        marker["phase"] = "complete"
        marker["current_scenario"] = None
        marker["scenarios"] = [
            {
                "id": scenario_id,
                "expected": (
                    "reject" if scenario_id == "raw-request-limit-plus-one" else "pass"
                ),
                "status": "passed",
                "input": "generated",
                "admission": (
                    "expected-rejection"
                    if scenario_id == "raw-request-limit-plus-one"
                    else "passed"
                ),
                "installed": None,
            }
            for scenario_id in checker.REQUIRED_SCENARIOS
        ]
        with tempfile.TemporaryDirectory() as directory:
            marker_path = pathlib.Path(directory) / "scale-failure.json"
            scale.write_run_marker(marker_path, marker)
            checked = checker.check_run_marker(ROOT, marker_path)
            self.assertEqual(checked["status"], "passed")

    def test_failed_run_marker_identifies_the_owning_scenario(self) -> None:
        marker = scale.new_run_marker(ROOT, ROOT / "driver", ROOT / "evidence.json")
        marker["status"] = "failed"
        marker["exit_status"] = 1
        marker["phase"] = "complete"
        marker["current_scenario"] = None
        marker["scenarios"] = [
            {
                "id": "one-task",
                "expected": "pass",
                "status": "failed",
                "input": "generated",
                "admission": "failed",
                "installed": None,
            }
        ]
        marker["failure"] = {
            "phase": "scenario-result",
            "scenario_id": "one-task",
            "reason": "process-boundary-failed",
        }
        with tempfile.TemporaryDirectory() as directory:
            marker_path = pathlib.Path(directory) / "scale-failure.json"
            scale.write_run_marker(marker_path, marker)
            checked = checker.check_run_marker(ROOT, marker_path)
            self.assertEqual(checked["failure"]["scenario_id"], "one-task")

    def test_nightly_scale_job_uploads_failures_and_has_a_final_gate(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "nightly.yml").read_text(
            encoding="utf-8"
        )
        scale_job = workflow.split("  materialization-scale:\n", 1)[1].split(
            "  evidence-ownership:\n", 1
        )[0]
        harness_source = (ROOT / "tests" / "install" / "clang22_materializer_scale_test.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("--failure-marker", scale_job)
        self.assertIn("--run-marker", scale_job)
        self.assertIn("materialization scale scenario start", harness_source)
        self.assertIn("materialization scale scenario result", harness_source)
        self.assertIn("if: always()", scale_job)
        self.assertIn("name: Enforce scale evidence result", scale_job)
        self.assertIn("cxxlens-materialization-scale-failure.json", scale_job)
        self.assertIn("if-no-files-found: warn", scale_job)
        self.assertIn("HARNESS_STATUS: ${{ steps.materialization-scale-harness.outputs.exit_status }}", scale_job)
        self.assertIn("CHECKER_STATUS: ${{ steps.materialization-scale-check.outputs.exit_status }}", scale_job)
        self.assertIn("scale evidence ownership was not generated", scale_job)
        self.assertIn('if [[ "${HARNESS_STATUS}" != "0" || "${CHECKER_STATUS}" != "0" ]]', scale_job)
        self.assertNotIn("continue-on-error:", scale_job)


if __name__ == "__main__":
    unittest.main()
