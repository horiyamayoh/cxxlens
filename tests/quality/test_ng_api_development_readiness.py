#!/usr/bin/env python3
"""Positive and fail-closed tests for API development Wave 0 readiness."""

from __future__ import annotations

import copy
import json
import pathlib
import shutil
import sys
import tempfile
import unittest
from unittest import mock

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_api_development_readiness import (  # noqa: E402
    ReadinessError,
    build_report,
    load_document,
    public_callable_evidence,
    sha256,
    validate_documents,
    validate_schema,
)
import public_callable_inventory as callable_inventory  # noqa: E402
import check_ng_production_scope_closure as production_scope  # noqa: E402


REQUIRED_FILES = (
    ".github/workflows/quality.yml",
    ".github/ISSUE_TEMPLATE/design-feedback.yml",
    "AGENTS.md",
    "CMakeLists.txt",
    "docs/design/adr/0092-exact-public-callable-inventory.md",
    "docs/design/adr/0093-implementation-learning-design-feedback.md",
    "docs/design/adr/0094-risk-tiered-goal-authorization.md",
    "docs/design/adr/0095-production-scope-closure.md",
    "docs/design/adr/0096-clang22-installed-materialization-boundary.md",
    "docs/development/agent-api-development-goal.md",
    "schemas/cxxlens_ng_acceptance_manifest.yaml",
    "schemas/cxxlens_ng_api_development_readiness.schema.yaml",
    "schemas/cxxlens_ng_api_development_readiness_report.schema.yaml",
    "schemas/cxxlens_ng_api_development_readiness.yaml",
    "schemas/cxxlens_ng_clang22_materialization_contract.yaml",
    "schemas/cxxlens_ng_design_feedback_record.schema.yaml",
    "schemas/cxxlens_ng_g5_qualification.yaml",
    "schemas/cxxlens_ng_logical_query_contract.yaml",
    "schemas/cxxlens_ng_namespace_registry.yaml",
    "schemas/cxxlens_ng_production_scope_closure.schema.yaml",
    "schemas/cxxlens_ng_production_scope_closure.yaml",
    "schemas/cxxlens_ng_production_scope_closure_report.schema.yaml",
    "schemas/cxxlens_ng_provider_protocol.yaml",
    "schemas/cxxlens_ng_provider_conformance_vectors.yaml",
    "schemas/cxxlens_ng_provider_runtime_contract.yaml",
    "schemas/cxxlens_ng_provider_support_matrix.yaml",
    "schemas/cxxlens_ng_public_callable_inventory.schema.yaml",
    "schemas/cxxlens_ng_public_callable_inventory.yaml",
    "schemas/cxxlens_ng_public_callable_inventory_report.schema.yaml",
    "schemas/cxxlens_ng_public_api_catalog.yaml",
    "schemas/cxxlens_ng_relation_registry.yaml",
    "schemas/cxxlens_ng_release_bundle.yaml",
    "schemas/cxxlens_ng_release_qualification.yaml",
    "schemas/cxxlens_ng_release_qualification_report.schema.yaml",
    "schemas/cxxlens_ng_query_conformance_vectors.yaml",
    "schemas/cxxlens_ng_security_conformance_vectors.yaml",
    "schemas/cxxlens_ng_security_profile.yaml",
    "schemas/cxxlens_ng_quality_ownership.yaml",
    "tests/quality/test_ng_g5_qualification.py",
    "tests/quality/test_ng_clang22_materialization.py",
    "tests/CMakeLists.txt",
    "tests/quality/test_ng_foundation_completion.py",
    "tests/quality/test_ng_provider_protocol.py",
    "tests/quality/test_ng_provider_runtime.py",
    "tests/quality/test_ng_public_callable_inventory.py",
    "tests/quality/test_ng_query_contract.py",
    "tests/quality/test_ng_relation_contract.py",
    "tests/quality/test_ng_release_contract.py",
    "tests/quality/test_ng_release_qualification.py",
    "tests/quality/test_ng_sdk_contract.py",
    "tests/quality/test_ng_security_contract.py",
    "tests/quality/test_quality_ownership.py",
    "tools/quality/check_ng_production_scope_closure.py",
    "tools/quality/public_callable_inventory.py",
    "tools/quality/check_ng_migration_completion.py",
    "tools/quality/check_ng_design_feedback.py",
)


