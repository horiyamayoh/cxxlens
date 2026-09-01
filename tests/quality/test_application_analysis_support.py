#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import unittest

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]


class ApplicationAnalysisSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = yaml.safe_load(
            (ROOT / "schemas/cxxlens_application_analysis_support.yaml").read_text(
                encoding="utf-8"
            )
        )
        cls.schema = yaml.safe_load(
            (ROOT / "schemas/cxxlens_application_analysis_support.schema.yaml").read_text(
                encoding="utf-8"
            )
        )
        cls.wire_contracts = {
            name: yaml.safe_load((ROOT / f"schemas/{name}.yaml").read_text(encoding="utf-8"))
            for name in (
                "cxxlens_build_capture_bundle",
                "cxxlens_compiler_replay_plan",
                "cxxlens_detached_provider_run",
            )
        }

    def test_contract_validates(self) -> None:
        jsonschema.Draft202012Validator(self.schema).validate(self.contract)

    def test_host_support_remains_separate(self) -> None:
        self.assertEqual(
            self.contract["host_package_support"],
            {
                "authority": "schemas/cxxlens_support_matrix.yaml",
                "disposition": "unchanged",
            },
        )
        support = yaml.safe_load(
            (ROOT / "schemas/cxxlens_support_matrix.yaml").read_text(encoding="utf-8")
        )
        self.assertFalse(any(row["os"] == "windows" for row in support["entries"]))

    def test_unimplemented_targets_are_unavailable(self) -> None:
        self.assertEqual(
            {target["id"] for target in self.contract["targets"]},
            {"gcc-x86_64-linux", "msvc-x64-windows"},
        )
        for target in self.contract["targets"]:
            self.assertEqual(target["implementation_state"], "planned")
            self.assertEqual(target["availability"], "unavailable")
            self.assertEqual(target["guarantee_floor"], "frontend-replayed")

    def test_only_materialization_ready_targets_can_be_available(self) -> None:
        targets = self.contract["targets"]
        for index, target in enumerate(targets):
            candidate = dict(target)
            candidate["availability"] = "experimental"
            candidates = list(targets)
            candidates[index] = candidate
            with self.assertRaises(jsonschema.ValidationError):
                jsonschema.Draft202012Validator(self.schema).validate(
                    {**self.contract, "targets": candidates}
                )

        ready = dict(targets[0])
        ready["implementation_state"] = "materialization-ready"
        ready["availability"] = "experimental"
        jsonschema.Draft202012Validator(self.schema).validate(
            {**self.contract, "targets": [ready, targets[1]]}
        )

    def test_initial_relation_subset_is_existing_and_equal(self) -> None:
        registry = yaml.safe_load(
            (ROOT / "schemas/cxxlens_ng_relation_registry.yaml").read_text(encoding="utf-8")
        )
        admitted = {
            f"{relation['name']}.v{relation['semantic_major']}"
            for relation in registry["relations"]
        }
        relation_sets = [tuple(target["relations"]) for target in self.contract["targets"]]
        self.assertEqual(relation_sets[0], relation_sets[1])
        self.assertTrue(set(relation_sets[0]).issubset(admitted))
        self.assertNotIn("frontend.clang22.entity_observation.v2", relation_sets[0])

    def test_exact_toolchain_pins_are_not_latest_aliases(self) -> None:
        pins = self.contract["toolchain_pins"]
        self.assertEqual(pins["gcc"]["exact_version"], "16.2.0")
        self.assertEqual(pins["msvc"]["distribution_version"], "18.9.2")
        self.assertEqual(pins["msvc"]["exact_version"], "19.51.36247")
        self.assertEqual(pins["windows_sdk"]["exact_version"], "10.0.28000.2705")
        self.assertEqual(pins["clang_replay"]["exact_version"], "23.1.0")
        self.assertNotIn("latest", str(pins).lower())
        self.assertFalse(pins["windows_runner"]["ambient_defaults_authoritative"])

    def test_wire_authorities_are_distinct_and_bounded(self) -> None:
        expected = {
            "cxxlens_build_capture_bundle": "cxxlens.build-capture-bundle.v1",
            "cxxlens_compiler_replay_plan": "cxxlens.compiler-replay-plan.v1",
            "cxxlens_detached_provider_run": "cxxlens.detached-provider-run.v1",
        }
        self.assertEqual(
            {name: value["schema"] for name, value in self.wire_contracts.items()},
            expected,
        )
        for contract in self.wire_contracts.values():
            self.assertEqual(contract["encoding"], "cxxlens-canonical-tuple-v1")
            self.assertTrue(all(value > 0 for value in contract["bounds"].values()))
            self.assertIn("not_authoritative_for", contract["authority"])
            root = contract["root_tuple"]
            self.assertEqual([field["index"] for field in root], list(range(len(root))))
            self.assertEqual(len({field["name"] for field in root}), len(root))

        capture = self.wire_contracts["cxxlens_build_capture_bundle"]
        replay = self.wire_contracts["cxxlens_compiler_replay_plan"]
        detached = self.wire_contracts["cxxlens_detached_provider_run"]
        self.assertIn("replay-fidelity", capture["authority"]["not_authoritative_for"])
        self.assertIn("production-compiler-exactness", replay["authority"]["not_authoritative_for"])
        self.assertIn("store-publication", detached["authority"]["not_authoritative_for"])
        self.assertEqual(
            replay["option_mapping_tuple"]["fields"][2]["values"],
            ["exact", "semantics_preserving", "approximation", "unsupported", "nonsemantic"],
        )


if __name__ == "__main__":
    unittest.main()
