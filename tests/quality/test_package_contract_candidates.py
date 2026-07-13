#!/usr/bin/env python3
"""Positive and fail-closed tests for package Contract Candidate records."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_package_contract_candidates import (  # noqa: E402
    CandidateError,
    digest,
    validate_candidates,
)


def load(name: str) -> dict:
    return yaml.safe_load((ROOT / "schemas" / name).read_text(encoding="utf-8"))


class PackageContractCandidateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = load("cxxlens_package_contract_candidates.yaml")
        cls.schema = load("cxxlens_package_contract_candidates.schema.yaml")
        cls.catalog = load("cxxlens_public_api_contract.yaml")
        cls.conventions = load("cxxlens_global_contract_conventions.yaml")
        cls.ownership = load("cxxlens_contract_ownership.yaml")

    def assert_invalid(self, manifest: dict, pattern: str) -> None:
        catalog = copy.deepcopy(self.catalog)
        fingerprints = {
            group["issue"]: group["candidate_fingerprint"]
            for group in manifest["groups"]
        }
        for package in catalog["packages"]:
            issue = package["contract"]["owner_issue"]
            if package["contract"]["state"] == "candidate" and issue in fingerprints:
                package["contract"]["candidate_fingerprint"] = fingerprints[issue]
        with self.assertRaisesRegex(CandidateError, pattern):
            validate_candidates(
                manifest,
                self.schema,
                catalog,
                self.conventions,
                self.ownership,
                ROOT,
            )

    @staticmethod
    def resign(group: dict) -> None:
        unsigned = copy.deepcopy(group)
        unsigned.pop("candidate_fingerprint", None)
        group["candidate_fingerprint"] = digest(unsigned)

    def test_positive_issue_43_candidate(self) -> None:
        validate_candidates(
            self.manifest,
            self.schema,
            self.catalog,
            self.conventions,
            self.ownership,
            ROOT,
        )
        group = self.manifest["groups"][0]
        self.assertEqual(group["issue"], "#43")
        self.assertEqual(len(group["packages"]), 3)
        self.assertEqual(len(group["api_contracts"]), 17)
        self.assertFalse(group["production_implementation_changed"])

    def test_positive_issue_44_candidate_and_custom_fact_schema(self) -> None:
        group = next(row for row in self.manifest["groups"] if row["issue"] == "#44")
        self.assertEqual(group["packages"], ["facts", "interop", "workspace"])
        self.assertEqual(len(group["api_contracts"]), 22)
        self.assertFalse(group["production_implementation_changed"])

        schema = load("cxxlens_custom_fact.schema.yaml")
        fact_schema = load("cxxlens_fact.schema.yaml")
        source_schema = load("cxxlens_source_span.schema.yaml")
        resolver = jsonschema.RefResolver.from_schema(
            schema,
            store={
                "https://schemas.cxxlens.dev/cxxlens_fact.schema.yaml": fact_schema,
                "https://schemas.cxxlens.dev/cxxlens_source_span.schema.yaml": source_schema,
            },
        )
        validator = jsonschema.Draft202012Validator(schema, resolver=resolver)
        valid = {
            "schema": "cxxlens.custom-fact.v1",
            "provider_namespace": "dev.example.analysis",
            "schema_id": "dev.example.analysis.escape-summary",
            "schema_version": {"major": 1, "minor": 0, "patch": 0, "prerelease": ""},
            "semantic_key": "symbol:symbol_" + "a" * 64,
            "payload": {"escapes": False, "reasons": ["return"]},
            "source": None,
        }
        validator.validate(valid)

        native_pointer = copy.deepcopy(valid)
        native_pointer["payload"] = {"decl_ptr": "0x7fff12345678"}
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(native_pointer)
        name_only = copy.deepcopy(valid)
        name_only["semantic_key"] = "widget"
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(name_only)

    def test_positive_issue_45_candidate_and_selector_registry(self) -> None:
        group = next(row for row in self.manifest["groups"] if row["issue"] == "#45")
        self.assertEqual(group["packages"], ["explain", "search", "select"])
        self.assertEqual(len(group["api_contracts"]), 25)
        self.assertFalse(group["production_implementation_changed"])

        selector_schema = load("cxxlens_selector.schema.yaml")
        domains = set(selector_schema["properties"]["domain"]["enum"])
        self.assertEqual(
            domains,
            {"file", "symbol", "type", "expression", "reference", "call", "conversion", "include", "macro"},
        )
        reasons = load("cxxlens_selector_reason_codes.yaml")
        predicates = [(row["name"], row["reason_code"]) for row in reasons["predicates"]]
        self.assertEqual(len(predicates), len(set(predicates)))
        self.assertTrue(
            {"expression", "reference", "conversion", "include", "macro"}
            <= {row["domain"] for row in reasons["predicates"]}
        )

        search_schema = load("cxxlens_search_report.schema.yaml")
        self.assertEqual(
            set(search_schema["properties"]["result_kind"]["enum"]),
            {"symbol", "reference", "call", "inheritance", "override", "include", "macro", "conversion"},
        )
        jsonschema.Draft202012Validator.check_schema(load("cxxlens_search_options.schema.yaml"))

    def test_positive_issue_46_candidate_and_graph_schemas(self) -> None:
        group = next(row for row in self.manifest["groups"] if row["issue"] == "#46")
        self.assertEqual(group["packages"], ["graph"])
        self.assertEqual(len(group["api_contracts"]), 6)
        self.assertFalse(group["production_implementation_changed"])

        graph_schema = load("cxxlens_graph.schema.yaml")
        option_schema = load("cxxlens_graph_options.schema.yaml")
        jsonschema.Draft202012Validator.check_schema(graph_schema)
        jsonschema.Draft202012Validator.check_schema(option_schema)
        limits = option_schema["properties"]["limits"]["properties"]
        self.assertEqual(set(limits), {"max_depth", "max_nodes", "max_edges", "max_paths"})
        self.assertTrue(all(row["minimum"] == 1 for row in limits.values()))
        self.assertEqual(
            set(graph_schema["$defs"]["edge"]["properties"]["kind"]["enum"]),
            {"base", "override_relation", "direct_call", "possible_dynamic_call", "indirect_call", "include", "impact", "custom"},
        )

    def test_positive_issue_47_candidate_and_rule_report_schemas(self) -> None:
        group = next(row for row in self.manifest["groups"] if row["issue"] == "#47")
        self.assertEqual(group["packages"], ["report", "rules"])
        self.assertEqual(len(group["api_contracts"]), 9)
        self.assertFalse(group["production_implementation_changed"])
        for name in (
            "cxxlens_rule.schema.yaml",
            "cxxlens_suppression.schema.yaml",
            "cxxlens_report_options.schema.yaml",
            "cxxlens_rendered_report.schema.yaml",
        ):
            jsonschema.Draft202012Validator.check_schema(load(name))
        suppression = load("cxxlens_suppression.schema.yaml")
        self.assertEqual(
            suppression["properties"]["precedence"]["const"],
            ["inline_source", "external_configuration", "baseline"],
        )
        report_options = load("cxxlens_report_options.schema.yaml")
        self.assertEqual(report_options["properties"]["output_budget_bytes"]["minimum"], 1)
        self.assertNotIn("absolute", report_options["properties"]["paths"]["enum"])

    def test_positive_issue_48_candidate_and_transaction_contract(self) -> None:
        group = next(row for row in self.manifest["groups"] if row["issue"] == "#48")
        self.assertEqual(group["packages"], ["transform"])
        self.assertEqual(len(group["api_contracts"]), 9)
        self.assertFalse(group["production_implementation_changed"])
        for name in (
            "cxxlens_transform_options.schema.yaml",
            "cxxlens_edit_plan.schema.yaml",
            "cxxlens_apply_result.schema.yaml",
        ):
            jsonschema.Draft202012Validator.check_schema(load(name))
        options = load("cxxlens_transform_options.schema.yaml")
        self.assertEqual(options["properties"]["apply_mode"]["const"], "dry_run")
        transaction = load("cxxlens_transform_transaction.yaml")
        self.assertFalse(transaction["partial_commit_allowed"])
        self.assertFalse(transaction["silent_rebase_allowed"])
        self.assertIn("rollback_failed", transaction["terminal_states"])
        plan = load("cxxlens_edit_plan.schema.yaml")
        required = set(plan["$defs"]["edit"]["properties"]["precondition"]["required"])
        self.assertTrue({"source_digest", "file_identity", "catalog_version", "fact_snapshot_id", "verified_variants"} <= required)

    def test_positive_issue_49_candidate_and_generation_schemas(self) -> None:
        group = next(row for row in self.manifest["groups"] if row["issue"] == "#49")
        self.assertEqual(group["packages"], ["copy", "fuzz", "generate", "method_harness", "mock"])
        self.assertEqual(len(group["api_contracts"]), 15)
        self.assertFalse(group["production_implementation_changed"])
        for name in (
            "cxxlens_generation_plan.schema.yaml",
            "cxxlens_generation_result.schema.yaml",
            "cxxlens_method_spec.schema.yaml",
            "cxxlens_generation_options.schema.yaml",
        ):
            jsonschema.Draft202012Validator.check_schema(load(name))
        plan = load("cxxlens_generation_plan.schema.yaml")
        states = set(plan["properties"]["decisions"]["items"]["properties"]["state"]["enum"])
        self.assertEqual(states, {"requested", "accepted", "excluded", "unsupported", "ambiguous", "failed", "deferred"})
        artifact_state = plan["$defs"]["artifact"]["properties"]["state"]["required"]
        self.assertTrue({"present", "publishable", "usable", "link_ready", "listed", "quarantined"} <= set(artifact_state))
        result = load("cxxlens_generation_result.schema.yaml")
        self.assertIn("rollback_failed", result["properties"]["state"]["enum"])

    def test_positive_issue_50_candidate_and_flow_model_schemas(self) -> None:
        group = next(row for row in self.manifest["groups"] if row["issue"] == "#50")
        self.assertEqual(group["packages"], ["flow", "models"])
        self.assertEqual(len(group["api_contracts"]), 12)
        self.assertFalse(group["production_implementation_changed"])
        for name in (
            "cxxlens_flow_options.schema.yaml",
            "cxxlens_taint_report.schema.yaml",
            "cxxlens_resource_report.schema.yaml",
            "cxxlens_effect_summary.schema.yaml",
            "cxxlens_api_model_pack.schema.yaml",
        ):
            jsonschema.Draft202012Validator.check_schema(load(name))
        taint = load("cxxlens_taint_report.schema.yaml")
        states = set(taint["$defs"]["cfg"]["properties"]["state"]["enum"])
        self.assertEqual(states, {"available", "absent", "unsupported", "failed", "partial", "stale", "variant_divergent"})
        options = load("cxxlens_flow_options.schema.yaml")
        for name in ("max_paths", "max_steps", "max_states", "widening_after_iterations"):
            self.assertEqual(options["properties"][name]["minimum"], 1)
        pack = load("cxxlens_api_model_pack.schema.yaml")
        self.assertEqual(pack["properties"]["compatibility"]["properties"]["unknown_kind_policy"]["const"], "reject")

    def test_positive_issue_51_candidate_and_review_qa_schemas(self) -> None:
        group = next(row for row in self.manifest["groups"] if row["issue"] == "#51")
        self.assertEqual(group["packages"], ["qa", "review"])
        self.assertEqual(len(group["api_contracts"]), 9)
        self.assertFalse(group["production_implementation_changed"])
        for name in (
            "cxxlens_review_diff.schema.yaml",
            "cxxlens_review_baseline.schema.yaml",
            "cxxlens_review_report.schema.yaml",
            "cxxlens_qa_profile.schema.yaml",
            "cxxlens_qa_report.schema.yaml",
        ):
            jsonschema.Draft202012Validator.check_schema(load(name))
        review = load("cxxlens_review_report.schema.yaml")
        self.assertEqual(
            set(review["properties"]["gate"]["properties"]["state"]["enum"]),
            {"pass", "warn", "fail", "indeterminate"},
        )
        profile = load("cxxlens_qa_profile.schema.yaml")
        self.assertEqual(
            set(profile["properties"]["steps"]["items"]["properties"]["requirement"]["enum"]),
            {"required", "optional"},
        )
        report = load("cxxlens_qa_report.schema.yaml")
        states = set(report["properties"]["steps"]["items"]["properties"]["state"]["enum"])
        self.assertTrue({"unavailable", "unsupported", "timed_out", "crashed", "partial"} <= states)

    def test_missing_assigned_api_is_rejected(self) -> None:
        document = copy.deepcopy(self.manifest)
        group = document["groups"][0]
        group["api_contracts"].pop()
        self.resign(group)
        self.assert_invalid(document, "API coverage differs")

    def test_duplicate_api_is_rejected(self) -> None:
        document = copy.deepcopy(self.manifest)
        group = document["groups"][0]
        group["api_contracts"].append(copy.deepcopy(group["api_contracts"][0]))
        self.resign(group)
        self.assert_invalid(document, "duplicate API contract")

    def test_signature_drift_is_rejected(self) -> None:
        document = copy.deepcopy(self.manifest)
        group = document["groups"][0]
        group["api_contracts"][0]["declaration"]["signature"] += " noexcept"
        self.resign(group)
        self.assert_invalid(document, "exact declaration differs")

    def test_missing_result_outcome_is_rejected(self) -> None:
        document = copy.deepcopy(self.manifest)
        group = document["groups"][0]
        del group["policies"][0]["result_semantics"]["ambiguous"]
        self.resign(group)
        self.assert_invalid(document, "schema validation failed")

    def test_dangling_public_type_owner_is_rejected(self) -> None:
        document = copy.deepcopy(self.manifest)
        group = document["groups"][0]
        group["api_contracts"][0]["ownership_refs"]["public_types"] = [
            "public-type:none:none"
        ]
        self.resign(group)
        self.assert_invalid(document, "dangling public type owner")

    def test_duplicated_registry_owner_is_rejected(self) -> None:
        document = copy.deepcopy(self.manifest)
        group = document["groups"][0]
        duplicate = copy.deepcopy(group["registry_owners"][0])
        duplicate["id"] += ".duplicate"
        group["registry_owners"].append(duplicate)
        self.resign(group)
        self.assert_invalid(document, "multiple owners")

    def test_public_header_preemption_is_rejected(self) -> None:
        document = copy.deepcopy(self.manifest)
        group = document["groups"][0]
        group["candidate_headers"][0] = "include/cxxlens/core.hpp"
        self.resign(group)
        self.assert_invalid(document, "cannot pre-empt #53")

    def test_candidate_fingerprint_drift_is_rejected(self) -> None:
        document = copy.deepcopy(self.manifest)
        document["groups"][0]["candidate_fingerprint"] = "sha256:" + "0" * 64
        self.assert_invalid(document, "candidate fingerprint mismatch")


if __name__ == "__main__":
    unittest.main()
