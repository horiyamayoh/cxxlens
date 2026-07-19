#!/usr/bin/env python3
"""Fail-closed tests for distribution 1.0 GR qualification."""

from __future__ import annotations

import copy
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_release_qualification as release  # noqa: E402
import check_ng_production_scope_closure as scope  # noqa: E402


class NgReleaseQualificationTests(unittest.TestCase):
    def scope_inventory(self, closure_status: str) -> dict:
        gaps = closure_status != "qualified"
        return {
            "manifest_path": "schemas/cxxlens_ng_production_scope_closure.yaml",
            "manifest_digest": "sha256:" + "1" * 64,
            "authority_census_digest": "sha256:" + "2" * 64,
            "evidence_census_digest": "sha256:" + "4" * 64,
            "classification_digest": "sha256:" + "3" * 64,
            "evidence_tests": ["quality.ownership"],
            "summary": {
                "domain_count": 30,
                "assignable_count": 2,
                "expanded_count": 2,
                "aggregate_count": 14,
                "qualified": 1 if gaps else 2,
                "tracked_gap": 1 if gaps else 0,
                "blocked": 0,
                "not_applicable": 0,
            },
            "closure_status": closure_status,
        }

    def evaluation_evidence(
        self, directory: pathlib.Path, closure_status: str
    ) -> dict:
        paths = {}
        for name in ("foundation", "readiness", "g5", "security"):
            path = directory / f"{name}.json"
            path.write_text(f"{name}\n", encoding="utf-8")
            paths[name] = path
        install_values = {}
        for configuration, digit in (("static", "a"), ("shared", "b")):
            value_digest = "sha256:" + digit * 64
            install_values[configuration] = {
                "manifest_digest": value_digest,
                "prefix_digest": value_digest,
                "toolchain": {"identity": "clang version 22.1.0"},
                "files": [
                    {
                        "path": "bin/cxxlens-clang-worker-22",
                        "digest": value_digest,
                    }
                ],
            }
        return {
            "git": {
                "revision": "1" * 40,
                "tree": "2" * 40,
                "branch": "main",
                "clean": True,
            },
            "packages": [],
            "install_values": install_values,
            "foundation_path": paths["foundation"],
            "readiness_path": paths["readiness"],
            "callable_evidence": {
                "report": {"digest": "sha256:" + "c" * 64}
            },
            "scope_inventory": self.scope_inventory(closure_status),
            "g5_path": paths["g5"],
            "g5": {},
            "security_path": paths["security"],
            "security": {},
        }

    def make_callable_evidence(
        self, evidence: pathlib.Path
    ) -> tuple[dict, dict, dict, pathlib.Path, pathlib.Path]:
        manifest = release.load(ROOT / release.MANIFEST)
        inventory_path = ROOT / release.CALLABLE_INVENTORY
        inventory = release.load(inventory_path)
        git = {
            "revision": "1" * 40,
            "tree": "2" * 40,
            "branch": "main",
            "clean": True,
        }
        run_url = "https://github.com/horiyamayoh/cxxlens/actions/runs/1"
        doxygen_digest = "sha256:" + "4" * 64
        review_path = (
            evidence / manifest["documentation"]["public_callable_review_filename"]
        )
        review_path.write_text(
            release.callable_inventory.review_markdown(
                inventory, git, run_url, doxygen_digest
            ),
            encoding="utf-8",
        )
        inventory_binding = {
            "path": release.CALLABLE_INVENTORY.as_posix(),
            "file_digest": release.digest(inventory_path),
            "semantic_digest": release.callable_inventory.inventory_digest(inventory),
            "callable_count": len(inventory["callables"]),
        }
        report = {
            "schema": "cxxlens.ng-public-callable-inventory-report.v1",
            "result": "passed",
            "generated_at": "2026-07-19T00:00:00Z",
            "run_url": run_url,
            "git": git,
            "inventory": inventory_binding,
            "extractor": inventory["extractor"],
            "headers": {"count": 1, "digest": "sha256:" + "3" * 64},
            "doxygen": {
                "count": len(inventory["callables"]),
                "digest": doxygen_digest,
            },
            "review": {
                "path": review_path.name,
                "digest": release.digest(review_path),
            },
        }
        report_path = (
            evidence / manifest["documentation"]["public_callable_report_filename"]
        )
        report_path.write_text(
            json.dumps(report, ensure_ascii=False, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        readiness = {
            "public_callable_inventory": {
                "path": report_path.name,
                "report_digest": release.digest(report_path),
                "inventory_file_digest": inventory_binding["file_digest"],
                "inventory_semantic_digest": inventory_binding["semantic_digest"],
                "review_path": review_path.name,
                "review_digest": report["review"]["digest"],
                "callable_count": inventory_binding["callable_count"],
                "doxygen_count": report["doxygen"]["count"],
                "result": report["result"],
                "revision": git["revision"],
                "tree": git["tree"],
            }
        }
        return manifest, readiness, git, report_path, review_path

    def test_repository_contract_is_implemented(self) -> None:
        manifest = release.validate_documents(ROOT)
        self.assertEqual(manifest["binding"]["package_version"], "1.0.0")
        self.assertEqual(manifest["prerequisites"]["gates"], [f"gate.g{i}" for i in range(6)])
        self.assertEqual(
            manifest["prerequisites"]["public_callable_evidence"],
            "cxxlens.ng-public-callable-inventory-report.v1",
        )
        self.assertTrue((ROOT / release.EVALUATION_REPORT_SCHEMA).is_file())

    def test_release_schemas_require_static_and_shared_exactly_once(self) -> None:
        evaluation_schema = release.load(ROOT / release.EVALUATION_REPORT_SCHEMA)
        install_schema = copy.deepcopy(
            evaluation_schema["properties"]["evidence"]["properties"][
                "install_manifests"
            ]
        )
        install_schema["$defs"] = copy.deepcopy(evaluation_schema["$defs"])
        install_manifests = [
            {
                "configuration": configuration,
                "manifest_digest": "sha256:" + digit * 64,
                "prefix_digest": "sha256:" + digit * 64,
            }
            for configuration, digit in (("static", "1"), ("shared", "2"))
        ]
        release.validate_schema(
            install_manifests, install_schema, "evaluation install matrix"
        )
        duplicate_static = copy.deepcopy(install_manifests)
        duplicate_static[1]["configuration"] = "static"
        with self.assertRaisesRegex(
            release.ReleaseQualificationError, "schema validation failed"
        ):
            release.validate_schema(
                duplicate_static, install_schema, "evaluation install matrix"
            )

        report_schema = release.load(ROOT / release.REPORT_SCHEMA)
        packages_schema = copy.deepcopy(report_schema["properties"]["packages"])
        packages_schema["$defs"] = copy.deepcopy(report_schema["$defs"])
        packages = [
            {
                "configuration": configuration,
                "prefix_digest": "sha256:" + digit * 64,
                "manifest_digest": "sha256:" + digit * 64,
                "toolchain_digest": "sha256:" + digit * 64,
                "real_project": "passed",
                "storage_backends": ["memory", "sqlite"],
                "relocated": True,
                "license": "sha256:" + digit * 64,
                "notice": "sha256:" + digit * 64,
            }
            for configuration, digit in (("static", "3"), ("shared", "4"))
        ]
        release.validate_schema(packages, packages_schema, "strict package matrix")
        duplicate_shared = copy.deepcopy(packages)
        duplicate_shared[0]["configuration"] = "shared"
        with self.assertRaisesRegex(
            release.ReleaseQualificationError, "schema validation failed"
        ):
            release.validate_schema(
                duplicate_shared, packages_schema, "strict package matrix"
            )

    def test_wave0_scope_inventory_binding_is_exact(self) -> None:
        current = self.scope_inventory("classified-with-gaps")
        with mock.patch.object(
            release, "production_scope_inventory_binding", return_value=current
        ):
            self.assertEqual(
                release.verify_production_scope_inventory(
                    ROOT,
                    {
                        "production_scope_inventory": current,
                        "test_inventory": {
                            "production_scope_tests": current["evidence_tests"]
                        },
                    },
                ),
                current,
            )
            changed = copy.deepcopy(current)
            changed["classification_digest"] = "sha256:" + "f" * 64
            with self.assertRaisesRegex(
                release.ReleaseQualificationError,
                "Wave 0 production scope inventory binding differs",
            ):
                release.verify_production_scope_inventory(
                    ROOT,
                    {
                        "production_scope_inventory": changed,
                        "test_inventory": {
                            "production_scope_tests": current["evidence_tests"]
                        },
                    },
                )

    def test_scope_contract_error_is_wrapped_as_release_error(self) -> None:
        with mock.patch.object(
            scope,
            "inventory_binding",
            side_effect=scope.ContractError("synthetic scope violation"),
        ):
            with self.assertRaisesRegex(
                release.ReleaseQualificationError,
                "production scope inventory evaluation failed: "
                "synthetic scope violation",
            ):
                release.production_scope_inventory_binding(ROOT)

    def test_not_qualified_evaluation_has_no_production_tuple(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = self.evaluation_evidence(
                pathlib.Path(temporary), "classified-with-gaps"
            )
            with mock.patch.object(
                release, "collect_release_evidence", return_value=evidence
            ), mock.patch.object(
                release, "production_support_tuples"
            ) as production_support:
                report = release.make_evaluation_report(
                    ROOT,
                    release.load(ROOT / release.MANIFEST),
                    pathlib.Path(temporary),
                    evidence["security_path"],
                    "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
                    "1" * 40,
                    "2026-07-19T00:00:00Z",
                )
            production_support.assert_not_called()
            self.assertEqual(report["qualification"], "not-qualified")
            self.assertFalse(report["qualified"])
            self.assertEqual(report["production_support"], [])
            self.assertFalse(report["strict_report"]["eligible"])
            self.assertFalse(report["strict_report"]["emitted"])

    def test_qualified_evaluation_exposes_boolean_without_authority_tuples(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = self.evaluation_evidence(pathlib.Path(temporary), "qualified")
            with mock.patch.object(
                release, "collect_release_evidence", return_value=evidence
            ):
                report = release.make_evaluation_report(
                    ROOT,
                    release.load(ROOT / release.MANIFEST),
                    pathlib.Path(temporary),
                    evidence["security_path"],
                    "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
                    "1" * 40,
                    "2026-07-19T00:00:00Z",
                )
            self.assertEqual(report["qualification"], "qualified")
            self.assertTrue(report["qualified"])
            self.assertEqual(report["production_support"], [])
            self.assertTrue(report["strict_report"]["eligible"])
            self.assertFalse(report["strict_report"]["emitted"])

    def test_strict_evaluation_binding_revalidates_exact_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            evidence = self.evaluation_evidence(directory, "qualified")
            with mock.patch.object(
                release, "collect_release_evidence", return_value=evidence
            ):
                evaluation = release.make_evaluation_report(
                    ROOT,
                    release.load(ROOT / release.MANIFEST),
                    directory,
                    evidence["security_path"],
                    "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
                    "1" * 40,
                    "2026-07-19T00:00:00Z",
                )
            evaluation_path = directory / "evaluation.json"
            evaluation_path.write_text(
                json.dumps(evaluation, sort_keys=True) + "\n", encoding="utf-8"
            )
            self.assertEqual(
                release.verify_qualified_evaluation(
                    ROOT, evaluation_path, evidence
                ),
                evaluation,
            )
            changed = copy.deepcopy(evidence)
            changed["scope_inventory"] = self.scope_inventory(
                "classified-with-gaps"
            )
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "scope binding differs"
            ):
                release.verify_qualified_evaluation(
                    ROOT, evaluation_path, changed
                )

    def test_terminal_rejects_gr_bound_to_different_evaluation_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            evaluation_path = directory / "evaluation.json"
            gr_path = directory / "gr.json"
            evaluation_path.write_text("evaluation artifact\n", encoding="utf-8")
            gr_path.write_text("GR artifact\n", encoding="utf-8")
            git = {
                "revision": "1" * 40,
                "tree": "2" * 40,
                "branch": "main",
                "clean": True,
            }
            evaluation = {
                "git": git,
                "qualification": "qualified",
                "qualified": True,
            }
            gr = {
                "git": git,
                "prerequisites": {
                    "release_evaluation_report_digest": release.digest(
                        evaluation_path
                    )
                },
            }

            def load_document(path: pathlib.Path) -> dict:
                if path == evaluation_path:
                    return evaluation
                if path == gr_path:
                    return gr
                return {}

            with mock.patch.object(
                release, "load", side_effect=load_document
            ), mock.patch.object(release, "validate_schema"):
                self.assertEqual(
                    release.verify_gr_evaluation_artifact_binding(
                        ROOT, evaluation_path, gr_path
                    ),
                    release.digest(evaluation_path),
                )
                gr["prerequisites"]["release_evaluation_report_digest"] = (
                    "sha256:" + "0" * 64
                )
                with self.assertRaisesRegex(
                    release.ReleaseQualificationError,
                    "artifact digest differs",
                ):
                    release.verify_gr_evaluation_artifact_binding(
                        ROOT, evaluation_path, gr_path
                    )

    def test_strict_report_persists_exact_evaluation_artifact_digest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            evaluation_path = directory / "evaluation.json"
            evaluation_path.write_text("exact evaluation artifact\n", encoding="utf-8")
            evidence = self.evaluation_evidence(directory, "qualified")
            evidence["g5"] = {
                "performance": {
                    "metrics_us": {
                        "warm_zero_plan_median": 1,
                        "bounded_closure_median": 2,
                    }
                }
            }
            evidence["security"] = {
                "status": "green",
                "contract_digest": "sha256:" + "a" * 64,
                "vector_count": 1,
            }
            evidence["callable_evidence"].update(
                {
                    "doxygen": {"count": 1, "digest": "sha256:" + "b" * 64},
                    "inventory": {},
                    "review": {},
                }
            )
            with mock.patch.object(
                release, "collect_release_evidence", return_value=evidence
            ), mock.patch.object(
                release, "verify_qualified_evaluation"
            ), mock.patch.object(
                release, "production_support_tuples", return_value=[]
            ), mock.patch.object(release, "validate_schema"):
                report = release.make_report(
                    ROOT,
                    release.load(ROOT / release.MANIFEST),
                    directory,
                    evidence["security_path"],
                    "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
                    "1" * 40,
                    "2026-07-19T00:00:00Z",
                    evaluation_path,
                )
            self.assertEqual(
                report["prerequisites"]["release_evaluation_report_digest"],
                release.digest(evaluation_path),
            )

    def test_strict_report_rejects_scope_gaps_before_tuple_derivation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = self.evaluation_evidence(
                pathlib.Path(temporary), "classified-with-gaps"
            )
            with mock.patch.object(
                release, "collect_release_evidence", return_value=evidence
            ), mock.patch.object(
                release, "production_support_tuples"
            ) as production_support:
                with self.assertRaisesRegex(
                    release.ReleaseQualificationError,
                    "strict GR report requires production scope",
                ):
                    release.make_report(
                        ROOT,
                        release.load(ROOT / release.MANIFEST),
                        pathlib.Path(temporary),
                        evidence["security_path"],
                        "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
                        "1" * 40,
                        "2026-07-19T00:00:00Z",
                    )
            production_support.assert_not_called()

    def test_evaluate_cli_writes_machine_readable_qualified_signal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            output = root / "evaluation.json"
            github_output = root / "github-output.txt"
            github_output.write_text("prior=value\n", encoding="utf-8")
            evaluation = {"qualified": False, "qualification": "not-qualified"}
            arguments = [
                "check_ng_release_qualification.py",
                "evaluate",
                "--root",
                str(ROOT),
                "--evidence-dir",
                str(root),
                "--security-report",
                str(root / "security.json"),
                "--output",
                str(output),
                "--github-output",
                str(github_output),
                "--run-url",
                "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
                "--expected-revision",
                "1" * 40,
                "--generated-at",
                "2026-07-19T00:00:00Z",
            ]
            with mock.patch.object(sys, "argv", arguments), mock.patch.object(
                release, "validate_documents", return_value={}
            ), mock.patch.object(
                release, "make_evaluation_report", return_value=evaluation
            ), mock.patch.object(release, "make_report") as strict_report, mock.patch(
                "builtins.print"
            ) as printed:
                self.assertEqual(release.main(), 0)
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), evaluation)
            self.assertEqual(
                github_output.read_text(encoding="utf-8"),
                "prior=value\nqualification=not-qualified\n",
            )
            strict_report.assert_not_called()
            printed.assert_called_once_with("qualification=not-qualified")

    def test_pending_static_tuple_cannot_grant_production(self) -> None:
        original = release.load
        support = original(ROOT / release.SUPPORT)
        changed = copy.deepcopy(support)
        changed["entries"][0]["status"] = "production-supported"

        def load(path: pathlib.Path):
            return changed if path == ROOT / release.SUPPORT else original(path)

        with mock.patch.object(release, "load", side_effect=load):
            with self.assertRaisesRegex(release.ReleaseQualificationError, "static support matrix"):
                release.validate_documents(ROOT)

    def test_g5_cannot_be_bypassed(self) -> None:
        original = release.load
        acceptance = original(ROOT / release.ACCEPTANCE)
        changed = copy.deepcopy(acceptance)
        next(row for row in changed["entries"] if row["id"] == "gate.g5")["status"] = "deferred"

        def load(path: pathlib.Path):
            return changed if path == ROOT / release.ACCEPTANCE else original(path)

        with mock.patch.object(release, "load", side_effect=load):
            with self.assertRaisesRegex(release.ReleaseQualificationError, "G0-G5"):
                release.validate_documents(ROOT)

    def test_install_manifest_digest_is_fail_closed(self) -> None:
        self.assertNotEqual(
            release.canonical_digest({"files": []}),
            release.canonical_digest({"files": [{"path": "forged"}]}),
        )

    def test_install_junit_is_resolved_from_downloaded_artifact_root(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            artifact = evidence / "cxxlens-install-static-revision"
            (artifact / "tests" / "install-consumer").mkdir(parents=True)
            junit = artifact / "ctest-install-OFF.xml"
            junit.touch()

            self.assertEqual(release.find_one(evidence, "ctest-install-OFF.xml"), junit)

    def test_public_callable_json_and_markdown_are_exactly_readiness_bound(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, readiness, git, _, _ = self.make_callable_evidence(evidence)
            value = release.verify_public_callable_evidence(
                ROOT, manifest, evidence, git, readiness
            )
            self.assertEqual(
                value["report"]["digest"],
                readiness["public_callable_inventory"]["report_digest"],
            )
            self.assertEqual(
                value["doxygen"]["count"], value["inventory"]["callable_count"]
            )

    def test_public_callable_json_digest_must_match_readiness(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, readiness, git, _, _ = self.make_callable_evidence(evidence)
            readiness["public_callable_inventory"]["report_digest"] = (
                "sha256:" + "0" * 64
            )
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "exact JSON artifact digest"
            ):
                release.verify_public_callable_evidence(
                    ROOT, manifest, evidence, git, readiness
                )

    def test_public_callable_doxygen_count_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, readiness, git, report_path, _ = self.make_callable_evidence(
                evidence
            )
            report = release.load(report_path)
            report["doxygen"]["count"] -= 1
            report_path.write_text(
                json.dumps(report, ensure_ascii=False, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "Doxygen count"
            ):
                release.verify_public_callable_evidence(
                    ROOT, manifest, evidence, git, readiness
                )

    def test_public_callable_markdown_digest_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, readiness, git, _, review_path = self.make_callable_evidence(
                evidence
            )
            review_path.write_text(
                review_path.read_text(encoding="utf-8") + "forged review\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "Markdown review digest"
            ):
                release.verify_public_callable_evidence(
                    ROOT, manifest, evidence, git, readiness
                )

    def test_duplicate_public_callable_artifact_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, readiness, git, report_path, _ = self.make_callable_evidence(
                evidence
            )
            duplicate = evidence / "duplicate" / report_path.name
            duplicate.parent.mkdir()
            duplicate.write_bytes(report_path.read_bytes())
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "expected exactly one"
            ):
                release.verify_public_callable_evidence(
                    ROOT, manifest, evidence, git, readiness
                )


if __name__ == "__main__":
    unittest.main()
