#!/usr/bin/env python3
"""DAG, ready predicate, prompt, shard, and dependency-request tests."""

from __future__ import annotations

import concurrent.futures
import copy
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "agent"))

from api_task_runner import (  # noqa: E402
    RunnerError,
    authorize_integration,
    authorize_run,
    digest,
    verify_conformant_artifacts,
)
from ownership_generator import (  # noqa: E402
    OwnershipError,
    transition_request,
    validate_changed_paths,
)
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
        self.assertEqual(
            generated["input_fingerprints"]["global_contract_conventions"],
            self.corpus["global_contract_fingerprints"]["conventions"],
        )
        self.assertEqual(
            generated["input_fingerprints"]["contract_ownership_registry"],
            self.corpus["global_contract_fingerprints"]["ownership_registry"],
        )
        self.assertTrue(
            all(
                not node["prerequisites"]["contract_candidate"]
                and not node["prerequisites"]["contract_frozen"]
                for node in generated["nodes"]
            )
        )
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

    def test_shared_components_providers_and_topology_fail_closed(self) -> None:
        engine_edges = [
            edge for edge in self.report["edges"]
            if edge["kind"] == "package_internal_engine"
        ]
        by_owner: dict[str, list[dict]] = {}
        for edge in engine_edges:
            by_owner.setdefault(edge["to"], []).append(edge)
        owner, shared_edges = next(
            (owner, edges) for owner, edges in by_owner.items() if len(edges) >= 2
        )
        self.assertGreaterEqual(len({edge["from"] for edge in shared_edges}), 2)
        wave_index = {
            unit_id: index
            for index, wave in enumerate(self.report["topological_waves"])
            for unit_id in wave
        }
        self.assertTrue(all(wave_index[owner] < wave_index[edge["from"]] for edge in shared_edges))
        self.assertTrue(all(
            edge["reason"] and edge["owner"]["steward"] and edge["source_contract"]
            for edge in self.report["edges"]
        ))
        self.assertTrue(any(
            blocker["code"] == "shared-component-not-complete"
            and ".shared_public_type" in blocker["subject"]
            for node in self.report["nodes"]
            for blocker in node["blockers"]
        ))

        inputs = list(self.inputs())
        owner_unit = next(
            provider["owner"]["atomic_unit_id"]
            for provider in self.report["providers"]["facts"]
            if provider["state"] == "available"
            and provider["owner"]["atomic_unit_id"] != "AU-WS-001"
        )
        unit = next(item for item in inputs[0]["atomic_units"] if item["id"] == owner_unit)
        unit["generation_state"] = "blocked"
        unit["readiness_state"] = "blocked"
        for packet in inputs[0]["packets"]:
            if packet["atomic_unit_id"] == owner_unit:
                packet["generation"] = {
                    "state": "blocked",
                    "block_reasons": ["provider_semantics_incomplete_fixture"],
                }
        incomplete = self.generated(tuple(inputs))
        self.assertTrue(any(
            provider["state"] == "available"
            and provider["semantics_state"] == "incomplete"
            for provider in incomplete["providers"]["facts"]
        ))
        self.assertTrue(any(
            blocker["code"] == "provider-semantics-incomplete"
            for node in incomplete["nodes"]
            for blocker in node["blockers"]
        ))

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
            bypassed = copy.deepcopy(resolution)
            bypassed["acceptance_command"]["argv"].remove("--execute")
            with self.assertRaises(jsonschema.ValidationError):
                jsonschema.Draft202012Validator(self.run_schema).validate(bypassed)

    def test_shards_include_declared_fixtures_and_non_bypassable_global_gates(self) -> None:
        required_gates = {"task-packets", "ownership", "configure", "build", "test", "quality"}
        for shard in self.report["shards"]:
            self.assertEqual(
                set(shard["fixture_categories"]),
                {"positive", "negative", "ambiguous"},
            )
            self.assertEqual(set(shard["mandatory_gate_ids"]), required_gates)
            self.assertTrue(all(isinstance(command["argv"], list) for command in shard["commands"]))
            self.assertIn("--execute", shard["acceptance_command"]["argv"])
            local_steps = [
                step for step in shard["execution_steps"]
                if step["scope"] == "unit_local"
            ]
            self.assertEqual(
                {step["fixture_category"] for step in local_steps},
                {"positive", "negative", "ambiguous"},
            )
            if shard["state"] != "blocked":
                self.assertTrue(all(step["evidence_paths"] for step in local_steps))
        node_states = {
            node["atomic_unit_id"]: node["state"] for node in self.report["nodes"]
        }
        for integration in self.report["package_integration_shards"]:
            self.assertEqual(set(integration["mandatory_gate_ids"]), required_gates)
            self.assertTrue(
                all(isinstance(command["argv"], list) for command in integration["commands"])
            )
            self.assertIn("--execute", integration["acceptance_command"]["argv"])
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
        bypassed = subprocess.run(
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
        self.assertNotEqual(bypassed.returncode, 0)
        self.assertIn("runner.execution-required", bypassed.stderr)
        other_path = next(
            tracked["path"]
            for tracked in self.ownership["tracked_paths"]
            if tracked["owner_role"].startswith("integration.package.")
            and tracked["owner_role"] != verification["package_integration_role"]
        )
        with self.assertRaises(OwnershipError):
            validate_changed_paths(
                self.ownership,
                verification["package_integration_role"],
                [other_path],
            )
        blocked = next(
            shard
            for shard in self.report["package_integration_shards"]
            if shard["state"] == "blocked"
        )
        self.assert_code(
            "runner.package-integration-blocked",
            lambda: authorize_integration(blocked),
        )

    def test_package_integration_requires_matching_executed_unit_artifacts(self) -> None:
        integration = next(
            shard
            for shard in self.report["package_integration_shards"]
            if shard["state"] == "verification"
            and shard["conformant_unit_artifacts"]
        )
        input_sha = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            root = pathlib.Path(temporary)
            for expected in integration["conformant_unit_artifacts"]:
                artifact = {
                    "schema": "cxxlens.api-ready.execution-result.v1",
                    "subject_kind": "atomic_unit",
                    "subject_id": expected["atomic_unit_id"],
                    "input_sha": input_sha,
                    "shard_id": expected["shard_id"],
                    "shard_digest": expected["shard_digest"],
                    "status": "passed",
                }
                artifact["artifact_digest"] = digest(artifact)
                path = root / expected["execution_result_path"]
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(artifact), encoding="utf-8")
            verified = verify_conformant_artifacts(root, integration, input_sha)
            self.assertEqual(
                {item["atomic_unit_id"] for item in verified},
                {
                    item["atomic_unit_id"]
                    for item in integration["conformant_unit_artifacts"]
                },
            )
            first = integration["conformant_unit_artifacts"][0]
            path = root / first["execution_result_path"]
            artifact = json.loads(path.read_text(encoding="utf-8"))
            artifact["status"] = "failed"
            artifact["artifact_digest"] = digest(
                {key: value for key, value in artifact.items() if key != "artifact_digest"}
            )
            path.write_text(json.dumps(artifact), encoding="utf-8")
            self.assert_code(
                "runner.unit-artifact-nonconformant",
                lambda: verify_conformant_artifacts(root, integration, input_sha),
            )

    def test_canonical_command_executes_and_attributes_local_and_global_failures(self) -> None:
        units = [
            node["atomic_unit_id"]
            for node in self.report["nodes"]
            if node["state"] == "complete"
        ][:2]

        def step(step_id: str, scope: str, exit_status: int, marker: str) -> dict:
            script = (
                "import pathlib,sys; "
                f"p=pathlib.Path({marker!r}); p.parent.mkdir(parents=True, exist_ok=True); "
                f"p.write_text({step_id!r}, encoding='utf-8'); "
                f"sys.exit({exit_status})"
            )
            category = step_id.removeprefix("local.") if scope == "unit_local" else None
            return {
                "id": step_id,
                "scope": scope,
                "fixture_category": category,
                "evidence_paths": [f"tests/{step_id}.fixture"] if category else [],
                "command": {
                    "id": step_id,
                    "argv": [sys.executable, "-c", script],
                    "environment": {},
                },
            }

        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            root = pathlib.Path(temporary)
            schemas = root / "schemas"
            schemas.mkdir()
            for relative in (
                "schemas/cxxlens.agent-task-packet-corpus.v1.json",
                "schemas/cxxlens.agent-ownership.v1.json",
                "schemas/cxxlens.api-ready.run-manifest.v1.schema.yaml",
                "schemas/cxxlens.api-ready.execution-result.v1.schema.yaml",
            ):
                shutil.copyfile(ROOT / relative, root / relative)
            report = copy.deepcopy(self.report)
            first_shard = next(
                shard for shard in report["shards"] if shard["atomic_unit_id"] == units[0]
            )
            second_shard = next(
                shard for shard in report["shards"] if shard["atomic_unit_id"] == units[1]
            )
            first_shard["execution_steps"] = [
                step("global.setup", "global_setup", 0, "markers/first-setup"),
                step("local.ambiguous", "unit_local", 0, "markers/first-ambiguous"),
                step("local.negative", "unit_local", 0, "markers/first-negative"),
                step("local.positive", "unit_local", 0, "markers/first-positive"),
                step("global.verify", "global_verification", 0, "markers/first-global"),
            ]
            second_shard["execution_steps"] = [
                step("global.setup", "global_setup", 0, "markers/second-setup"),
                step("local.ambiguous", "unit_local", 0, "markers/second-ambiguous"),
                step("local.negative", "unit_local", 7, "markers/second-negative"),
                step("local.positive", "unit_local", 0, "markers/second-positive"),
                step("global.verify", "global_verification", 0, "markers/second-global"),
            ]
            (schemas / "cxxlens.api-ready.report.v1.json").write_text(
                json.dumps(report), encoding="utf-8"
            )
            runner = ROOT / "tools/agent/api_task_runner.py"

            def command(shard: dict, execute: bool = True) -> list[str]:
                argv = shard["acceptance_command"]["argv"]
                selected = [sys.executable, str(runner), *argv[2:]]
                if not execute:
                    selected.remove("--execute")
                return [*selected, "--root", str(root)]

            bypassed = subprocess.run(
                command(first_shard, execute=False),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(bypassed.returncode, 0)
            self.assertIn("runner.execution-required", bypassed.stderr)

            first = subprocess.run(
                command(first_shard),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertTrue((root / "markers/first-positive").is_file())
            first_artifact = json.loads(
                (root / first_shard["execution_result_path"]).read_text(encoding="utf-8")
            )
            self.assertEqual(first_artifact["status"], "passed")
            self.assertEqual(
                {item["scope"] for item in first_artifact["steps"]},
                {"global_setup", "unit_local", "global_verification"},
            )

            second = subprocess.run(
                command(second_shard),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(second.returncode, 0)
            second_artifact = json.loads(
                (root / second_shard["execution_result_path"]).read_text(encoding="utf-8")
            )
            self.assertEqual(second_artifact["subject_id"], units[1])
            self.assertEqual(second_artifact["steps"][-1]["id"], "local.negative")
            self.assertEqual(second_artifact["steps"][-1]["exit_status"], 7)
            self.assertEqual(first_artifact["status"], "passed")

            second_shard["execution_steps"] = [
                step("global.setup", "global_setup", 0, "markers/global-setup"),
                step("local.ambiguous", "unit_local", 0, "markers/global-ambiguous"),
                step("local.negative", "unit_local", 0, "markers/global-negative"),
                step("local.positive", "unit_local", 0, "markers/global-positive"),
                step("global.verify", "global_verification", 9, "markers/global-failure"),
            ]
            (schemas / "cxxlens.api-ready.report.v1.json").write_text(
                json.dumps(report), encoding="utf-8"
            )
            global_failure = subprocess.run(
                command(second_shard),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(global_failure.returncode, 0)
            global_artifact = json.loads(
                (root / second_shard["execution_result_path"]).read_text(encoding="utf-8")
            )
            self.assertEqual(global_artifact["steps"][-1]["scope"], "global_verification")
            self.assertEqual(global_artifact["steps"][-1]["exit_status"], 9)

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
            "acceptance_command_bypass": self.negative_acceptance_command,
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
        edge = copy.deepcopy(next(edge for edge in report["edges"] if edge["scope"] == "readiness"))
        edge["to"] = "AU-NOT-999"
        edge["owner"]["atomic_unit_id"] = "AU-NOT-999"
        report["edges"].append(edge)
        validate_report(report, self.report, self.schema)

    def negative_cycle(self) -> None:
        report = copy.deepcopy(self.report)
        first, second = report["topological_waves"][0][:2]
        api_id = report["nodes"][0]["api_ids"][0]
        template = copy.deepcopy(next(edge for edge in report["edges"] if edge["scope"] == "readiness"))
        template.update({
            "scope": "readiness",
            "kind": "api_dependency",
            "component_id": None,
            "reason": "negative cycle fixture",
            "source_contract": "tests/agent/runner/fixtures/negative_cases.yaml",
            "required_semantics_version": None,
            "via_api_ids": [api_id],
        })
        forward = copy.deepcopy(template)
        forward.update({"from": first, "to": second})
        forward["owner"] = {"atomic_unit_id": second, "steward": "fixture"}
        reverse = copy.deepcopy(template)
        reverse.update({"from": second, "to": first})
        reverse["owner"] = {"atomic_unit_id": first, "steward": "fixture"}
        report["edges"].extend(
            [forward, reverse]
        )
        validate_report(report, self.report, self.schema)

    def negative_provider(self) -> None:
        report = copy.deepcopy(self.report)
        report["providers"]["facts"].append(copy.deepcopy(report["providers"]["facts"][0]))
        validate_report(report, self.report, self.schema)

    def negative_acceptance_command(self) -> None:
        report = copy.deepcopy(self.report)
        report["shards"][0]["acceptance_command"]["argv"].remove("--execute")
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
