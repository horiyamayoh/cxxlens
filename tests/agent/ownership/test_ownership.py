#!/usr/bin/env python3
"""Ownership, skeleton, diff-audit, and dependency-request contract tests."""

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

from ownership_generator import (  # noqa: E402
    OwnershipError,
    digest,
    generate_manifest,
    repository_paths,
    reserved_policy,
    transition_request,
    validate_changed_paths,
    validate_dependency_request,
    validate_manifest,
)


def process_manifest_digest(corpus: dict, paths: list[str]) -> str:
    return generate_manifest(corpus, paths)["semantic_digest"]


class OwnershipTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.corpus = json.loads(
            (ROOT / "schemas" / "cxxlens.agent-task-packet-corpus.v1.json").read_text(
                encoding="utf-8"
            )
        )
        cls.manifest = json.loads(
            (ROOT / "schemas" / "cxxlens.agent-ownership.v1.json").read_text(
                encoding="utf-8"
            )
        )
        cls.schema = yaml.safe_load(
            (ROOT / "schemas" / "cxxlens.agent-ownership.v1.schema.yaml").read_text(
                encoding="utf-8"
            )
        )
        cls.request_schema = yaml.safe_load(
            (ROOT / "schemas" / "cxxlens.dependency-request.v1.schema.yaml").read_text(
                encoding="utf-8"
            )
        )
        cls.examples = json.loads(
            (ROOT / "schemas" / "cxxlens.dependency-request.examples.v1.json").read_text(
                encoding="utf-8"
            )
        )
        cls.paths = repository_paths(ROOT)
        cls.negative_cases = yaml.safe_load(
            (ROOT / "tests" / "agent" / "ownership" / "fixtures" / "negative_cases.yaml")
            .read_text(encoding="utf-8")
        )["cases"]

    def assert_code(self, code: str, action) -> None:
        with self.assertRaises(OwnershipError) as raised:
            action()
        self.assertEqual(raised.exception.code, code)

    @staticmethod
    def resign_request(request: dict) -> None:
        unsigned = copy.deepcopy(request)
        unsigned.pop("semantic_digest", None)
        request["semantic_digest"] = digest(unsigned)

    def test_full_manifest_schema_coverage_and_frozen_skeletons(self) -> None:
        generated = generate_manifest(self.corpus, self.paths)
        baseline_paths = [path for path in self.paths if reserved_policy(path) is None]
        self.assertEqual(generated, self.manifest)
        self.assertNotIn(str(ROOT), json.dumps(generated, sort_keys=True))
        validate_manifest(generated, self.corpus, self.paths, self.schema)
        jsonschema.Draft202012Validator(self.schema).validate(generated)
        self.assertEqual(generated["summary"]["unit_count"], 124)
        self.assertEqual(
            generated["summary"]["tracked_path_count"], len(baseline_paths)
        )
        self.assertEqual(generated["summary"]["reserved_path_count"], 11)
        self.assertEqual(
            generated["summary"]["skeleton_state_counts"],
            {"blocked": 0, "frozen": 124},
        )
        self.assertEqual(
            generated["summary"]["contract_state_counts"],
            {"frozen": 124},
        )
        self.assertEqual(
            generated["global_contract_fingerprints"],
            self.corpus["global_contract_fingerprints"],
        )
        tracked = [item["path"] for item in generated["tracked_paths"]]
        self.assertEqual(tracked, sorted(baseline_paths))
        self.assertEqual(len(tracked), len(set(tracked)))
        ng_authority_paths = {
            item["path"]
            for item in generated["tracked_paths"]
            if item["owner_role"] == "steward.ng-authority"
        }
        self.assertEqual(
            ng_authority_paths,
            {
                "docs/design/adr/0002-semantic-relation-platform.md",
                "docs/design/adr/0003-versioned-relation-kernel.md",
                "docs/design/adr/0004-legacy-contract-reset.md",
                "docs/design/cxxlens_next_generation_integrated_design_ja.md",
                "schemas/cxxlens_legacy_api_baseline.schema.yaml",
                "schemas/cxxlens_legacy_api_baseline.yaml",
                "schemas/cxxlens_ng_authority_transition.schema.yaml",
                "schemas/cxxlens_ng_authority_transition.yaml",
                "schemas/cxxlens_ng_authority_transition_report.schema.yaml",
                "tests/quality/test_ng_authority.py",
                "tools/quality/check_ng_authority.py",
            },
        )
        self.assertEqual(
            {item["api_id"] for item in generated["skeletons"]},
            {packet["api_id"] for packet in self.corpus["packets"]},
        )
        for skeleton in generated["skeletons"]:
            packet = next(
                value for value in self.corpus["packets"] if value["api_id"] == skeleton["api_id"]
            )
            self.assertEqual(skeleton["contract_state"], packet["contract"]["state"])
            self.assertEqual(
                skeleton["contract_owner_issue"], packet["contract"]["owner_issue"]
            )
            if skeleton["state"] == "blocked":
                self.assertIsNone(skeleton["signature"])
                self.assertTrue(skeleton["block_reasons"])
            else:
                self.assertIsNotNone(skeleton["signature"])
                self.assertFalse(skeleton["block_reasons"])

    def test_unit_prefixes_are_disjoint_and_shared_inputs_are_read_only(self) -> None:
        prefixes = []
        tracked = {item["path"] for item in self.manifest["tracked_paths"]}
        for unit in self.manifest["units"]:
            self.assertEqual(len(unit["allowed_write_prefixes"]), 3)
            self.assertEqual(
                unit["unit_owner_role"], f"unit.{unit['atomic_unit_id'].lower()}"
            )
            for prefix in unit["allowed_write_prefixes"]:
                self.assertFalse(
                    any(
                        prefix.startswith(other) or other.startswith(prefix)
                        for other in prefixes
                    )
                )
                prefixes.append(prefix)
            self.assertIn(
                "schemas/cxxlens.agent-task-packet-corpus.v1.json",
                unit["read_only_shared_inputs"],
            )
            self.assertTrue(
                any(path in tracked for path in unit["read_only_shared_inputs"])
            )

    def test_unit_and_package_integration_preflight(self) -> None:
        unit = self.manifest["units"][0]
        allowed = unit["allowed_write_prefixes"][0] + "implementation.cpp"
        audit = validate_changed_paths(self.manifest, unit["atomic_unit_id"], [allowed])
        self.assertEqual(audit["status"], "passed")
        self.assertEqual(audit["changed_paths"], [allowed])

        package_role = unit["package_integration_role"]
        owned_header = next(
            item["path"]
            for item in self.manifest["tracked_paths"]
            if item["owner_role"] == package_role
        )
        self.assertEqual(
            validate_changed_paths(self.manifest, package_role, [owned_header])["status"],
            "passed",
        )
        other_header = next(
            item["path"]
            for item in self.manifest["tracked_paths"]
            if item["owner_role"].startswith("integration.package.")
            and item["owner_role"] != package_role
        )
        self.assert_code(
            "ownership.unauthorized-path",
            lambda: validate_changed_paths(self.manifest, package_role, [other_header]),
        )

    def test_generated_and_shared_edits_are_rejected_before_build(self) -> None:
        unit_id = self.manifest["units"][0]["atomic_unit_id"]
        generated = next(item for item in self.manifest["tracked_paths"] if item["generated"])
        self.assert_code(
            "ownership.generated-direct-edit",
            lambda: validate_changed_paths(self.manifest, unit_id, [generated["path"]]),
        )
        shared = next(item for item in self.manifest["tracked_paths"] if not item["generated"])
        with self.assertRaises(OwnershipError) as raised:
            validate_changed_paths(self.manifest, unit_id, [shared["path"]])
        self.assertEqual(raised.exception.code, "ownership.unauthorized-path")
        message = str(raised.exception)
        self.assertIn(f"path={shared['path']}", message)
        self.assertIn(f"owner={shared['owner_role']}", message)
        self.assertIn(f"requester={unit_id}", message)
        self.assertIn("alternative=dependency_request", message)

    def test_dependency_request_schema_transitions_and_reissue(self) -> None:
        pending = copy.deepcopy(self.examples["requests"][0])
        validate_dependency_request(pending, self.manifest, self.request_schema)
        accepted = transition_request(pending, "accepted", self.manifest)
        validate_dependency_request(accepted, self.manifest, self.request_schema)
        resolved_a = transition_request(
            accepted,
            "resolved",
            self.manifest,
            ["exact declaration and negative fixture published"],
        )
        resolved_b = transition_request(
            accepted,
            "resolved",
            self.manifest,
            ["exact declaration and negative fixture published"],
        )
        self.assertEqual(resolved_a, resolved_b)
        validate_dependency_request(resolved_a, self.manifest, self.request_schema)
        self.assertEqual(
            resolved_a["resolution"]["affected_packet_ids"], pending["blocked_api_ids"]
        )
        self.assertRegex(resolved_a["resolution"]["reissue_fingerprint"], r"^sha256:")

    def test_order_process_and_cli_determinism(self) -> None:
        baseline = self.manifest["semantic_digest"]
        self.assertEqual(generate_manifest(self.corpus, list(reversed(self.paths))), self.manifest)
        future_paths = self.paths + [
            "tools/agent/ready_evaluator.py",
            "schemas/cxxlens.api-ready.report.v1.json",
            "tests/agent/readiness/test_readiness.py",
        ]
        self.assertEqual(generate_manifest(self.corpus, future_paths), self.manifest)
        self.assertEqual(
            validate_changed_paths(
                self.manifest,
                "steward.runner",
                ["tools/agent/ready_evaluator.py"],
            )["status"],
            "passed",
        )
        with concurrent.futures.ProcessPoolExecutor(max_workers=2) as executor:
            futures = [
                executor.submit(process_manifest_digest, self.corpus, self.paths) for _ in range(2)
            ]
            self.assertEqual({future.result() for future in futures}, {baseline})
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "agent" / "ownership_generator.py"),
                "check",
                "--root",
                str(ROOT),
            ],
            check=True,
            text=True,
            capture_output=True,
        )

    def test_negative_fixture_codes(self) -> None:
        mutators = {
            "overlapping_unit_prefix": self.negative_overlap,
            "missing_tracked_owner": self.negative_missing_owner,
            "unauthorized_shared_edit": self.negative_unauthorized,
            "generated_direct_edit": self.negative_generated,
            "skeleton_signature_drift": self.negative_skeleton,
            "invalid_request_transition": self.negative_transition,
            "request_api_unit_mismatch": self.negative_request_api,
        }
        self.assertEqual({case["id"] for case in self.negative_cases}, set(mutators))
        for case in self.negative_cases:
            with self.subTest(case=case["id"]):
                self.assert_code(case["expected_code"], mutators[case["id"]])

    def negative_overlap(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["units"][1]["allowed_write_prefixes"][0] = manifest["units"][0][
            "allowed_write_prefixes"
        ][0]
        validate_manifest(manifest, self.corpus, self.paths, self.schema)

    def negative_missing_owner(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["tracked_paths"].pop()
        validate_manifest(manifest, self.corpus, self.paths, self.schema)

    def negative_unauthorized(self) -> None:
        unit = self.manifest["units"][0]
        validate_changed_paths(self.manifest, unit["atomic_unit_id"], ["CMakeLists.txt"])

    def negative_generated(self) -> None:
        unit = self.manifest["units"][0]
        validate_changed_paths(
            self.manifest,
            unit["atomic_unit_id"],
            ["schemas/cxxlens.agent-ownership.v1.json"],
        )

    def negative_skeleton(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        skeleton = next(item for item in manifest["skeletons"] if item["state"] == "frozen")
        skeleton["signature_fingerprint"] = "sha256:" + "0" * 64
        validate_manifest(manifest, self.corpus, self.paths, self.schema)

    def negative_transition(self) -> None:
        transition_request(self.examples["requests"][0], "resolved", self.manifest)

    def negative_request_api(self) -> None:
        request = copy.deepcopy(self.examples["requests"][0])
        unit = next(
            unit
            for unit in self.manifest["units"]
            if unit["atomic_unit_id"] != request["requesting_atomic_unit"]
        )
        request["blocked_api_ids"] = [unit["member_api_ids"][0]]
        self.resign_request(request)
        validate_dependency_request(request, self.manifest, self.request_schema)


if __name__ == "__main__":
    unittest.main()
