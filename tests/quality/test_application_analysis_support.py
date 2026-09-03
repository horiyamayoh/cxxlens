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
                "cxxlens_gcc_replay_input",
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

    def test_target_availability_tracks_implementation(self) -> None:
        self.assertEqual(
            {target["id"] for target in self.contract["targets"]},
            {"gcc-x86_64-linux", "msvc-x64-windows"},
        )
        by_id = {target["id"]: target for target in self.contract["targets"]}
        self.assertEqual(
            (by_id["gcc-x86_64-linux"]["implementation_state"],
             by_id["gcc-x86_64-linux"]["availability"]),
            ("materialization-ready", "experimental"),
        )
        self.assertEqual(
            (by_id["msvc-x64-windows"]["implementation_state"],
             by_id["msvc-x64-windows"]["availability"]),
            ("planned", "unavailable"),
        )
        for target in by_id.values():
            self.assertEqual(target["guarantee_floor"], "frontend-replayed")

    def test_only_materialization_ready_targets_can_be_available(self) -> None:
        targets = self.contract["targets"]
        planned_but_available = dict(targets[1])
        planned_but_available["availability"] = "experimental"
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(self.schema).validate(
                {**self.contract, "targets": [targets[0], planned_but_available]}
            )

        ready_but_unavailable = dict(targets[0])
        ready_but_unavailable["availability"] = "unavailable"
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(self.schema).validate(
                {**self.contract, "targets": [ready_but_unavailable, targets[1]]}
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
            "cxxlens_gcc_replay_input": "cxxlens.gcc-replay-input.v2",
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
        gcc_input = self.wire_contracts["cxxlens_gcc_replay_input"]
        self.assertIn("replay-fidelity", capture["authority"]["not_authoritative_for"])
        self.assertIn("production-compiler-exactness", replay["authority"]["not_authoritative_for"])
        self.assertIn("store-publication", detached["authority"]["not_authoritative_for"])
        self.assertIn("store-publication", gcc_input["authority"]["not_authoritative_for"])
        self.assertEqual(gcc_input["root_tuple"][3]["name"], "replay_plan_digest")
        self.assertEqual(gcc_input["root_tuple"][8]["name"], "source_closure_digest")
        self.assertEqual(capture["document_version"], "1.3.0")
        self.assertEqual(
            [field["type"] for field in capture["toolchain_tuple"]["fields"][6:]],
            ["captured-digest"] * 4,
        )
        self.assertEqual(
            replay["option_mapping_tuple"]["fields"][2]["values"],
            ["exact", "semantics_preserving", "approximation", "unsupported", "nonsemantic"],
        )
        closure_fields = capture["source_closure_member_tuple"]["fields"]
        self.assertEqual([field["index"] for field in closure_fields], list(range(8)))
        self.assertEqual(
            [field["name"] for field in closure_fields],
            [
                "file_id",
                "logical_path",
                "content_digest",
                "content",
                "size_bytes",
                "role",
                "encoding",
                "read_only",
            ],
        )
        source_closure_fields = capture["source_closure_tuple"]["fields"]
        self.assertEqual(
            [field["index"] for field in source_closure_fields], list(range(8))
        )
        self.assertEqual(source_closure_fields[-1]["name"], "membership_coverage")
        self.assertEqual(source_closure_fields[-1]["type"], "captured-string")
        self.assertEqual(source_closure_fields[-1]["values"], ["complete"])
        compile_unit_fields = capture["compile_unit_tuple"]["fields"]
        self.assertEqual(
            [field["name"] for field in compile_unit_fields[-4:]],
            [
                "captured_working_directory",
                "language_standard",
                "extension_mode",
                "source_closure_id",
            ],
        )
        self.assertEqual(capture["root_tuple"][6]["name"], "source_closures")


if __name__ == "__main__":
    unittest.main()