class NgApiDevelopmentReadinessTest(unittest.TestCase):
    CALLABLE_RUN_URL = "https://github.com/horiyamayoh/cxxlens/actions/runs/2"

    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = validate_documents(ROOT)

    def copied_root(self, temporary: str) -> pathlib.Path:
        root = pathlib.Path(temporary)
        for relative in REQUIRED_FILES:
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        shutil.copytree(
            ROOT / "docs/development/implementation-learning",
            root / "docs/development/implementation-learning",
        )
        shutil.copytree(ROOT / "include/cxxlens", root / "include/cxxlens")
        return root

    @staticmethod
    def write_yaml(path: pathlib.Path, document: dict) -> None:
        path.write_text(
            yaml.safe_dump(document, sort_keys=False, allow_unicode=True),
            encoding="utf-8",
        )

    @staticmethod
    def write_json(path: pathlib.Path, document: dict) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    def callable_report(
        self, evidence_dir: pathlib.Path, git_state: dict[str, object]
    ) -> pathlib.Path:
        inventory = load_document(
            ROOT / "schemas/cxxlens_ng_public_callable_inventory.yaml"
        )
        doxygen_digest = callable_inventory.semantic_digest(["doxygen-fixture"])
        review_path = (
            evidence_dir / "cxxlens-ng-public-callable-inventory-review.md"
        )
        review_path.write_text(
            callable_inventory.review_markdown(
                inventory,
                git_state,
                self.CALLABLE_RUN_URL,
                doxygen_digest,
            ),
            encoding="utf-8",
        )
        headers = callable_inventory.admitted_public_headers(ROOT)
        report = {
            "schema": "cxxlens.ng-public-callable-inventory-report.v1",
            "result": "passed",
            "generated_at": "2026-07-19T00:00:00Z",
            "run_url": self.CALLABLE_RUN_URL,
            "git": git_state,
            "inventory": {
                "path": "schemas/cxxlens_ng_public_callable_inventory.yaml",
                "file_digest": sha256(
                    ROOT / "schemas/cxxlens_ng_public_callable_inventory.yaml"
                ),
                "semantic_digest": inventory["inventory_digest"],
                "callable_count": len(inventory["callables"]),
            },
            "extractor": inventory["extractor"],
            "headers": {
                "count": len(headers),
                "digest": callable_inventory.semantic_digest(headers),
            },
            "doxygen": {
                "count": len(inventory["callables"]),
                "digest": doxygen_digest,
            },
            "review": {
                "path": review_path.name,
                "digest": sha256(review_path),
            },
        }
        report_path = (
            evidence_dir / "cxxlens-ng-public-callable-inventory-report.json"
        )
        self.write_json(report_path, report)
        return report_path

    def complete_evidence(
        self, evidence_dir: pathlib.Path, git_state: dict[str, object]
    ) -> pathlib.Path:
        self.write_json(
            evidence_dir / "toolchain-quality.json",
            {
                "source": {
                    "revision": git_state["revision"],
                    "tree": git_state["tree"],
                }
            },
        )
        self.write_json(evidence_dir / "quality.evidence.json", {"result": "passed"})
        self.write_json(
            evidence_dir / "static" / "install-artifact-manifest.json",
            {"configuration": "static"},
        )
        self.write_json(
            evidence_dir / "shared" / "install-artifact-manifest.json",
            {"configuration": "shared"},
        )
        scope_tests = production_scope.validate_repository(ROOT).evidence_tests
        cases = "".join(f'<testcase name="{name}"/>' for name in scope_tests)
        (evidence_dir / "ctest-quality.xml").write_text(
            f"<testsuite>{cases}</testsuite>\n", encoding="utf-8"
        )
        self.write_json(
            evidence_dir / "cxxlens-ng-foundation-completion-report.json",
            {"result": "passed", "git": git_state},
        )
        return self.callable_report(evidence_dir, git_state)

    def test_repository_is_wave0_ready(self) -> None:
        self.assertEqual(self.manifest["maturity"], "implemented")
        self.assertEqual(
            self.manifest["target_contract"]["provider_sdk_role"],
            "high-level-author-sdk",
        )

    def test_cmake_dependency_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            cmake = root / "CMakeLists.txt"
            cmake.write_text(
                cmake.read_text(encoding="utf-8").replace(
                    "target_link_libraries(cxxlens_provider_sdk PUBLIC cxxlens::cxxlens\n"
                    "                                                  cxxlens::recipes)",
                    "target_link_libraries(cxxlens_provider_sdk PUBLIC cxxlens::cxxlens)",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ReadinessError, "CMake target graph differs"):
                validate_documents(root)

    def test_catalog_unregistered_header_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            (root / "include/cxxlens/unregistered.hpp").write_text(
                "#pragma once\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(ReadinessError, "extra=.*unregistered"):
                validate_documents(root)

    def test_catalog_missing_header_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            (root / "include/cxxlens/cxxlens.hpp").unlink()
            with self.assertRaisesRegex(ReadinessError, "missing=.*cxxlens.hpp"):
                validate_documents(root)

    def test_catalog_relation_without_registry_binding_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            header = "include/cxxlens/relations/synthetic_unbound.hpp"
            (root / header).write_text("#pragma once\n", encoding="utf-8")
            catalog_path = root / "schemas/cxxlens_ng_public_api_catalog.yaml"
            catalog = load_document(catalog_path)
            catalog["packages"][0]["headers"].append(header)
            self.write_yaml(catalog_path, catalog)
            with self.assertRaisesRegex(ReadinessError, "lack registry binding"):
                validate_documents(root)

    def test_multiple_active_write_units_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            manifest_path = root / "schemas/cxxlens_ng_api_development_readiness.yaml"
            manifest = load_document(manifest_path)
            unit = {
                "issue": "#200",
                "contract_ids": ["synthetic.contract"],
                "write_paths": ["include/cxxlens/synthetic.hpp"],
                "completed_stages": [],
            }
            manifest["api_unit_workflow"]["active_write_units"] = [
                unit,
                {**copy.deepcopy(unit), "issue": "#201"},
            ]
            self.write_yaml(manifest_path, manifest)
            with self.assertRaisesRegex(ReadinessError, "schema validation failed"):
                validate_documents(root)

    def test_missing_implementation_learning_handbook_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            (root / "docs/development/implementation-learning/README.md").unlink()
            with self.assertRaisesRegex(ReadinessError, "asset is missing"):
                validate_documents(root)

    def test_implementation_learning_manifest_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            manifest_path = root / "schemas/cxxlens_ng_api_development_readiness.yaml"
            manifest = load_document(manifest_path)
            manifest["implementation_learning"]["checker"] = "tools/quality/other.py"
            self.write_yaml(manifest_path, manifest)
            with self.assertRaisesRegex(ReadinessError, "schema validation failed"):
                validate_documents(root)

    def test_authorization_policy_binding_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            agents = root / "AGENTS.md"
            agents.write_text(
                agents.read_text(encoding="utf-8").replace(
                    "CXXLENS_AGENT_AUTHORIZATION_V1", "REMOVED_POLICY_ID"
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ReadinessError, "must appear exactly once"):
                validate_documents(root)

    def test_authorization_policy_must_be_bound_in_short_goal_example(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / "docs/development/agent-api-development-goal.md"
            text = goal.read_text(encoding="utf-8").replace(
                "CXXLENS_AGENT_AUTHORIZATION_V1 を適用し",
                "policy を適用し",
            )
            goal.write_text(
                text + "\nCXXLENS_AGENT_AUTHORIZATION_V1\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ReadinessError, "short goal example"):
                validate_documents(root)

    def test_authorization_policy_suffix_is_not_an_exact_binding(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            agents = root / "AGENTS.md"
            agents.write_text(
                agents.read_text(encoding="utf-8").replace(
                    "CXXLENS_AGENT_AUTHORIZATION_V1",
                    "CXXLENS_AGENT_AUTHORIZATION_V10",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ReadinessError, "must appear exactly once"):
                validate_documents(root)

    def test_authorization_platform_carveout_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / "docs/development/agent-api-development-goal.md"
            goal.write_text(
                goal.read_text(encoding="utf-8").replace(
                    "`platform-approval: never-bypass`", "platform marker removed"
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ReadinessError, "platform-approval"):
                validate_documents(root)

    def test_authorization_standing_scope_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / "docs/development/agent-api-development-goal.md"
            goal.write_text(
                goal.read_text(encoding="utf-8").replace(
                    "`standing-scope: canonical-repository-active-unit`",
                    "standing scope marker removed",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ReadinessError, "standing-scope"):
                validate_documents(root)

    def test_authorization_fresh_approval_boundary_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / "docs/development/agent-api-development-goal.md"
            goal.write_text(
                goal.read_text(encoding="utf-8").replace(
                    "`fresh-approval: exact-target-effect-after-disclosure`",
                    "fresh approval marker removed",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ReadinessError, "fresh-approval"):
                validate_documents(root)

    def test_authorization_external_blocker_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / "docs/development/agent-api-development-goal.md"
            goal.write_text(
                goal.read_text(encoding="utf-8").replace(
                    "`external-blocker: evidence-options-stop`",
                    "external blocker marker removed",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ReadinessError, "external-blocker"):
                validate_documents(root)

    def test_authorization_ordinary_request_non_activation_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / "docs/development/agent-api-development-goal.md"
            goal.write_text(
                goal.read_text(encoding="utf-8").replace(
                    "`non-activation: ordinary-request`",
                    "non-activation marker removed",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ReadinessError, "non-activation"):
                validate_documents(root)

    def test_authorization_protected_main_workflow_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / "docs/development/agent-api-development-goal.md"
            goal.write_text(
                goal.read_text(encoding="utf-8").replace(
                    "`protected-main: unit-branch-pr-exact-head-review-merge-exact-merged-main`",
                    "protected main marker removed",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ReadinessError, "protected-main"):
                validate_documents(root)

    def test_authorization_direct_main_prohibition_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / "docs/development/agent-api-development-goal.md"
            goal.write_text(
                goal.read_text(encoding="utf-8").replace(
                    "`direct-main: prohibited`",
                    "direct main marker removed",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ReadinessError, "direct-main"):
                validate_documents(root)

    def test_authorization_fresh_approval_reuse_is_forbidden(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / "docs/development/agent-api-development-goal.md"
            goal.write_text(
                goal.read_text(encoding="utf-8").replace(
                    "`fresh-approval-reuse: forbidden`",
                    "fresh approval reuse marker removed",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ReadinessError, "fresh-approval-reuse"):
                validate_documents(root)

    def test_legacy_direct_main_workflow_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / "docs/development/agent-api-development-goal.md"
            goal.write_text(
                goal.read_text(encoding="utf-8")
                + "\n1つの GitHub issue を完了するごとに、対象差分だけを commit し、"
                "`main` へ push する。\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ReadinessError, "direct-main"):
                validate_documents(root)

    def test_gate_owner_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            acceptance_path = root / "schemas/cxxlens_ng_acceptance_manifest.yaml"
            acceptance = load_document(acceptance_path)
            next(
                row for row in acceptance["entries"] if row["id"] == "gate.g5"
            )["owner_issue"] = "#999"
            self.write_yaml(acceptance_path, acceptance)
            with self.assertRaisesRegex(ReadinessError, "G5 gate owner differs"):
                validate_documents(root)

    def test_release_bundle_production_scope_binding_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            bundle_path = root / "schemas/cxxlens_ng_release_bundle.yaml"
            bundle = load_document(bundle_path)
            bundle["production_scope_closure"]["report"]["ci_job"] = (
                "synthetic-terminal"
            )
            self.write_yaml(bundle_path, bundle)
            with self.assertRaisesRegex(
                ReadinessError, "Release Bundle production-scope binding differs"
            ):
                validate_documents(root)

    def test_release_evaluation_requires_sqlite_qualification(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/quality.yml"
            workflow.write_text(
                workflow.read_text(encoding="utf-8").replace(
                    "needs: [g5-qualification, sqlite-store-v3-qualification]",
                    "needs: [g5-qualification]",
                    1,
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                ReadinessError, "production-scope job needs differ: release-evaluation"
            ):
                validate_documents(root)

    def test_production_scope_dependency_matrix_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/quality.yml"
            workflow.write_text(
                workflow.read_text(encoding="utf-8").replace(
                    '"${STRICT_RESULT}" == "skipped"',
                    '"${STRICT_RESULT}" == "success"',
                    1,
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                ReadinessError, "dependency matrix marker is missing"
            ):
                validate_documents(root)

    def test_production_scope_report_conditions_cannot_be_reversed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/quality.yml"
            text = workflow.read_text(encoding="utf-8")
            normal = (
                "if: needs.release-evaluation.outputs.qualification == "
                "'not-qualified'"
            )
            final = (
                "if: needs.release-evaluation.outputs.qualification == 'qualified'"
            )
            normal_header = (
                "      - name: Generate classified production-scope report\n"
                f"        {normal}\n"
            )
            final_header = (
                "      - name: Generate final production-scope report\n"
                f"        {final}\n"
            )
            self.assertIn(normal_header, text)
            self.assertIn(final_header, text)
            text = text.replace(
                normal_header,
                "      - name: Generate classified production-scope report\n"
                f"        {final}\n",
                1,
            )
            text = text.replace(
                final_header,
                "      - name: Generate final production-scope report\n"
                f"        {normal}\n",
                1,
            )
            text = text.replace(
                "        run: |\n"
                "          python tools/quality/check_ng_production_scope_closure.py",
                "        run: |\n"
                f"          # {normal}\n"
                "          python tools/quality/check_ng_production_scope_closure.py",
                1,
            )
            final_step = "      - name: Generate final production-scope report\n"
            text = text.replace(
                final_step,
                final_step + f"        # {final}\n",
                1,
            )
            workflow.write_text(text, encoding="utf-8")
            with self.assertRaisesRegex(
                ReadinessError, "normal production-scope routing"
            ):
                validate_documents(root)

    def test_production_scope_modes_cannot_be_reversed_with_comments(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/quality.yml"
            text = workflow.read_text(encoding="utf-8")
            text = text.replace("--mode normal \\", "--mode synthetic \\", 1)
            text = text.replace("--mode final \\", "--mode normal \\", 1)
            text = text.replace("--mode synthetic \\", "--mode final \\", 1)
            text = text.replace(
                "        run: |\n"
                "          python tools/quality/check_ng_production_scope_closure.py",
                "        run: |\n"
                "          # --mode normal\n"
                "          python tools/quality/check_ng_production_scope_closure.py",
                1,
            )
            final_step = "      - name: Generate final production-scope report\n"
            text = text.replace(
                final_step,
                final_step + "        # --mode final\n",
                1,
            )
            workflow.write_text(text, encoding="utf-8")
            with self.assertRaisesRegex(
                ReadinessError, "normal production-scope routing command argv differ"
            ):
                validate_documents(root)

    def test_final_scope_command_requires_its_own_gr_argument(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/quality.yml"
            text = workflow.read_text(encoding="utf-8")
            final_gr = (
                "            --gr build/production-scope-input/gr/"
                "cxxlens-ng-release-qualification-report.json \\\n"
            )
            self.assertIn(final_gr, text)
            workflow.write_text(text.replace(final_gr, "", 1), encoding="utf-8")
            with self.assertRaisesRegex(
                ReadinessError, "final production-scope routing command argv differ"
            ):
                validate_documents(root)

    def test_strict_gr_command_requires_evaluation_argument(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/quality.yml"
            text = workflow.read_text(encoding="utf-8")
            evaluation_argument = (
                '            --evaluation "build/release-qualification-input/'
                'cxxlens-ng-release-qualification-evaluation-${GITHUB_SHA}/'
                'cxxlens-ng-release-qualification-evaluation-report.json" \\\n'
            )
            self.assertIn(evaluation_argument, text)
            text = text.replace(
                evaluation_argument,
                "            # --evaluation intentionally omitted\n",
                1,
            )
            workflow.write_text(text, encoding="utf-8")
            with self.assertRaisesRegex(
                ReadinessError, "strict release qualification command argv differ"
            ):
                validate_documents(root)

    def test_qualification_dependency_pairs_cannot_be_swapped(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/quality.yml"
            text = workflow.read_text(encoding="utf-8")
            not_qualified = (
                '"${QUALIFICATION}" == "not-qualified" && '
                '"${STRICT_RESULT}" == "skipped"'
            )
            qualified = (
                '"${QUALIFICATION}" == "qualified" && '
                '"${STRICT_RESULT}" == "success"'
            )
            text = text.replace(not_qualified, "synthetic-pair", 1)
            text = text.replace(qualified, not_qualified, 1)
            text = text.replace("synthetic-pair", qualified, 1)
            workflow.write_text(text, encoding="utf-8")
            with self.assertRaisesRegex(
                ReadinessError, "qualification dependency matrix script differs"
            ):
                validate_documents(root)

    def test_terminal_evaluation_artifact_download_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/quality.yml"
            text = workflow.read_text(encoding="utf-8")
            download = (
                "      - uses: actions/download-artifact@"
                "fa0a91b85d4f404e444e00e005971372dc801d16  # v4.1.8\n"
                "        with:\n"
                "          name: cxxlens-ng-release-qualification-evaluation-"
                "${{ github.sha }}\n"
                "          path: build/production-scope-input/evaluation\n"
            )
            self.assertIn(download, text)
            workflow.write_text(text.replace(download, "", 1), encoding="utf-8")
            with self.assertRaisesRegex(
                ReadinessError, "terminal evaluation artifact download"
            ):
                validate_documents(root)

    def test_terminal_artifact_download_must_precede_report_steps(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/quality.yml"
            text = workflow.read_text(encoding="utf-8")
            download = (
                "      - uses: actions/download-artifact@"
                "fa0a91b85d4f404e444e00e005971372dc801d16  # v4.1.8\n"
                "        with:\n"
                "          name: cxxlens-ng-release-qualification-evaluation-"
                "${{ github.sha }}\n"
                "          path: build/production-scope-input/evaluation\n"
            )
            upload = (
                "      - uses: actions/upload-artifact@"
                "ea165f8d65b6e75b540449e92b4886f43607fa02  # v4.6.2\n"
                "        with:\n"
                "          name: cxxlens-ng-production-scope-closure-"
                "${{ github.sha }}\n"
            )
            self.assertIn(download, text)
            self.assertIn(upload, text)
            text = text.replace(download, "", 1)
            text = text.replace(upload, download + upload, 1)
            workflow.write_text(text, encoding="utf-8")
            with self.assertRaisesRegex(
                ReadinessError, "terminal production-scope workflow step order differs"
            ):
                validate_documents(root)

    def test_terminal_artifacts_cannot_be_mutated_by_extra_steps(self) -> None:
        evaluation_path = (
            "build/production-scope-input/evaluation/"
            "cxxlens-ng-release-qualification-evaluation-report.json"
        )
        mutations = (
            (
                "          path: build/production-scope-input/evaluation\n",
                "          path: build/production-scope-input/evaluation\n"
                f'      - run: "printf x >> {evaluation_path}"\n',
            ),
            (
                "      - uses: actions/upload-artifact@"
                "ea165f8d65b6e75b540449e92b4886f43607fa02  # v4.6.2\n"
                "        with:\n"
                "          name: cxxlens-ng-production-scope-closure-"
                "${{ github.sha }}\n",
                "      - run: \"printf x >> "
                "${RUNNER_TEMP}/cxxlens-ng-production-scope-closure-report.json\"\n"
                "      - uses: actions/upload-artifact@"
                "ea165f8d65b6e75b540449e92b4886f43607fa02  # v4.6.2\n"
                "        with:\n"
                "          name: cxxlens-ng-production-scope-closure-"
                "${{ github.sha }}\n",
            ),
        )
        for marker, replacement in mutations:
            with self.subTest(marker=marker):
                with tempfile.TemporaryDirectory() as temporary:
                    root = self.copied_root(temporary)
                    workflow = root / ".github/workflows/quality.yml"
                    text = workflow.read_text(encoding="utf-8")
                    self.assertIn(marker, text)
                    workflow.write_text(
                        text.replace(marker, replacement, 1), encoding="utf-8"
                    )
                    with self.assertRaisesRegex(
                        ReadinessError,
                        "terminal production-scope workflow step order differs",
                    ):
                        validate_documents(root)

    def test_terminal_job_runner_and_shape_are_fixed(self) -> None:
        mutations = (
            ("runs-on: ubuntu-24.04", "runs-on: windows-latest", "runner differs"),
            (
                "    runs-on: ubuntu-24.04\n    steps:\n",
                "    runs-on: ubuntu-24.04\n"
                "    defaults:\n"
                "      run:\n"
                "        shell: python\n"
                "    steps:\n",
                "job keys differ",
            ),
        )
        for marker, replacement, error in mutations:
            with self.subTest(error=error):
                with tempfile.TemporaryDirectory() as temporary:
                    root = self.copied_root(temporary)
                    workflow = root / ".github/workflows/quality.yml"
                    text = workflow.read_text(encoding="utf-8")
                    terminal = text.index("  production-scope-closure:\n")
                    prefix = text[:terminal]
                    body = text[terminal:]
                    self.assertIn(marker, body)
                    workflow.write_text(
                        prefix + body.replace(marker, replacement, 1),
                        encoding="utf-8",
                    )
                    with self.assertRaisesRegex(ReadinessError, error):
                        validate_documents(root)

    def test_tail_jobs_cannot_inherit_uncontrolled_global_execution(self) -> None:
        mutations = (
            (
                "env:\n  CMAKE_GENERATOR: Ninja\n",
                "env:\n  BASH_ENV: synthetic-hook\n  CMAKE_GENERATOR: Ninja\n",
                "global environment differs",
            ),
            (
                "jobs:\n",
                "defaults:\n  run:\n    shell: python\n\njobs:\n",
                "global run defaults",
            ),
        )
        for marker, replacement, error in mutations:
            with self.subTest(error=error):
                with tempfile.TemporaryDirectory() as temporary:
                    root = self.copied_root(temporary)
                    workflow = root / ".github/workflows/quality.yml"
                    text = workflow.read_text(encoding="utf-8")
                    self.assertIn(marker, text)
                    workflow.write_text(
                        text.replace(marker, replacement, 1), encoding="utf-8"
                    )
                    with self.assertRaisesRegex(ReadinessError, error):
                        validate_documents(root)

    def test_quality_workflow_requires_unrestricted_push_trigger(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            workflow = root / ".github/workflows/quality.yml"
            text = workflow.read_text(encoding="utf-8")
            self.assertIn("  push:\n", text)
            workflow.write_text(text.replace("  push:\n", "", 1), encoding="utf-8")
            with self.assertRaisesRegex(ReadinessError, "workflow triggers"):
                validate_documents(root)

    def test_callable_inventory_semantic_digest_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            inventory_path = (
                root / "schemas/cxxlens_ng_public_callable_inventory.yaml"
            )
            inventory = load_document(inventory_path)
            inventory["inventory_digest"] = "sha256:" + "0" * 64
            self.write_yaml(inventory_path, inventory)
            with self.assertRaisesRegex(ReadinessError, "semantic digest differs"):
                validate_documents(root)

    def test_public_callable_evidence_is_bound_into_report(self) -> None:
        git_state = {
            "revision": "1" * 40,
            "tree": "2" * 40,
            "branch": "main",
            "clean": True,
        }
        with tempfile.TemporaryDirectory() as temporary:
            evidence_dir = pathlib.Path(temporary)
            self.complete_evidence(evidence_dir, git_state)
            required_jobs = [
                *self.manifest["required_status_checks"]["contexts"],
                "foundation-completion",
            ]
            with mock.patch(
                "check_ng_api_development_readiness.current_git_state",
                return_value=git_state,
            ):
                report = build_report(
                    ROOT,
                    self.manifest,
                    evidence_dir,
                    "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
                    required_jobs,
                    "2026-07-19T00:00:00Z",
                    git_state["revision"],
                )
            validate_schema(
                report,
                load_document(
                    ROOT
                    / "schemas/cxxlens_ng_api_development_readiness_report.schema.yaml"
                ),
                "API development readiness report test fixture",
            )
            binding = report["public_callable_inventory"]
            scope_binding = report["production_scope_inventory"]
            inventory = load_document(
                ROOT / "schemas/cxxlens_ng_public_callable_inventory.yaml"
            )
            self.assertEqual(binding["result"], "passed")
            self.assertEqual(binding["revision"], git_state["revision"])
            self.assertEqual(binding["tree"], git_state["tree"])
            self.assertEqual(binding["callable_count"], len(inventory["callables"]))
            self.assertEqual(binding["doxygen_count"], binding["callable_count"])
            self.assertEqual(
                scope_binding["manifest_path"],
                "schemas/cxxlens_ng_production_scope_closure.yaml",
            )
            self.assertEqual(scope_binding["summary"]["domain_count"], 30)
            self.assertEqual(scope_binding["closure_status"], "classified-with-gaps")
            authority_paths = {row["path"] for row in report["authorities"]}
            learning_paths = {
                row["path"] for row in report["implementation_learning_assets"]
            }
            self.assertIn(
                "docs/design/adr/0093-implementation-learning-design-feedback.md",
                authority_paths,
            )
            self.assertIn(
                "schemas/cxxlens_ng_design_feedback_record.schema.yaml",
                authority_paths,
            )
            self.assertIn("AGENTS.md", authority_paths)
            self.assertIn(
                "docs/development/agent-api-development-goal.md",
                authority_paths,
            )
            self.assertIn(
                "docs/design/adr/0094-risk-tiered-goal-authorization.md",
                authority_paths,
            )
            self.assertIn(
                "docs/design/adr/0095-production-scope-closure.md",
                authority_paths,
            )
            self.assertIn(
                "schemas/cxxlens_ng_production_scope_closure.yaml",
                authority_paths,
            )
            self.assertIn(
                "docs/development/implementation-learning/mental-models/authority-and-learning-loop.md",
                learning_paths,
            )
            self.assertIn(
                "docs/development/implementation-learning/records/README.md",
                learning_paths,
            )
            self.assertTrue(authority_paths.isdisjoint(learning_paths))

            mixed = copy.deepcopy(report)
            mixed["authorities"].append(
                next(
                    row
                    for row in mixed["implementation_learning_assets"]
                    if "/mental-models/" in row["path"]
                )
            )
            with self.assertRaisesRegex(ReadinessError, "schema validation failed"):
                validate_schema(
                    mixed,
                    load_document(
                        ROOT
                        / "schemas/cxxlens_ng_api_development_readiness_report.schema.yaml"
                    ),
                    "mixed authority readiness report",
                )

    def test_wave0_junit_must_contain_every_typed_scope_evidence_test(self) -> None:
        git_state = {
            "revision": "1" * 40,
            "tree": "2" * 40,
            "branch": "main",
            "clean": True,
        }
        with tempfile.TemporaryDirectory() as temporary:
            evidence_dir = pathlib.Path(temporary)
            self.complete_evidence(evidence_dir, git_state)
            required = production_scope.validate_repository(ROOT).evidence_tests
            junit = evidence_dir / "ctest-quality.xml"
            text = junit.read_text(encoding="utf-8")
            text = text.replace(f'<testcase name="{required[0]}"/>', "", 1)
            junit.write_text(text, encoding="utf-8")
            required_jobs = [
                *self.manifest["required_status_checks"]["contexts"],
                "foundation-completion",
            ]
            with mock.patch(
                "check_ng_api_development_readiness.current_git_state",
                return_value=git_state,
            ), self.assertRaisesRegex(
                ReadinessError, "JUnit omits production-scope evidence tests"
            ):
                build_report(
                    ROOT,
                    self.manifest,
                    evidence_dir,
                    "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
                    required_jobs,
                    "2026-07-19T00:00:00Z",
                    git_state["revision"],
                )

    def test_duplicate_public_callable_evidence_pair_is_rejected(self) -> None:
        git_state = {
            "revision": "1" * 40,
            "tree": "2" * 40,
            "branch": "main",
            "clean": True,
        }
        with tempfile.TemporaryDirectory() as temporary:
            evidence_dir = pathlib.Path(temporary)
            report_path = self.callable_report(evidence_dir, git_state)
            duplicate = evidence_dir / "duplicate"
            duplicate.mkdir()
            shutil.copy2(report_path, duplicate / report_path.name)
            shutil.copy2(
                evidence_dir / "cxxlens-ng-public-callable-inventory-review.md",
                duplicate / "cxxlens-ng-public-callable-inventory-review.md",
            )
            with self.assertRaisesRegex(ReadinessError, "exactly one"):
                public_callable_evidence(
                    ROOT, self.manifest, evidence_dir, git_state
                )

    def test_public_callable_report_schema_drift_is_rejected(self) -> None:
        git_state = {
            "revision": "1" * 40,
            "tree": "2" * 40,
            "branch": "main",
            "clean": True,
        }
        with tempfile.TemporaryDirectory() as temporary:
            evidence_dir = pathlib.Path(temporary)
            report_path = self.callable_report(evidence_dir, git_state)
            report = load_document(report_path)
            del report["doxygen"]
            self.write_json(report_path, report)
            with self.assertRaisesRegex(ReadinessError, "schema validation failed"):
                public_callable_evidence(
                    ROOT, self.manifest, evidence_dir, git_state
                )

    def test_public_callable_review_digest_drift_is_rejected(self) -> None:
        git_state = {
            "revision": "1" * 40,
            "tree": "2" * 40,
            "branch": "main",
            "clean": True,
        }
        with tempfile.TemporaryDirectory() as temporary:
            evidence_dir = pathlib.Path(temporary)
            self.callable_report(evidence_dir, git_state)
            review = evidence_dir / "cxxlens-ng-public-callable-inventory-review.md"
            review.write_text(
                review.read_text(encoding="utf-8") + "manual edit\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ReadinessError, "Markdown digest differs"):
                public_callable_evidence(
                    ROOT, self.manifest, evidence_dir, git_state
                )
            report_path = (
                evidence_dir / "cxxlens-ng-public-callable-inventory-report.json"
            )
            report = load_document(report_path)
            report["review"]["digest"] = sha256(review)
            self.write_json(report_path, report)
            with self.assertRaisesRegex(ReadinessError, "current inventory"):
                public_callable_evidence(
                    ROOT, self.manifest, evidence_dir, git_state
                )

    def test_public_callable_git_and_count_drift_are_rejected(self) -> None:
        git_state = {
            "revision": "1" * 40,
            "tree": "2" * 40,
            "branch": "main",
            "clean": True,
        }
        with tempfile.TemporaryDirectory() as temporary:
            evidence_dir = pathlib.Path(temporary)
            report_path = self.callable_report(evidence_dir, git_state)
            report = load_document(report_path)
            report["git"]["revision"] = "3" * 40
            self.write_json(report_path, report)
            with self.assertRaisesRegex(ReadinessError, "git source"):
                public_callable_evidence(
                    ROOT, self.manifest, evidence_dir, git_state
                )

            self.callable_report(evidence_dir, git_state)
            report = load_document(report_path)
            report["inventory"]["file_digest"] = "sha256:" + "0" * 64
            self.write_json(report_path, report)
            with self.assertRaisesRegex(ReadinessError, "inventory binding differs"):
                public_callable_evidence(
                    ROOT, self.manifest, evidence_dir, git_state
                )

            self.callable_report(evidence_dir, git_state)
            report = load_document(report_path)
            report["doxygen"]["count"] += 1
            self.write_json(report_path, report)
            with self.assertRaisesRegex(ReadinessError, "Doxygen count differs"):
                public_callable_evidence(
                    ROOT, self.manifest, evidence_dir, git_state
                )

    def test_report_requires_exact_ci_job_set(self) -> None:
        git_state = {
            "revision": "1" * 40,
            "tree": "2" * 40,
            "branch": "main",
            "clean": True,
        }
        with tempfile.TemporaryDirectory() as temporary:
            with mock.patch(
                "check_ng_api_development_readiness.current_git_state",
                return_value=git_state,
            ):
                with self.assertRaisesRegex(ReadinessError, "CI job evidence differs"):
                    build_report(
                        ROOT,
                        self.manifest,
                        pathlib.Path(temporary),
                        "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
                        ["build-test (OFF)"],
                        "2026-07-18T00:00:00Z",
                        git_state["revision"],
                    )


if __name__ == "__main__":
    unittest.main()
