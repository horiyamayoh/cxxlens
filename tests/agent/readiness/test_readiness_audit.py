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
from unittest import mock

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "agent"))

from ownership_generator import OwnershipError  # noqa: E402
from ready_evaluator import ReadyError, digest  # noqa: E402
from readiness_audit import (  # noqa: E402
    AuditError,
    collect_online_evidence,
    declaration_digest,
    generate_authorization,
    gate_declarations,
    input_fingerprints,
    load_inputs,
    seal_evidence_bundle,
    seal_execution,
    validate_authorization,
    validate_gate_evidence,
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

    def source_identity(self) -> dict:
        return {
            "binding": "exact-git-object",
            "repository": "horiyamayoh/cxxlens",
            "commit_sha": "1" * 40,
            "tree_sha": "2" * 40,
            "dirty": False,
        }

    def valid_evidence(self, provider: str = "local-replay") -> dict:
        source = self.source_identity()
        workflow = input_fingerprints(self.inputs, ROOT)["quality_workflow"]
        rows = []
        for gate in gate_declarations(self.inputs):
            identities = (
                gate["required_ci_jobs"]
                if provider == "github-actions"
                else [
                    f"{gate['id']}.replay.{index}"
                    for index, _ in enumerate(gate["replay_commands"], start=1)
                ]
            )
            executions = []
            for index, identity in enumerate(identities):
                command_fingerprint = (
                    digest(gate["replay_commands"][index])
                    if provider == "local-replay"
                    else digest({"job": identity, "workflow": workflow})
                )
                executions.append(
                    seal_execution(
                        {
                            "identity": identity,
                            "provider_run_id": (
                                "12345" if provider == "github-actions" else None
                            ),
                            "provider_check_id": (
                                str(9000 + index)
                                if provider == "github-actions"
                                else None
                            ),
                            "source_sha": source["commit_sha"],
                            "tree_sha": source["tree_sha"],
                            "conclusion": "success",
                            "exit_status": 0 if provider == "local-replay" else None,
                            "started_at": "2026-07-13T00:00:00Z",
                            "completed_at": "2026-07-13T00:00:01Z",
                            "command_fingerprint": command_fingerprint,
                            "environment_fingerprint": digest(
                                {"identity": identity, "kind": "environment"}
                            ),
                            "toolchain_fingerprint": digest(
                                {"identity": identity, "kind": "toolchain"}
                            ),
                            "artifact_log_digest": digest(
                                {"identity": identity, "kind": "log"}
                            ),
                            "provider_record_digest": digest(
                                {"identity": identity, "kind": "provider"}
                            ),
                        }
                    )
                )
            rows.append(
                {
                    "gate_id": gate["id"],
                    "gate_kind": (
                        "global" if gate["id"] == "GLOBAL" else "foundation"
                    ),
                    "provider": provider,
                    "required_status": "success",
                    "observed_status": "success",
                    "declaration_digest": declaration_digest(gate),
                    "executions": executions,
                }
            )
        return seal_evidence_bundle(
            {
                "schema": "cxxlens.readiness.gate-evidence.v1",
                "provider": provider,
                "repository": source["repository"],
                "workflow_identity": ".github/workflows/quality.yml",
                "source": source,
                "workflow_fingerprint": workflow,
                "issued_at": "2026-07-13T00:00:02Z",
                "gates": rows,
            }
        )

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
        self.assertEqual(generated["gate_evidence"]["status"], "missing")
        self.assertEqual(generated["source_identity"]["binding"], "unbound")
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
        self.assertEqual(catalog["candidate_contract_package_count"], 10)
        self.assertEqual(catalog["frozen_contract_package_count"], 0)
        self.assertEqual(catalog["invalid_contract_transition_count"], 0)
        self.assertEqual(
            {api["contract_state"] for api in self.authorization["apis"]},
            {"candidate", "unresolved"},
        )
        self.assertEqual(
            self.authorization["input_fingerprints"]["global_contract_conventions"],
            self.inputs["corpus"]["global_contract_fingerprints"]["conventions"],
        )
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
            self.assertTrue(gate["required_ci_jobs"])
            self.assertEqual(gate["observed_execution"]["status"], "missing")
            self.assertTrue(all(isinstance(command["argv"], list) for command in gate["replay_commands"]))
        self.assertEqual(
            self.authorization["global_gate"]["required_ci_jobs"],
            [
                "build-test-clang22",
                "contracts-docs",
                "format-lint",
                "gcc-header-compat",
                "install-consumer",
                "public-api",
                "static-analysis",
            ],
        )
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

    def test_exact_source_observed_gate_evidence_is_schema_valid(self) -> None:
        for provider in ("local-replay", "github-actions"):
            with self.subTest(provider=provider):
                bundle = self.valid_evidence(provider)
                self.assertEqual(
                    validate_gate_evidence(bundle, self.inputs, self.source_identity()),
                    "success",
                )
                generated = generate_authorization(
                    self.fresh_inputs(), ROOT, bundle, self.source_identity()
                )
                self.assertEqual(generated["gate_evidence"]["status"], "success")
                self.assertEqual(
                    generated["source_identity"]["commit_sha"], "1" * 40
                )
                validate_authorization(generated, generated, self.schema)

    def test_failed_cancelled_and_skipped_gate_results_deny_dispatch(self) -> None:
        for conclusion in ("failure", "cancelled", "skipped"):
            with self.subTest(conclusion=conclusion):
                bundle = self.valid_evidence()
                execution = bundle["gates"][0]["executions"][0]
                execution["conclusion"] = conclusion
                execution["exit_status"] = 7
                bundle["gates"][0]["executions"][0] = seal_execution(execution)
                bundle["gates"][0]["observed_status"] = "failed"
                bundle = seal_evidence_bundle(bundle)
                self.assertEqual(
                    validate_gate_evidence(bundle, self.inputs, self.source_identity()),
                    "failed",
                )
                generated = generate_authorization(
                    self.fresh_inputs(), ROOT, bundle, self.source_identity()
                )
                self.assertEqual(generated["gate_evidence"]["status"], "failed")
                self.assertEqual(generated["authorization"]["decision"], "denied")
                self.assertFalse(any(unit["dispatch_authorized"] for unit in generated["units"]))

    def test_dispatch_true_requires_exact_success_evidence(self) -> None:
        bundle = self.valid_evidence()
        report = generate_authorization(
            self.fresh_inputs(), ROOT, bundle, self.source_identity()
        )
        unit = next(unit for unit in report["units"] if unit["state"] == "complete")
        unit["state"] = "ready"
        unit["dispatch_authorized"] = True
        for api in report["apis"]:
            if api["atomic_unit_id"] == unit["atomic_unit_id"]:
                api["state"] = "ready"
                api["dispatch_authorized"] = True
        report["authorization"].update(
            {
                "decision": "authorized",
                "reason_code": "ready-wave-available",
                "candidate_ready_unit_ids": [unit["atomic_unit_id"]],
                "ready_unit_ids": [unit["atomic_unit_id"]],
                "ready_waves": [[unit["atomic_unit_id"]]],
            }
        )
        self.resign(report)
        validate_authorization(report, report, self.schema)

        report["gate_evidence"]["status"] = "missing"
        self.resign(report)
        self.assert_code(
            "readiness.false-authorization",
            lambda: validate_authorization(report, report, self.schema),
        )

    def test_online_collector_requires_one_complete_exact_sha_workflow_run(self) -> None:
        source = self.source_identity()
        required_jobs = sorted(
            {
                job
                for gate in gate_declarations(self.inputs)
                for job in gate["required_ci_jobs"]
            }
        )
        checks = [
            {
                "id": 1000 + index,
                "name": name,
                "head_sha": source["commit_sha"],
                "app": {"slug": "github-actions"},
                "details_url": (
                    f"https://github.com/horiyamayoh/cxxlens/actions/runs/99/job/{2000 + index}"
                ),
                "conclusion": "success",
                "started_at": "2026-07-13T00:00:00Z",
                "completed_at": "2026-07-13T00:00:01Z",
            }
            for index, name in enumerate(required_jobs)
        ]
        with (
            mock.patch(
                "readiness_audit.current_source_identity", return_value=source
            ),
            mock.patch(
                "readiness_audit.github_api_json",
                return_value={"check_runs": checks},
            ),
            mock.patch(
                "readiness_audit.github_job_log", return_value=b"verified log"
            ) as log_mock,
        ):
            bundle = collect_online_evidence(
                self.inputs, ROOT, source["repository"], "99"
            )
        self.assertEqual(len(log_mock.call_args_list), len(required_jobs))
        self.assertEqual(
            validate_gate_evidence(bundle, self.inputs, source), "success"
        )

        with (
            mock.patch(
                "readiness_audit.current_source_identity", return_value=source
            ),
            mock.patch(
                "readiness_audit.github_api_json",
                return_value={"check_runs": checks[:-1]},
            ),
        ):
            self.assert_code(
                "readiness.required-job-omitted",
                lambda: collect_online_evidence(
                    self.inputs, ROOT, source["repository"], "99"
                ),
            )

    def test_execution_evidence_corruption_fixtures_fail_closed(self) -> None:
        mutators = {
            "missing_run": self.missing_run,
            "green_different_sha": self.green_different_sha,
            "stale_artifact_digest": self.stale_artifact_digest,
            "command_declaration_only": self.command_declaration_only,
            "required_job_omitted": self.required_job_omitted,
            "observed_result_tampered": self.observed_result_tampered,
            "observed_workflow_drift": self.observed_workflow_drift,
        }
        for case in self.negative_cases:
            if case["id"] not in mutators:
                continue
            with self.subTest(case=case["id"]):
                self.assert_code(case["expected_code"], mutators[case["id"]])

    def catalog_duplicate(self) -> None:
        inputs = self.fresh_inputs()
        duplicate = copy.deepcopy(inputs["catalog"]["packages"][0]["apis"][0])
        inputs["catalog"]["packages"][0]["apis"].append(duplicate)
        generate_authorization(inputs, ROOT)

    def missing_run(self) -> None:
        bundle = self.valid_evidence()
        bundle["gates"].pop()
        validate_gate_evidence(
            seal_evidence_bundle(bundle), self.inputs, self.source_identity()
        )

    def green_different_sha(self) -> None:
        bundle = self.valid_evidence()
        bundle["source"]["commit_sha"] = "3" * 40
        for gate in bundle["gates"]:
            for index, execution in enumerate(gate["executions"]):
                execution["source_sha"] = "3" * 40
                gate["executions"][index] = seal_execution(execution)
        validate_gate_evidence(
            seal_evidence_bundle(bundle), self.inputs, self.source_identity()
        )

    def stale_artifact_digest(self) -> None:
        bundle = self.valid_evidence()
        bundle["gates"][0]["executions"][0]["artifact_record_digest"] = (
            "sha256:" + "0" * 64
        )
        validate_gate_evidence(
            seal_evidence_bundle(bundle), self.inputs, self.source_identity()
        )

    def command_declaration_only(self) -> None:
        generated = generate_authorization(self.fresh_inputs(), ROOT)
        if generated["gate_evidence"]["status"] != "success":
            raise AuditError(
                "readiness.gate-evidence-missing",
                "command declarations are not observed execution evidence",
            )

    def required_job_omitted(self) -> None:
        bundle = self.valid_evidence("github-actions")
        bundle["gates"][0]["executions"].pop()
        validate_gate_evidence(
            seal_evidence_bundle(bundle), self.inputs, self.source_identity()
        )

    def observed_result_tampered(self) -> None:
        bundle = self.valid_evidence()
        bundle["gates"][0]["executions"][0]["conclusion"] = "failure"
        validate_gate_evidence(
            seal_evidence_bundle(bundle), self.inputs, self.source_identity()
        )

    def observed_workflow_drift(self) -> None:
        bundle = self.valid_evidence()
        bundle["workflow_fingerprint"] = "sha256:" + "0" * 64
        validate_gate_evidence(
            seal_evidence_bundle(bundle), self.inputs, self.source_identity()
        )

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
