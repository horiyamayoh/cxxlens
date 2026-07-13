#!/usr/bin/env python3
"""Contract, negative, drift, and determinism tests for agent task packets."""

from __future__ import annotations

import concurrent.futures
import copy
import hashlib
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

from task_packet_generator import (  # noqa: E402
    CORPUS_SCHEMA,
    PACKET_SCHEMA,
    TaskPacketError,
    digest,
    generate_corpus,
    make_report,
    validate_corpus,
)


def process_digest(document: dict, root: str) -> str:
    return generate_corpus(document, pathlib.Path(root))["semantic_digest"]


class TaskPacketTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.document = yaml.safe_load(
            (ROOT / "schemas" / "cxxlens_public_api_contract.yaml").read_text(
                encoding="utf-8"
            )
        )
        cls.schema = yaml.safe_load(
            (ROOT / "schemas" / "cxxlens.agent-task-packet.v1.schema.yaml").read_text(
                encoding="utf-8"
            )
        )
        cls.report_schema = yaml.safe_load(
            (
                ROOT
                / "schemas"
                / "cxxlens.agent-task-packet-validation-report.v1.schema.yaml"
            ).read_text(encoding="utf-8")
        )
        cls.checked_in = json.loads(
            (ROOT / "schemas" / "cxxlens.agent-task-packet-corpus.v1.json").read_text(
                encoding="utf-8"
            )
        )
        cls.negative_cases = yaml.safe_load(
            (ROOT / "tests" / "agent" / "task_packet" / "fixtures" / "negative_cases.yaml")
            .read_text(encoding="utf-8")
        )["cases"]

    def generated(self) -> dict:
        return generate_corpus(copy.deepcopy(self.document), ROOT)

    @staticmethod
    def packet(corpus: dict, api_id: str) -> dict:
        return next(packet for packet in corpus["packets"] if packet["api_id"] == api_id)

    @staticmethod
    def resign(corpus: dict, api_ids: tuple[str, ...] = ()) -> None:
        for api_id in api_ids:
            packet = TaskPacketTest.packet(corpus, api_id)
            packet_without_digest = copy.deepcopy(packet)
            packet_without_digest.pop("semantic_digest")
            packet["semantic_digest"] = digest(packet_without_digest)
        corpus_without_digest = copy.deepcopy(corpus)
        corpus_without_digest.pop("semantic_digest")
        corpus["semantic_digest"] = digest(corpus_without_digest)

    def assert_code(self, corpus: dict, code: str, root: pathlib.Path = ROOT) -> None:
        with self.assertRaises(TaskPacketError) as raised:
            validate_corpus(corpus, self.document, root, self.schema)
        self.assertEqual(raised.exception.code, code)

    def copy_declaration_sources(self, destination: pathlib.Path) -> None:
        sources = {
            api["declaration"]["source"]
            for package in self.document["packages"]
            for api in package["apis"]
        }
        for relative in sources:
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, target)

    def test_full_corpus_schema_membership_and_completeness(self) -> None:
        generated = self.generated()
        self.assertEqual(generated, self.checked_in)
        self.assertEqual(generated["schema"], CORPUS_SCHEMA)
        self.assertEqual(generated["summary"]["package_count"], 22)
        self.assertEqual(generated["summary"]["api_count"], 124)
        self.assertEqual(generated["summary"]["atomic_unit_count"], 124)
        self.assertEqual(
            generated["summary"]["generation_state_counts"],
            {"blocked": 77, "complete": 47, "ready": 0},
        )
        self.assertEqual(
            generated["summary"]["declaration_state_counts"],
            {"exact": 47, "unresolved": 77},
        )
        self.assertEqual(
            set(generated["global_contract_fingerprints"]),
            {"conventions", "ownership_registry"},
        )
        api_ids = [packet["api_id"] for packet in generated["packets"]]
        member_ids = [
            api_id for unit in generated["atomic_units"] for api_id in unit["member_api_ids"]
        ]
        self.assertEqual(api_ids, sorted(api_ids))
        self.assertEqual(sorted(member_ids), api_ids)
        self.assertEqual(len(member_ids), len(set(member_ids)))
        jsonschema.Draft202012Validator(self.schema).validate(generated)
        report = validate_corpus(generated, self.document, ROOT, self.schema)
        jsonschema.Draft202012Validator(self.report_schema).validate(report)
        self.assertEqual(report, make_report(generated))

    def test_packet_fields_are_typed_and_do_not_guess_readiness(self) -> None:
        for packet in self.generated()["packets"]:
            self.assertEqual(packet["schema"], PACKET_SCHEMA)
            self.assertRegex(packet["declaration"]["source_fingerprint"], r"^sha256:")
            self.assertRegex(packet["declaration"]["contract_fingerprint"], r"^sha256:")
            self.assertEqual(
                packet["contract"]["conventions_fingerprint"],
                self.checked_in["global_contract_fingerprints"]["conventions"],
            )
            self.assertEqual(
                packet["contract"]["ownership_registry_fingerprint"],
                self.checked_in["global_contract_fingerprints"]["ownership_registry"],
            )
            package = next(
                value for value in self.document["packages"] if value["id"] == packet["package"]["id"]
            )
            self.assertEqual(
                {key: packet["contract"][key] for key in package["contract"]},
                package["contract"],
            )
            self.assertEqual(packet["coordination"]["schema_owner_refs"], ["steward.schema"])
            self.assertEqual(
                {fixture["category"] for fixture in packet["fixtures"]},
                {"positive", "negative", "ambiguous"},
            )
            for fixture in packet["fixtures"]:
                if packet["generation"]["state"] == "complete":
                    self.assertEqual(len(fixture["case_ids"]), 1)
                    self.assertEqual(len(fixture["test_ids"]), 1)
                    self.assertEqual(len(fixture["expected_outcomes"]), 1)
                    self.assertEqual(len(fixture["evidence_candidates"]), 1)
                else:
                    self.assertEqual(fixture["case_ids"], [])
            self.assertTrue(
                all(isinstance(command["argv"], list) for command in packet["acceptance_commands"])
            )
            self.assertEqual(
                set(packet["quality_obligations"]),
                {"evidence", "coverage", "unresolved", "invariant_schema"},
            )
            if packet["declaration"]["status"] == "unresolved":
                self.assertEqual(packet["generation"]["state"], "blocked")
                self.assertIn(
                    "exact_declaration_unresolved", packet["generation"]["block_reasons"]
                )

    def test_contract_state_drift_is_rejected(self) -> None:
        corpus = self.generated()
        corpus["packets"][0]["contract"]["state"] = "candidate"
        self.resign(corpus, (corpus["packets"][0]["api_id"],))
        self.assert_code(corpus, "task_packet.contract-state-drift")

    def test_complete_packet_without_category_evidence_is_rejected(self) -> None:
        corpus = self.generated()
        packet = next(
            value for value in corpus["packets"] if value["generation"]["state"] == "complete"
        )
        packet["fixtures"][0]["case_ids"] = []
        self.resign(corpus, (packet["api_id"],))
        self.assert_code(corpus, "task_packet.fixture-evidence-missing")

    def test_typed_expression_and_atomic_dependency_expansion(self) -> None:
        api_to_unit = {
            packet["api_id"]: packet["atomic_unit_id"]
            for packet in self.generated()["packets"]
        }
        expression_registry = {
            item["id"]: item["expands_to"]
            for item in self.document["registries"]["dependency_expressions"]
        }
        for packet in self.generated()["packets"]:
            dependencies = packet["dependencies"]
            component_units = {
                component["owner_atomic_unit"]
                for component in dependencies["components"]
            }
            expected_units = {
                api_to_unit[api_id] for api_id in dependencies["apis"]
            } | component_units
            expected_units.discard(packet["atomic_unit_id"])
            self.assertEqual(
                dependencies["atomic_units"],
                sorted(expected_units),
            )
            self.assertEqual(len(dependencies["components"]), len({
                component["id"] for component in dependencies["components"]
            }))
            self.assertEqual(
                packet["coordination"]["shared_contract_steward_refs"],
                [next(
                    component["steward"]
                    for component in dependencies["components"]
                    if component["kind"] == "package_internal_engine"
                )],
            )
            for expression in dependencies["expressions"]:
                self.assertEqual(
                    expression["expands_to"], expression_registry[expression["id"]]
                )

    def test_catalog_order_root_and_process_determinism(self) -> None:
        baseline = self.generated()
        reordered = copy.deepcopy(self.document)
        reordered["packages"].reverse()
        for package in reordered["packages"]:
            package["apis"].reverse()
        self.assertEqual(generate_corpus(reordered, ROOT), baseline)
        with tempfile.TemporaryDirectory() as temporary:
            relocated = pathlib.Path(temporary)
            self.copy_declaration_sources(relocated)
            self.assertEqual(generate_corpus(self.document, relocated), baseline)
            with concurrent.futures.ProcessPoolExecutor(max_workers=2) as executor:
                futures = [
                    executor.submit(process_digest, self.document, str(relocated))
                    for _ in range(2)
                ]
                self.assertEqual(
                    {future.result() for future in futures},
                    {baseline["semantic_digest"]},
                )

    def test_source_drift_changes_only_bound_packet_and_rejects_stale_corpus(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            relocated = pathlib.Path(temporary)
            self.copy_declaration_sources(relocated)
            before = generate_corpus(self.document, relocated)
            api = next(
                api
                for package in self.document["packages"]
                for api in package["apis"]
                if api["declaration"]["status"] == "exact"
            )
            source = relocated / api["declaration"]["source"]
            source.write_text(source.read_text(encoding="utf-8") + "\n", encoding="utf-8")
            after = generate_corpus(self.document, relocated)
            changed = [
                left["api_id"]
                for left, right in zip(before["packets"], after["packets"], strict=True)
                if left["semantic_digest"] != right["semantic_digest"]
            ]
            expected_changed = sorted(
                candidate["id"]
                for package in self.document["packages"]
                for candidate in package["apis"]
                if candidate["declaration"]["source"] == api["declaration"]["source"]
            )
            self.assertEqual(changed, expected_changed)
            self.assert_code(before, "task_packet.source-drift", relocated)

    def test_catalog_signature_change_affects_only_its_packet(self) -> None:
        before = self.generated()
        changed_document = copy.deepcopy(self.document)
        api = next(
            api
            for package in changed_document["packages"]
            for api in package["apis"]
            if api["declaration"]["status"] == "exact"
        )
        signature = api["declaration"]["signature"] + " noexcept(false)"
        api["declaration"]["signature"] = signature
        api["declaration"]["signature_fingerprint"] = (
            "sha256:" + hashlib.sha256(signature.encode("utf-8")).hexdigest()
        )
        after = generate_corpus(changed_document, ROOT)
        changed = [
            left["api_id"]
            for left, right in zip(before["packets"], after["packets"], strict=True)
            if left["semantic_digest"] != right["semantic_digest"]
        ]
        self.assertEqual(changed, [api["id"]])
        with self.assertRaises(TaskPacketError) as raised:
            validate_corpus(before, changed_document, ROOT, self.schema)
        self.assertEqual(raised.exception.code, "task_packet.signature-drift")

    def test_cli_check_is_drift_free(self) -> None:
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "agent" / "task_packet_generator.py"),
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
            "family_split": self.mutate_family_split,
            "dangling_dependency": self.mutate_dangling_dependency,
            "unresolved_planned_signature": self.mutate_unresolved_ready,
            "unknown_schema": self.mutate_unknown_schema,
            "signature_drift": self.mutate_signature_drift,
            "duplicate_membership": self.mutate_duplicate_membership,
            "dependency_cycle": self.mutate_dependency_cycle,
            "hidden_shared_dependency": self.mutate_hidden_shared_dependency,
            "shared_steward_ambiguity": self.mutate_shared_steward_ambiguity,
            "maturity_only_readiness": self.mutate_unresolved_ready,
        }
        self.assertEqual({case["id"] for case in self.negative_cases}, set(mutators))
        for case in self.negative_cases:
            with self.subTest(case=case["id"]):
                corpus = self.generated()
                mutators[case["id"]](corpus)
                self.assert_code(corpus, case["expected_code"])

    def mutate_family_split(self, corpus: dict) -> None:
        packet = next(
            packet
            for packet in corpus["packets"]
            if packet["family_contract"]["mode"] == "coherent_family"
        )
        packet["atomic_unit_id"] = next(
            unit["id"] for unit in corpus["atomic_units"] if unit["id"] != packet["atomic_unit_id"]
        )

    def mutate_dangling_dependency(self, corpus: dict) -> None:
        corpus["packets"][0]["dependencies"]["apis"].append("API-NOT-999")

    def mutate_unresolved_ready(self, corpus: dict) -> None:
        packet = next(
            packet
            for packet in corpus["packets"]
            if packet["declaration"]["status"] == "unresolved"
            and packet["contract_maturity"] == "planned"
        )
        packet["generation"] = {"state": "ready", "block_reasons": []}

    @staticmethod
    def mutate_unknown_schema(corpus: dict) -> None:
        corpus["schema"] = "cxxlens.agent-task-packet-corpus.v999"

    def mutate_signature_drift(self, corpus: dict) -> None:
        packet = next(
            packet
            for packet in corpus["packets"]
            if packet["declaration"]["status"] == "exact"
        )
        packet["declaration"]["signature_fingerprint"] = "sha256:" + "0" * 64

    @staticmethod
    def mutate_duplicate_membership(corpus: dict) -> None:
        corpus["atomic_units"][1]["member_api_ids"].append(
            corpus["atomic_units"][0]["member_api_ids"][0]
        )

    def mutate_dependency_cycle(self, corpus: dict) -> None:
        first, second = corpus["packets"][:2]
        first["dependencies"]["apis"] = [second["api_id"]]
        first["dependencies"]["atomic_units"] = [second["atomic_unit_id"]]
        second["dependencies"]["apis"] = [first["api_id"]]
        second["dependencies"]["atomic_units"] = [first["atomic_unit_id"]]
        self.resign(corpus, (first["api_id"], second["api_id"]))

    def mutate_hidden_shared_dependency(self, corpus: dict) -> None:
        packet = corpus["packets"][0]
        packet["dependencies"]["components"].pop()
        self.resign(corpus, (packet["api_id"],))

    @staticmethod
    def mutate_shared_steward_ambiguity(corpus: dict) -> None:
        corpus["packets"][0]["coordination"]["shared_contract_steward_refs"] = [
            "steward-a",
            "steward-b",
        ]


if __name__ == "__main__":
    unittest.main()
