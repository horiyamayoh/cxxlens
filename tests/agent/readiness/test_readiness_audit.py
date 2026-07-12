#!/usr/bin/env python3
"""Independent final-readiness audit and corruption tests."""

from __future__ import annotations

import concurrent.futures
import copy
import json
import pathlib
import subprocess
import sys
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "agent"))

from ownership_generator import OwnershipError  # noqa: E402
from ready_evaluator import ReadyError, digest  # noqa: E402
from readiness_audit import (  # noqa: E402
    AuditError,
    generate_authorization,
    load_inputs,
    validate_authorization,
)
from task_packet_generator import TaskPacketError  # noqa: E402


def process_digest(root: str) -> str:
    path = pathlib.Path(root)
    return generate_authorization(load_inputs(path), path)["semantic_digest"]


class ReadinessAuditTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.inputs = load_inputs(ROOT)
        cls.schema = cls.inputs["authorization_schema"]
        cls.authorization = json.loads(
            (ROOT / "schemas/cxxlens.readiness.authorization.v1.json").read_text(
                encoding="utf-8"
            )
        )
        cls.negative_cases = yaml.safe_load(
            (
                ROOT
                / "tests/agent/readiness/fixtures/negative_cases.yaml"
            ).read_text(encoding="utf-8")
        )["cases"]

    def fresh_inputs(self) -> dict:
        return copy.deepcopy(self.inputs)

    @staticmethod
    def resign(report: dict) -> None:
        unsigned = copy.deepcopy(report)
        unsigned.pop("semantic_digest", None)
        report["semantic_digest"] = digest(unsigned)

    def assert_code(self, expected: str, action) -> None:
        with self.assertRaises(
            (AuditError, OwnershipError, ReadyError, TaskPacketError)
        ) as raised:
            action()
        self.assertEqual(raised.exception.code, expected)

    def test_full_manifest_is_truthful_complete_and_schema_valid(self) -> None:
        generated = generate_authorization(self.fresh_inputs(), ROOT)
        self.assertEqual(generated, self.authorization)
        validate_authorization(generated, generated, self.schema)
        self.assertEqual(generated["summary"]["package_count"], 22)
        self.assertEqual(generated["summary"]["api_count"], 124)
        self.assertEqual(generated["summary"]["unit_count"], 124)
        self.assertEqual(
            generated["summary"]["unit_state_counts"],
            {"blocked": 77, "complete": 47, "ready": 0},
        )
        self.assertEqual(generated["authorization"]["decision"], "denied")
        self.assertEqual(generated["authorization"]["ready_waves"], [])
        self.assertEqual(len({api["api_id"] for api in generated["apis"]}), 124)
        self.assertEqual(
            len({unit["atomic_unit_id"] for unit in generated["units"]}), 124
        )
        self.assertTrue(
            all(unit["blockers"] for unit in generated["units"] if unit["state"] == "blocked")
        )
        self.assertNotIn(str(ROOT), json.dumps(generated, sort_keys=True))

    def test_catalog_ownership_dag_and_foundation_zero_defect_counts(self) -> None:
        catalog = self.authorization["catalog_audit"]
        self.assertEqual(catalog["duplicate_api_count"], 0)
        self.assertEqual(catalog["dangling_api_dependency_count"], 0)
        self.assertEqual(catalog["unresolved_ready_signature_count"], 0)
        self.assertEqual(catalog["evidence_free_conformant_count"], 0)
        infrastructure = self.authorization["infrastructure_audit"]
        for key in (
            "write_overlap_count",
            "unowned_path_count",
            "cycle_count",
            "dangling_edge_count",
            "provider_ambiguity_count",
        ):
            self.assertEqual(infrastructure[key], 0)
        self.assertEqual(
            [gate["issue"] for gate in self.authorization["foundation_gates"]],
            [12, 22, 26],
        )
        for gate in self.authorization["foundation_gates"]:
            self.assertEqual(gate["required_status"], "success")
            self.assertTrue(all(isinstance(command["argv"], list) for command in gate["replay_commands"]))
        matrix = self.inputs["m2"]["matrix"]
        self.assertEqual(matrix["jobs"], [1, 2, 8])
        self.assertEqual(set(matrix["backends"]), {"memory", "sqlite"})
        self.assertEqual(set(matrix["cache_states"]), {"cold", "warm"})

    def test_prompt_shard_and_dependency_request_dry_runs(self) -> None:
        runs = self.authorization["dry_runs"]
        self.assertEqual(
            {row["kind"] for row in runs["prompts"]},
            {"free_function", "method", "method_family", "builder_family", "static_factory"},
        )
        for row in runs["prompts"]:
            self.assertTrue(row["acceptance_argv"])
            self.assertEqual(row["start_authorized"], row["state"] == "ready")
        self.assertEqual(runs["unknown_prompt"]["code"], "ready.unknown-api")
        self.assertEqual(runs["blocked_prompt"]["code"], "runner.not-ready")
        self.assertTrue(runs["unauthorized_edit"]["before_compilation"])
        exercise = runs["dependency_request"]
        self.assertEqual(exercise["transition_order"], ["pending", "accepted", "resolved"])
        self.assertTrue(exercise["open_blocker_observed"])
        self.assertTrue(exercise["resolved_blocker_removed"])
        self.assertTrue(exercise["report_reissued"])

    def test_process_and_repeated_generation_are_deterministic(self) -> None:
        expected = self.authorization["semantic_digest"]
        self.assertEqual(generate_authorization(self.fresh_inputs(), ROOT)["semantic_digest"], expected)
        with concurrent.futures.ProcessPoolExecutor(max_workers=2) as executor:
            results = [executor.submit(process_digest, str(ROOT)) for _ in range(2)]
            self.assertEqual({result.result() for result in results}, {expected})

    def test_upstream_corruption_fixtures_fail_closed(self) -> None:
        mutators = {
            "catalog_duplicate": self.catalog_duplicate,
            "signature_drift": self.signature_drift,
            "task_omission": self.task_omission,
            "ownership_omission": self.ownership_omission,
            "dangling_dag_edge": self.dangling_dag_edge,
            "maturity_only_ready": self.maturity_only_ready,
        }
        for case in self.negative_cases:
            if case["id"] not in mutators:
                continue
            with self.subTest(case=case["id"]):
                self.assert_code(case["expected_code"], mutators[case["id"]])

    def test_authorization_corruption_and_expiry_fixtures_fail_closed(self) -> None:
        mutators = {
            "authorization_api_omission": self.authorization_api_omission,
            "authorization_without_blocker": self.authorization_without_blocker,
            "false_authorization": self.false_authorization,
            "expired_on_input_drift": self.expired_on_input_drift,
        }
        for case in self.negative_cases:
            if case["id"] not in mutators:
                continue
            with self.subTest(case=case["id"]):
                self.assert_code(case["expected_code"], mutators[case["id"]])

    def test_cli_replays_static_foundation_and_agent_checks(self) -> None:
        completed = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/agent/readiness_audit.py"),
                "check",
                "--root",
                str(ROOT),
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("decision denied", completed.stdout)

    def catalog_duplicate(self) -> None:
        inputs = self.fresh_inputs()
        duplicate = copy.deepcopy(inputs["catalog"]["packages"][0]["apis"][0])
        inputs["catalog"]["packages"][0]["apis"].append(duplicate)
        generate_authorization(inputs, ROOT)

    def signature_drift(self) -> None:
        inputs = self.fresh_inputs()
        inputs["corpus"]["packets"][0]["declaration"]["signature"] += " drift"
        generate_authorization(inputs, ROOT)

    def task_omission(self) -> None:
        inputs = self.fresh_inputs()
        inputs["corpus"]["packets"].pop()
        generate_authorization(inputs, ROOT)

    def ownership_omission(self) -> None:
        inputs = self.fresh_inputs()
        inputs["ownership"]["tracked_paths"].pop()
        generate_authorization(inputs, ROOT)

    def dangling_dag_edge(self) -> None:
        inputs = self.fresh_inputs()
        inputs["ready"]["edges"].append(
            {
                "from": inputs["ready"]["nodes"][0]["atomic_unit_id"],
                "to": "AU-NOT-999",
                "kind": "api_dependency",
                "via_api_ids": [inputs["ready"]["nodes"][0]["api_ids"][0]],
            }
        )
        generate_authorization(inputs, ROOT)

    def maturity_only_ready(self) -> None:
        inputs = self.fresh_inputs()
        node = next(node for node in inputs["ready"]["nodes"] if node["state"] == "blocked")
        node["state"] = "ready"
        generate_authorization(inputs, ROOT)

    def authorization_api_omission(self) -> None:
        report = copy.deepcopy(self.authorization)
        report["apis"].pop()
        self.resign(report)
        validate_authorization(report, self.authorization, self.schema)

    def authorization_without_blocker(self) -> None:
        report = copy.deepcopy(self.authorization)
        unit = next(unit for unit in report["units"] if unit["state"] == "blocked")
        unit["blockers"] = []
        self.resign(report)
        validate_authorization(report, self.authorization, self.schema)

    def false_authorization(self) -> None:
        report = copy.deepcopy(self.authorization)
        report["units"][0]["dispatch_authorized"] = True
        self.resign(report)
        validate_authorization(report, self.authorization, self.schema)

    def expired_on_input_drift(self) -> None:
        report = copy.deepcopy(self.authorization)
        report["input_fingerprints"]["catalog"] = "sha256:" + "0" * 64
        self.resign(report)
        validate_authorization(report, self.authorization, self.schema)


if __name__ == "__main__":
    unittest.main()
