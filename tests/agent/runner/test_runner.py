#!/usr/bin/env python3
"""DAG, ready predicate, prompt, shard, and dependency-request tests."""

from __future__ import annotations

import concurrent.futures
import copy
import json
import pathlib
import subprocess
import sys
import unittest

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "agent"))

from api_task_runner import (  # noqa: E402
    RunnerError,
    authorize_integration,
    authorize_run,
)
from ownership_generator import transition_request  # noqa: E402
from ready_evaluator import (  # noqa: E402
    ReadyError,
    generate_report,
    parse_prompt,
    resolve_api,
    validate_report,
)


def process_report_digest(inputs: tuple[dict, ...]) -> str:
    return generate_report(*inputs)["semantic_digest"]


class RunnerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.corpus = cls.read_json("schemas/cxxlens.agent-task-packet-corpus.v1.json")
        cls.ownership = cls.read_json("schemas/cxxlens.agent-ownership.v1.json")
        cls.requests = cls.read_json("schemas/cxxlens.dependency-request.examples.v1.json")
        cls.report = cls.read_json("schemas/cxxlens.api-ready.report.v1.json")
        cls.m0 = cls.read_yaml("schemas/cxxlens_m0_completion.yaml")
        cls.m1 = cls.read_yaml("schemas/cxxlens_m1_completion.yaml")
        cls.m2 = cls.read_yaml("schemas/cxxlens_m2_completion.yaml")
        cls.schema = cls.read_yaml("schemas/cxxlens.api-ready.v1.schema.yaml")
        cls.run_schema = cls.read_yaml(
            "schemas/cxxlens.api-ready.run-manifest.v1.schema.yaml"
        )
        cls.negative_cases = cls.read_yaml(
            "tests/agent/runner/fixtures/negative_cases.yaml"
        )["cases"]

    @staticmethod
    def read_json(relative: str) -> dict:
        return json.loads((ROOT / relative).read_text(encoding="utf-8"))

    @staticmethod
    def read_yaml(relative: str) -> dict:
        return yaml.safe_load((ROOT / relative).read_text(encoding="utf-8"))

    def inputs(self) -> tuple[dict, ...]:
        return (
            copy.deepcopy(self.corpus),
            copy.deepcopy(self.ownership),
            copy.deepcopy(self.m0),
            copy.deepcopy(self.m1),
            copy.deepcopy(self.m2),
            copy.deepcopy(self.requests),
        )

    def generated(self, inputs: tuple[dict, ...] | None = None) -> dict:
        return generate_report(*(inputs or self.inputs()))

    def assert_code(self, code: str, action) -> None:
        with self.assertRaises((ReadyError, RunnerError)) as raised:
            action()
        self.assertEqual(raised.exception.code, code)

    def test_full_dag_schema_states_shards_and_exactly_once_resolution(self) -> None:
        generated = self.generated()
        self.assertEqual(generated, self.report)
        validate_report(generated, generated, self.schema)
        jsonschema.Draft202012Validator(self.schema).validate(generated)
        self.assertEqual(generated["summary"]["api_count"], 124)
        self.assertEqual(generated["summary"]["unit_count"], 124)
        self.assertEqual(generated["summary"]["package_count"], 22)
        self.assertEqual(
            generated["summary"]["state_counts"],
            {"blocked": 77, "complete": 47, "ready": 0},
        )
        self.assertEqual(generated["summary"]["ready_waves"], [])
        wave_units = [unit for wave in generated["topological_waves"] for unit in wave]
        node_units = [node["atomic_unit_id"] for node in generated["nodes"]]
        self.assertEqual(sorted(wave_units), node_units)
        self.assertEqual(len(wave_units), len(set(wave_units)))
        self.assertEqual(
            {shard["atomic_unit_id"] for shard in generated["shards"]},
            set(node_units),
        )
        self.assertEqual(len(generated["package_integration_shards"]), 22)
        integrated_units = {
            unit_id
            for shard in generated["package_integration_shards"]
            for unit_id in shard["blocked_atomic_units"]
            + [
                artifact["atomic_unit_id"]
                for artifact in shard["conformant_unit_artifacts"]
            ]
        }
        self.assertEqual(integrated_units, set(node_units))
        self.assertNotIn(str(ROOT), json.dumps(generated, sort_keys=True))

    def test_typed_providers_blockers_and_dependency_chains_are_visible(self) -> None:
        providers = {
            kind: {item["id"]: item["state"] for item in self.report["providers"][kind]}
            for kind in ("facts", "capabilities")
        }
        self.assertEqual(providers["capabilities"]["interop.clang"], "available")
        self.assertEqual(providers["capabilities"]["include.cleaner"], "unavailable")
        blocked = [node for node in self.report["nodes"] if node["state"] == "blocked"]
        self.assertTrue(all(node["blockers"] for node in blocked))
        self.assertTrue(
            any(
                blocker["code"]
                in {"fact-provider-unavailable", "capability-provider-unavailable"}
                for node in blocked
                for blocker in node["blockers"]
            )
        )
        for node in blocked:
            for blocker in node["blockers"]:
                self.assertEqual(blocker["chain"][0], node["atomic_unit_id"])
                self.assertTrue(blocker["steward"])

    def test_representative_minimal_prompts_resolve_one_constrained_unit(self) -> None:
        kinds = ["free_function", "method", "method_family", "builder_family", "static_factory"]
        for kind in kinds:
            packet = next(item for item in self.corpus["packets"] if item["kind"] == kind)
            prompt = f"{packet['api_id']} を実装してください"
            self.assertEqual(parse_prompt(prompt), packet["api_id"])
            resolution = resolve_api(
                packet["api_id"], self.report, self.corpus, self.ownership
            )
            jsonschema.Draft202012Validator(self.run_schema).validate(resolution)
            self.assertEqual(resolution["atomic_unit_id"], packet["atomic_unit_id"])
            self.assertTrue(resolution["allowed_write_prefixes"])
            self.assertIsInstance(resolution["acceptance_command"]["argv"], list)
            self.assertFalse(resolution["start_authorized"])

    def test_shards_include_declared_fixtures_and_non_bypassable_global_gates(self) -> None:
        required_gates = {"task-packets", "ownership", "configure", "build", "test", "quality"}
        for shard in self.report["shards"]:
            self.assertEqual(
                set(shard["fixture_categories"]),
                {"positive", "negative", "ambiguous"},
            )
            self.assertEqual(set(shard["mandatory_gate_ids"]), required_gates)
            self.assertTrue(all(isinstance(command["argv"], list) for command in shard["commands"]))
            self.assertEqual(
                shard["acceptance_command"]["argv"][-1], shard["atomic_unit_id"]
            )
        node_states = {
            node["atomic_unit_id"]: node["state"] for node in self.report["nodes"]
        }
        for integration in self.report["package_integration_shards"]:
            self.assertEqual(set(integration["mandatory_gate_ids"]), required_gates)
            self.assertTrue(
                all(isinstance(command["argv"], list) for command in integration["commands"])
            )
            self.assertEqual(
                integration["acceptance_command"]["argv"][-1],
                integration["package_id"],
            )
            self.assertTrue(
                all(
                    node_states[artifact["atomic_unit_id"]] == "complete"
                    for artifact in integration["conformant_unit_artifacts"]
                )
            )
            self.assertTrue(
                all(
                    node_states[unit_id] != "complete"
                    for unit_id in integration["blocked_atomic_units"]
                )
            )

    def test_package_integration_runner_accepts_only_conformant_inputs_and_owner(self) -> None:
        verification = next(
            shard
            for shard in self.report["package_integration_shards"]
            if shard["state"] == "verification" and shard["allowed_write_paths"]
        )
        accepted = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "agent" / "api_task_runner.py"),
                "integrate",
                "--root",
                str(ROOT),
                "--package-id",
                verification["package_id"],
                "--changed-path",
                verification["allowed_write_paths"][0],
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        other_path = next(
            tracked["path"]
            for tracked in self.ownership["tracked_paths"]
            if tracked["owner_role"].startswith("integration.package.")
            and tracked["owner_role"] != verification["package_integration_role"]
        )
        rejected = subprocess.run(
            accepted.args + ["--changed-path", other_path],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("ownership.unauthorized-path", rejected.stderr)
        blocked = next(
            shard
            for shard in self.report["package_integration_shards"]
            if shard["state"] == "blocked"
        )
        self.assert_code(
            "runner.package-integration-blocked",
            lambda: authorize_integration(blocked),
        )

    def test_dependency_request_block_resolve_and_reissue_is_deterministic(self) -> None:
        pending = self.requests["requests"][0]
        unit_id = pending["requesting_atomic_unit"]
        pending_node = next(
            node for node in self.report["nodes"] if node["atomic_unit_id"] == unit_id
        )
        self.assertTrue(
            any(
                blocker["code"] == "dependency-request-open"
                for blocker in pending_node["blockers"]
            )
        )
        accepted = transition_request(pending, "accepted", self.ownership)
        resolved = transition_request(
            accepted,
            "resolved",
            self.ownership,
            ["shared contract published and packet reissued"],
        )
        inputs = list(self.inputs())
        inputs[-1] = {"schema": self.requests["schema"], "requests": [resolved]}
        report_a = self.generated(tuple(inputs))
        report_b = self.generated(tuple(copy.deepcopy(inputs)))
        self.assertEqual(report_a, report_b)
        resolved_node = next(
            node for node in report_a["nodes"] if node["atomic_unit_id"] == unit_id
        )
        self.assertFalse(
            any(
                blocker["code"] == "dependency-request-open"
                for blocker in resolved_node["blockers"]
            )
        )
        self.assertNotEqual(report_a["semantic_digest"], self.report["semantic_digest"])

    def test_order_process_and_cli_determinism(self) -> None:
        inputs = list(self.inputs())
        inputs[0]["packets"].reverse()
        inputs[0]["atomic_units"].reverse()
        inputs[1]["units"].reverse()
        inputs[1]["skeletons"].reverse()
        inputs[3]["fact_kinds"].reverse()
        inputs[3]["vectors"].reverse()
        self.assertEqual(self.generated(tuple(inputs)), self.report)
        with concurrent.futures.ProcessPoolExecutor(max_workers=2) as executor:
            futures = [executor.submit(process_report_digest, self.inputs()) for _ in range(2)]
            self.assertEqual(
                {future.result() for future in futures},
                {self.report["semantic_digest"]},
            )
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "agent" / "ready_evaluator.py"),
                "check",
                "--root",
                str(ROOT),
            ],
            check=True,
            text=True,
            capture_output=True,
        )

    def test_blocked_run_and_unknown_prompt_never_start(self) -> None:
        blocked = next(node for node in self.report["nodes"] if node["state"] == "blocked")
        api_id = blocked["api_ids"][0]
        completed = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "agent" / "api_task_runner.py"),
                "run",
                "--root",
                str(ROOT),
                "--api-id",
                api_id,
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("runner.not-ready", completed.stderr)
        self.assert_code(
            "ready.unknown-api",
            lambda: resolve_api("API-NOT-999", self.report, self.corpus, self.ownership),
        )

    def test_negative_fixture_codes(self) -> None:
        mutators = {
            "dangling_edge": self.negative_dangling,
            "dependency_cycle": self.negative_cycle,
            "provider_ambiguity": self.negative_provider,
            "maturity_only_ready": self.negative_maturity,
            "unknown_prompt_api": self.negative_unknown,
            "ambiguous_prompt": self.negative_prompt,
            "blocked_run": self.negative_run,
        }
        self.assertEqual({case["id"] for case in self.negative_cases}, set(mutators))
        for case in self.negative_cases:
            with self.subTest(case=case["id"]):
                self.assert_code(case["expected_code"], mutators[case["id"]])

    def negative_dangling(self) -> None:
        report = copy.deepcopy(self.report)
        report["edges"].append(
            {
                "from": report["nodes"][0]["atomic_unit_id"],
                "to": "AU-NOT-999",
                "kind": "api_dependency",
                "via_api_ids": [report["nodes"][0]["api_ids"][0]],
            }
        )
        validate_report(report, self.report, self.schema)

    def negative_cycle(self) -> None:
        report = copy.deepcopy(self.report)
        first, second = report["topological_waves"][0][:2]
        api_id = report["nodes"][0]["api_ids"][0]
        report["edges"].extend(
            [
                {"from": first, "to": second, "kind": "api_dependency", "via_api_ids": [api_id]},
                {"from": second, "to": first, "kind": "api_dependency", "via_api_ids": [api_id]},
            ]
        )
        validate_report(report, self.report, self.schema)

    def negative_provider(self) -> None:
        report = copy.deepcopy(self.report)
        report["providers"]["facts"].append(copy.deepcopy(report["providers"]["facts"][0]))
        validate_report(report, self.report, self.schema)

    def negative_maturity(self) -> None:
        report = copy.deepcopy(self.report)
        node = next(item for item in report["nodes"] if item["state"] == "blocked")
        node["state"] = "ready"
        validate_report(report, self.report, self.schema)

    def negative_unknown(self) -> None:
        resolve_api("API-NOT-999", self.report, self.corpus, self.ownership)

    @staticmethod
    def negative_prompt() -> None:
        parse_prompt("API-CORE-001 and API-CORE-002")

    def negative_run(self) -> None:
        blocked = next(node for node in self.report["nodes"] if node["state"] == "blocked")
        resolution = resolve_api(
            blocked["api_ids"][0], self.report, self.corpus, self.ownership
        )
        authorize_run(resolution)


if __name__ == "__main__":
    unittest.main()
