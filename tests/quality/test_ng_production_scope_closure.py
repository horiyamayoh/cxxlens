#!/usr/bin/env python3

from __future__ import annotations

import copy
import dataclasses
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/quality"))

import check_ng_production_scope_closure as closure  # noqa: E402


REVISION = "1" * 40
TREE = "2" * 40
GIT = {"revision": REVISION, "tree": TREE, "branch": "main", "clean": True}


class ProductionScopeClosureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.model = closure.validate_repository(ROOT)
        cls.static_binding = closure.binding_from_model(cls.model)

    def clone_contract_root(self) -> tuple[tempfile.TemporaryDirectory[str], Path]:
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        (root / "tools/quality").mkdir(parents=True)
        (root / "schemas").mkdir()
        shutil.copy2(ROOT / closure.CHECKER, root / closure.CHECKER)
        schema_files = {
            *closure.SOURCE_CONTRACTS,
            *closure.EVIDENCE_CONTRACTS,
            closure.MANIFEST.as_posix(),
            closure.MANIFEST_SCHEMA.as_posix(),
            closure.REPORT_SCHEMA.as_posix(),
            closure.EVALUATION_SCHEMA.as_posix(),
            closure.GR_REPORT_SCHEMA.as_posix(),
        }
        for relative in sorted(schema_files):
            shutil.copy2(ROOT / relative, root / relative)
        os.symlink(ROOT / "docs", root / "docs", target_is_directory=True)
        os.symlink(ROOT / "tests", root / "tests", target_is_directory=True)
        return temporary, root

    @staticmethod
    def read_manifest(root: Path) -> dict:
        return yaml.safe_load((root / closure.MANIFEST).read_text(encoding="utf-8"))

    @staticmethod
    def write_manifest(root: Path, manifest: dict) -> None:
        (root / closure.MANIFEST).write_text(
            yaml.safe_dump(manifest, sort_keys=False), encoding="utf-8"
        )

    def evaluation(self, qualification: str) -> dict:
        digest = "sha256:" + "a" * 64
        inventory = copy.deepcopy(self.static_binding)
        return {
            "schema": closure.EVALUATION_SCHEMA_ID,
            "result": "passed",
            "generated_at": "2026-07-19T00:00:00+00:00",
            "run_url": "https://example.invalid/actions/evaluation",
            "git": GIT,
            "release": {"id": "distribution-1.0", "version": "1.0.0"},
            "qualification": qualification,
            "qualified": qualification == "qualified",
            "scope_inventory": inventory,
            "evidence": {
                "foundation_report_digest": digest,
                "readiness_report_digest": digest,
                "public_callable_report_digest": digest,
                "g5_report_digest": digest,
                "security_report_digest": digest,
                "install_manifests": [
                    {"configuration": "shared", "manifest_digest": digest, "prefix_digest": digest},
                    {"configuration": "static", "manifest_digest": digest, "prefix_digest": digest},
                ],
                "same_revision": True,
            },
            "production_support": [],
            "strict_report": {
                "schema": closure.GR_SCHEMA_ID,
                "eligible": qualification == "qualified",
                "emitted": False,
            },
            "reason_codes": [] if qualification == "qualified" else ["release.scope-not-qualified"],
        }

    def gr_report(self, production_support: list[dict]) -> dict:
        digest = "sha256:" + "b" * 64
        return {
            "schema": closure.GR_SCHEMA_ID,
            "result": "passed",
            "generated_at": "2026-07-19T00:00:00+00:00",
            "run_url": "https://example.invalid/actions/gr",
            "git": GIT,
            "release": {"id": "distribution-1.0", "version": "1.0.0", "state": "qualified"},
            "prerequisites": {
                "gates": [*[f"gate.g{i}" for i in range(6)], "gate.release"],
                "migrations": [f"R{i}" for i in range(5)],
                "foundation_report_digest": digest,
                "readiness_report_digest": digest,
                "public_callable_report_digest": digest,
                "g5_report_digest": digest,
                "release_evaluation_report_digest": "sha256:" + "e" * 64,
                "same_revision": True,
            },
            "packages": [
                {
                    "configuration": configuration,
                    "prefix_digest": digest,
                    "manifest_digest": digest,
                    "toolchain_digest": digest,
                    "real_project": "passed",
                    "storage_backends": ["memory", "sqlite"],
                    "relocated": True,
                    "license": digest,
                    "notice": digest,
                }
                for configuration in ("shared", "static")
            ],
            "production_support": production_support,
            "security": {"status": "green", "contract_digest": digest, "vector_count": 1},
            "performance": {
                "report_digest": digest,
                "warm_zero_plan_median_us": 1,
                "bounded_closure_median_us": 1,
            },
            "documentation": {
                "doxygen_contract": "passed",
                "doxygen_callable_count": 536,
                "doxygen_digest": digest,
                "public_callable_report": {
                    "path": "cxxlens-ng-public-callable-inventory-report.json",
                    "digest": digest,
                },
                "public_callable_inventory": {
                    "path": "schemas/cxxlens_ng_public_callable_inventory.yaml",
                    "file_digest": digest,
                    "semantic_digest": digest,
                    "callable_count": 536,
                },
                "public_callable_review": {
                    "path": "cxxlens-ng-public-callable-inventory-review.md",
                    "digest": digest,
                },
                "support_matrix": "exact-report-only",
            },
            "negative_evidence": ["a", "b", "c", "d"],
            "authority_digests": [
                {"path": f"authority-{index}", "digest": digest} for index in range(6)
            ],
        }

    def test_repository_check_closes_exact_30_domains(self) -> None:
        self.assertEqual(tuple(sorted({key.domain for key in self.model.nodes})), tuple(sorted(closure.DOMAINS)))
        self.assertEqual(self.model.summary["domain_count"], 30)
        self.assertEqual(self.model.summary["assignable_count"], 223)
        self.assertEqual(self.model.summary["expanded_count"], 773)
        self.assertEqual(self.model.summary["aggregate_count"], 14)
        self.assertEqual(self.model.closure_status, "classified-with-gaps")

    def test_callable_rows_inherit_exact_catalog_assignment(self) -> None:
        callable_rows = [row for row in self.model.expanded if row["domain"] == "public.callable"]
        self.assertEqual(len(callable_rows), 536)
        self.assertTrue(all(row["inherited_from"]["domain"] == "public.catalog-entry" for row in callable_rows))
        incremental = [row for row in callable_rows if row["inherited_from"]["id"] == "public.incremental"]
        self.assertTrue(incremental)
        self.assertTrue(all(row["qualification"] == "tracked-gap" for row in incremental))
        self.assertFalse(
            any(
                surface["domain"] == "public.callable"
                for assignment in self.model.manifest["assignments"]
                for surface in assignment["surfaces"]
            )
        )

    def test_known_gap_and_blocker_census_is_truthful(self) -> None:
        assignments = {assignment["id"]: assignment for assignment in self.model.manifest["assignments"]}
        self.assertEqual(len(assignments["scope.clang22-installed-adoption-gap"]["surfaces"]), 8)
        self.assertEqual(len(assignments["scope.ng1-provider-hardening-gap"]["surfaces"]), 6)
        self.assertEqual(len(assignments["scope.incremental-coordinator-gap"]["surfaces"]), 6)
        self.assertEqual(len(assignments["scope.nightly-qualification-gap"]["surfaces"]), 4)
        self.assertEqual(
            {
                row["id"]
                for row in assignments["scope.nightly-qualification-gap"]["surfaces"]
            },
            {
                "analysis.clang-tidy",
                "quality.production-contracts",
                "sanitizer.asan-ubsan",
                "sanitizer.tsan",
            },
        )
        self.assertEqual(self.model.blocking_feedback, ("DF-0174", "DF-0182"))

    def test_conformance_only_support_is_never_promoted(self) -> None:
        rows = [row for row in self.model.expanded if row["domain"] == "provider.support-tuple"]
        self.assertEqual(len(rows), 5)
        self.assertTrue(all(row["disposition"] == "explicit-non-1.0" for row in rows))
        self.assertTrue(all(row["qualification"] == "not-applicable" for row in rows))

    def test_aggregate_contracts_are_derived_without_leaf_ownership(self) -> None:
        rows = {(row["domain"], row["id"]): row for row in self.model.expanded}
        for key in (
            ("release.gate", "gate.release"),
            ("release.gate", "gate.quality-evidence"),
            ("release.profile", "NG1"),
        ):
            self.assertEqual(rows[key]["kind"], "aggregate")
            self.assertEqual(rows[key]["derived_qualification"], "tracked-gap")
            self.assertNotIn("assignment", rows[key])
            self.assertNotIn("owner_issue", rows[key])
            self.assertNotIn("qualification", rows[key])
            self.assertTrue(rows[key]["derived_from"])
        self.assertEqual(self.model.closure_status, "classified-with-gaps")
        self.assertGreater(self.model.summary["tracked_gap"], 0)

    def test_inventory_binding_is_static_and_importable(self) -> None:
        binding = closure.inventory_binding(ROOT)
        self.assertEqual(
            set(binding),
            {
                "manifest_path",
                "manifest_digest",
                "authority_census_digest",
                "evidence_census_digest",
                "classification_digest",
                "evidence_tests",
                "summary",
                "closure_status",
            },
        )
        self.assertEqual(binding["closure_status"], "classified-with-gaps")
        self.assertNotIn("release_evaluation_digest", binding)
        self.assertNotIn("release_qualification_digest", binding)

    def test_normal_report_accepts_green_not_qualified_evaluation(self) -> None:
        evaluation = self.evaluation("not-qualified")
        evaluation_digest = "sha256:" + "e" * 64
        report = closure.build_report(
            self.model,
            mode="normal",
            evaluation=evaluation,
            git=GIT,
            run_url="https://example.invalid/actions/179",
            generated_at="2026-07-19T00:00:00+00:00",
            evaluation_digest=evaluation_digest,
        )
        self.assertEqual(report["closure_status"], "classified-with-gaps")
        self.assertEqual(report["upstream"], {"release_evaluation_digest": evaluation_digest})
        self.assertNotIn("release_qualification_digest", report["upstream"])
        self.assertEqual(len(report["domains"]), 30)
        with self.assertRaisesRegex(closure.ContractError, "run_url"):
            closure.build_report(
                self.model,
                mode="normal",
                evaluation=evaluation,
                git=GIT,
                run_url="not a URI",
                generated_at="not a date-time",
                evaluation_digest=evaluation_digest,
            )

    def test_terminal_report_exact_sets_and_derived_content_fail_closed(self) -> None:
        report = closure.build_report(
            self.model,
            mode="normal",
            evaluation=self.evaluation("not-qualified"),
            git=GIT,
            run_url="https://example.invalid/actions/179",
            generated_at="2026-07-19T00:00:00+00:00",
            evaluation_digest="sha256:" + "e" * 64,
        )
        schema_rejections = []
        duplicate_domain = copy.deepcopy(report)
        duplicate_domain["domains"][1] = {
            **duplicate_domain["domains"][0],
            "digest": "sha256:" + "0" * 64,
        }
        schema_rejections.append(duplicate_domain)
        duplicate_source = copy.deepcopy(report)
        duplicate_source["binding"]["source_digests"][1] = {
            **duplicate_source["binding"]["source_digests"][0],
            "digest": "sha256:" + "0" * 64,
        }
        schema_rejections.append(duplicate_source)
        duplicate_census = copy.deepcopy(report)
        duplicate_census["evidence_censuses"][1] = {
            **duplicate_census["evidence_censuses"][0],
            "digest": "sha256:" + "0" * 64,
        }
        schema_rejections.append(duplicate_census)
        missing_domains = copy.deepcopy(report)
        missing_domains["domains"] = []
        schema_rejections.append(missing_domains)
        missing_sources = copy.deepcopy(report)
        missing_sources["binding"]["source_digests"] = []
        schema_rejections.append(missing_sources)
        missing_censuses = copy.deepcopy(report)
        missing_censuses["evidence_censuses"] = []
        schema_rejections.append(missing_censuses)
        for changed in schema_rejections:
            with self.assertRaisesRegex(closure.ContractError, "validation failed"):
                closure.validate_schema(changed, ROOT / closure.REPORT_SCHEMA)

        duplicate_surface = copy.deepcopy(report)
        duplicate_surface["surfaces"][1] = copy.deepcopy(
            duplicate_surface["surfaces"][0]
        )
        with self.assertRaisesRegex(closure.ContractError, "duplicate typed surface"):
            closure.validate_report_exactness(self.model, duplicate_surface)
        stale_domain = copy.deepcopy(report)
        stale_domain["domains"][0]["count"] += 1
        with self.assertRaisesRegex(closure.ContractError, "domain census differs"):
            closure.validate_report_exactness(self.model, stale_domain)

    def test_final_report_rejects_current_gap_contract(self) -> None:
        evaluation = self.evaluation("not-qualified")
        with self.assertRaisesRegex(closure.ContractError, "final mode forbids"):
            closure.build_report(
                self.model,
                mode="final",
                evaluation=evaluation,
                git=GIT,
                gr={},
                run_url="https://example.invalid/actions/179",
                generated_at="2026-07-19T00:00:00+00:00",
                evaluation_digest="sha256:" + "e" * 64,
            )

    def test_all_qualified_final_requires_and_accepts_exact_six_gr_templates(self) -> None:
        expanded = copy.deepcopy(self.model.expanded)
        for row in expanded:
            if row["kind"] == "aggregate":
                if row["derived_qualification"] == "tracked-gap":
                    row["derived_scope"] = "included"
                    row["derived_qualification"] = "qualified"
                continue
            if row["qualification"] in {"tracked-gap", "blocked"}:
                row["scope"] = "included"
                row["qualification"] = "qualified"
                row["disposition"] = "qualification-evidence"
                row["evidence"] = list(
                    closure.evidence_for_surface(
                        closure.SurfaceKey(row["domain"], row["id"])
                    )
                )
        manifest = copy.deepcopy(self.model.manifest)
        for assignment in manifest["assignments"]:
            assignment.pop("gap", None)
        summary = dict(self.model.summary)
        summary["qualified"] += summary["tracked_gap"] + summary["blocked"]
        summary["tracked_gap"] = 0
        summary["blocked"] = 0
        qualified_model = dataclasses.replace(
            self.model,
            manifest=manifest,
            expanded=expanded,
            blocking_feedback=(),
            summary=summary,
            closure_status="qualified",
        )
        evaluation = self.evaluation("qualified")
        evaluation["scope_inventory"] = {
            **evaluation["scope_inventory"],
            "summary": summary,
            "closure_status": "qualified",
        }
        production_support = []
        for key in sorted(qualified_model.nodes):
            if key.domain != "provider.production-tuple-template":
                continue
            configuration, relation = key.id.split("/", 1)
            production_support.append({
                "provider_id": "cxxlens.clang22.reference",
                "provider_version": "1.0.0",
                "binary_digest": "sha256:" + "b" * 64,
                "relation": relation,
                "interpretation": "cc.clang22-canonical-1",
                "toolchain": "clang-22.1.0",
                "platform": f"linux-x86_64-{configuration}",
                "status": "production-supported",
                "capabilities": ["canonical-entity", "call-site", "direct-target", "process-isolation"],
                "guarantee": "exact-claims-with-coverage-unresolved-and-provenance",
                "security_profile_digest": "sha256:" + "b" * 64,
                "evidence_digest": "sha256:" + "b" * 64,
            })
        evaluation_digest = "sha256:" + "e" * 64
        gr = self.gr_report(production_support)
        gr_digest = "sha256:" + "f" * 64
        with self.assertRaisesRegex(closure.ContractError, "qualified closure requires final"):
            closure.build_report(
                qualified_model,
                mode="normal",
                evaluation=evaluation,
                git=GIT,
                evaluation_digest=evaluation_digest,
                run_url="https://example.invalid/actions/final",
                generated_at="2026-07-19T00:00:00+00:00",
            )
        with self.assertRaisesRegex(closure.ContractError, "requires qualified evaluation and exact GR"):
            closure.build_report(
                qualified_model,
                mode="final",
                evaluation=evaluation,
                git=GIT,
                evaluation_digest=evaluation_digest,
                run_url="https://example.invalid/actions/final",
                generated_at="2026-07-19T00:00:00+00:00",
            )
        malformed_gr = copy.deepcopy(gr)
        malformed_gr.pop("packages")
        with self.assertRaisesRegex(closure.ContractError, "validation failed"):
            closure.build_report(
                qualified_model,
                mode="final",
                evaluation=evaluation,
                git=GIT,
                evaluation_digest=evaluation_digest,
                gr=malformed_gr,
                gr_digest=gr_digest,
                run_url="https://example.invalid/actions/final",
                generated_at="2026-07-19T00:00:00+00:00",
            )
        stale_gr = copy.deepcopy(gr)
        stale_gr["prerequisites"]["release_evaluation_report_digest"] = (
            "sha256:" + "d" * 64
        )
        with self.assertRaisesRegex(
            closure.ContractError, "does not bind the exact evaluation artifact"
        ):
            closure.build_report(
                qualified_model,
                mode="final",
                evaluation=evaluation,
                git=GIT,
                evaluation_digest=evaluation_digest,
                gr=stale_gr,
                gr_digest=gr_digest,
                run_url="https://example.invalid/actions/final",
                generated_at="2026-07-19T00:00:00+00:00",
            )
        report = closure.build_report(
            qualified_model,
            mode="final",
            evaluation=evaluation,
            git=GIT,
            gr=gr,
            run_url="https://example.invalid/actions/final",
            generated_at="2026-07-19T00:00:00+00:00",
            evaluation_digest=evaluation_digest,
            gr_digest=gr_digest,
        )
        self.assertEqual(report["closure_status"], "qualified")
        self.assertIn("release_qualification_digest", report["upstream"])
        self.assertEqual(report["findings"], [])

    def test_missing_surface_fails_closed(self) -> None:
        temporary, root = self.clone_contract_root()
        self.addCleanup(temporary.cleanup)
        manifest = self.read_manifest(root)
        manifest["assignments"][0]["surfaces"].pop()
        self.write_manifest(root, manifest)
        with self.assertRaisesRegex(closure.ContractError, "partition is not exact"):
            closure.validate_repository(root)

    def test_duplicate_surface_fails_closed(self) -> None:
        temporary, root = self.clone_contract_root()
        self.addCleanup(temporary.cleanup)
        manifest = self.read_manifest(root)
        manifest["assignments"][1]["surfaces"].append(manifest["assignments"][1]["surfaces"][0])
        self.write_manifest(root, manifest)
        with self.assertRaisesRegex(closure.ContractError, "unique"):
            closure.validate_repository(root)

    def test_source_item_addition_rejects_stale_census(self) -> None:
        temporary, root = self.clone_contract_root()
        self.addCleanup(temporary.cleanup)
        quality_path = root / "schemas/cxxlens_ng_quality_ownership.yaml"
        quality = yaml.safe_load(quality_path.read_text(encoding="utf-8"))
        quality["checks"].append(
            {
                "id": "new.unclassified-check",
                "version": 1,
                "owner": "fixture",
                "command": "true",
                "inputs": ["schemas"],
                "dimensions": ["fixture"],
                "configurations": ["fixture"],
            }
        )
        quality_path.write_text(yaml.safe_dump(quality, sort_keys=False), encoding="utf-8")
        with self.assertRaisesRegex(closure.ContractError, "stale authority census digest"):
            closure.validate_repository(root)

    def test_semantic_evidence_census_drift_is_rejected_without_id_or_count_change(self) -> None:
        mutations = (
            (
                "schemas/cxxlens_ng_security_profile.yaml",
                lambda document: document["reason_codes"].__setitem__(
                    0, "security.audit-evidence-incomplete-v2"
                ),
            ),
            (
                "schemas/cxxlens_ng_provider_conformance_vectors.yaml",
                lambda document: document["vectors"][0]["expected"].__setitem__(
                    "reason_code", "provider.wire-valid-v2"
                ),
            ),
            (
                "schemas/cxxlens_ng_g5_qualification.yaml",
                lambda document: document["required_artifacts"].__setitem__(
                    0, "include/cxxlens/sdk/query.hpp"
                ),
            ),
        )
        for relative, mutate in mutations:
            with self.subTest(relative=relative):
                temporary, root = self.clone_contract_root()
                self.addCleanup(temporary.cleanup)
                path = root / relative
                document = yaml.safe_load(path.read_text(encoding="utf-8"))
                mutate(document)
                path.write_text(
                    yaml.safe_dump(document, sort_keys=False), encoding="utf-8"
                )
                with self.assertRaisesRegex(
                    closure.ContractError, "stale authority census digest"
                ):
                    closure.validate_repository(root)

    def test_unrelated_ctest_id_cannot_self_qualify_surfaces(self) -> None:
        temporary, root = self.clone_contract_root()
        self.addCleanup(temporary.cleanup)
        manifest = self.read_manifest(root)
        manifest["assignments"][0]["evidence"] = ["quality.ng-g5_qualification"]
        self.write_manifest(root, manifest)
        with self.assertRaisesRegex(closure.ContractError, "typed evidence mismatch"):
            closure.validate_repository(root)

    def test_aggregate_cannot_reenter_manifest_ownership(self) -> None:
        temporary, root = self.clone_contract_root()
        self.addCleanup(temporary.cleanup)
        manifest = self.read_manifest(root)
        qualified = manifest["assignments"][0]
        qualified["surfaces"].append(
            {"domain": "release.gate", "id": "gate.release"}
        )
        qualified["surfaces"].sort(key=lambda row: (row["domain"], row["id"]))
        self.write_manifest(root, manifest)
        with self.assertRaisesRegex(closure.ContractError, "aggregate surface must be derived"):
            closure.validate_repository(root)

    def test_unsupported_exclusion_fails_closed(self) -> None:
        temporary, root = self.clone_contract_root()
        self.addCleanup(temporary.cleanup)
        manifest = self.read_manifest(root)
        qualified = manifest["assignments"][0]
        excluded = next(row for row in manifest["assignments"] if row["id"] == "scope.explicit-non-1.0")
        surface = qualified["surfaces"].pop(0)
        excluded["surfaces"].insert(0, surface)
        self.write_manifest(root, manifest)
        with self.assertRaisesRegex(closure.ContractError, "unsupported classification"):
            closure.validate_repository(root)

    def test_unmapped_active_blocking_feedback_fails_closed(self) -> None:
        temporary, root = self.clone_contract_root()
        self.addCleanup(temporary.cleanup)
        manifest = self.read_manifest(root)
        clang_gap = next(
            row for row in manifest["assignments"] if row["id"] == "scope.clang22-installed-adoption-gap"
        )
        clang_gap.pop("feedback")
        self.write_manifest(root, manifest)
        with self.assertRaisesRegex(closure.ContractError, r"not mapped: \['DF-0182'\]"):
            closure.validate_repository(root)

    def test_feedback_exclusion_cannot_be_self_authorized(self) -> None:
        temporary, root = self.clone_contract_root()
        self.addCleanup(temporary.cleanup)
        manifest = self.read_manifest(root)
        manifest["feedback_exclusions"] = [
            {
                "feedback": "DF-0182",
                "surfaces": [{"domain": "release.profile", "id": "NG2"}],
                "authority": "schemas/cxxlens_ng_release_bundle.yaml",
                "reason": "fixture attempts to exclude an unrelated blocker",
            }
        ]
        self.write_manifest(root, manifest)
        with self.assertRaisesRegex(closure.ContractError, "must remain empty"):
            closure.validate_repository(root)

    def test_evidence_free_qualification_and_invalid_pair_fail_schema(self) -> None:
        temporary, root = self.clone_contract_root()
        self.addCleanup(temporary.cleanup)
        manifest = self.read_manifest(root)
        manifest["assignments"][0].pop("evidence")
        self.write_manifest(root, manifest)
        with self.assertRaisesRegex(closure.ContractError, "validation failed"):
            closure.validate_repository(root)

        manifest = self.read_manifest(ROOT)
        manifest["assignments"][0]["scope"] = "unresolved"
        self.write_manifest(root, manifest)
        with self.assertRaisesRegex(closure.ContractError, "validation failed"):
            closure.validate_repository(root)

    def test_normal_rejects_gr_and_mixed_revision(self) -> None:
        evaluation = self.evaluation("not-qualified")
        evaluation["git"] = {**GIT, "tree": "3" * 40}
        with self.assertRaisesRegex(closure.ContractError, "tree does not match"):
            closure.build_report(
                self.model,
                mode="normal",
                evaluation=evaluation,
                git=GIT,
                run_url="https://example.invalid/actions/179",
                generated_at="2026-07-19T00:00:00+00:00",
                evaluation_digest="sha256:" + "e" * 64,
            )

        evaluation["git"] = GIT
        with self.assertRaisesRegex(closure.ContractError, "normal mode must not consume GR"):
            closure.build_report(
                self.model,
                mode="normal",
                evaluation=evaluation,
                git=GIT,
                gr={},
                run_url="https://example.invalid/actions/179",
                generated_at="2026-07-19T00:00:00+00:00",
                evaluation_digest="sha256:" + "e" * 64,
            )

    def test_expected_revision_and_upstream_schema_fail_closed(self) -> None:
        with self.assertRaisesRegex(closure.ContractError, "does not match --expected-revision"):
            closure.require_expected_revision(GIT, "3" * 40)
        with self.assertRaisesRegex(closure.ContractError, "out of tree"):
            closure.require_out_of_tree(ROOT, ROOT / "scope-report.json")
        closure.require_out_of_tree(ROOT, Path(tempfile.gettempdir()) / "scope-report.json")
        malformed = self.evaluation("not-qualified")
        malformed.pop("scope_inventory")
        with self.assertRaisesRegex(closure.ContractError, "validation failed"):
            closure.build_report(
                self.model,
                mode="normal",
                evaluation=malformed,
                git=GIT,
                evaluation_digest="sha256:" + "e" * 64,
                run_url="https://example.invalid/actions/179",
                generated_at="2026-07-19T00:00:00+00:00",
            )
        stale = self.evaluation("not-qualified")
        stale["scope_inventory"]["classification_digest"] = "sha256:" + "9" * 64
        with self.assertRaisesRegex(closure.ContractError, "does not match terminal inventory binding"):
            closure.build_report(
                self.model,
                mode="normal",
                evaluation=stale,
                git=GIT,
                evaluation_digest="sha256:" + "e" * 64,
                run_url="https://example.invalid/actions/179",
                generated_at="2026-07-19T00:00:00+00:00",
            )


if __name__ == "__main__":
    unittest.main()
