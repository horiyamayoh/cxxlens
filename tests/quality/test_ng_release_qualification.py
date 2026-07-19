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
        materialization_reports = {}
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
                    },
                    {
                        "path": "bin/cxxlens-clang22-materialize",
                        "digest": value_digest,
                    },
                ],
            }
            for backend in ("memory", "sqlite"):
                report_path = directory / f"{configuration}-{backend}-materialization.json"
                report_path.write_text(
                    f"{configuration} {backend} materialization\n", encoding="utf-8"
                )
                materialization_reports[(configuration, backend)] = {
                    "path": report_path,
                    "digest": release.digest(report_path),
                }
        materialization_report_sets = {
            configuration: release.materialization_report_set_digest(
                materialization_reports, configuration
            )
            for configuration in ("static", "shared")
        }
        return {
            "root": ROOT,
            "git": {
                "revision": "1" * 40,
                "tree": "2" * 40,
                "branch": "main",
                "clean": True,
            },
            "packages": [],
            "install_values": install_values,
            "materialization_reports": materialization_reports,
            "materialization_report_sets": materialization_report_sets,
            "materialization_evidence": {
                "state": "exact-matrix",
                "request_count": 4,
                "report_count": 4,
                "report_set_count": 2,
                "owner_issue": None,
                "feedback": [],
            },
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

    def make_materialization_matrix(
        self, evidence: pathlib.Path
    ) -> tuple[dict, dict, dict, dict]:
        manifest = release.load(ROOT / release.MANIFEST)
        git = {
            "revision": "1" * 40,
            "tree": "2" * 40,
            "branch": "main",
            "clean": True,
        }
        install_values = {}
        written = {}
        for configuration, digit in (("static", "a"), ("shared", "b")):
            manifest_digest = "sha256:" + digit * 64
            prefix_digest = "sha256:" + ("c" if configuration == "static" else "d") * 64
            tool_digest = "sha256:" + ("e" if configuration == "static" else "f") * 64
            worker_digest = "sha256:" + ("1" if configuration == "static" else "2") * 64
            install_values[configuration] = {
                "manifest_digest": manifest_digest,
                "prefix_digest": prefix_digest,
                "files": [
                    {
                        "path": "bin/cxxlens-clang22-materialize",
                        "digest": tool_digest,
                    },
                    {
                        "path": "bin/cxxlens-clang-worker-22",
                        "digest": worker_digest,
                    },
                ],
            }
            for backend in ("memory", "sqlite"):
                request = release.materialization.sample_request(
                    ROOT,
                    configuration=configuration,
                    backend=backend,
                    translation_unit_count=2,
                )
                request["tool"].update(
                    {
                        "source_revision": git["revision"],
                        "source_tree": git["tree"],
                        "installed_executable_digest": tool_digest,
                        "prefix_manifest_digest": manifest_digest,
                        "relocated_prefix_digest": prefix_digest,
                    }
                )
                request["worker"]["installed_binary_digest"] = worker_digest
                release.materialization.bind_request_identity(request)
                report = release.materialization.sample_report(ROOT, request)
                report["installation"]["platform"] = (
                    f"linux-{release.platform.machine().lower()}-{configuration}"
                )
                directory = evidence / configuration / backend
                directory.mkdir(parents=True)
                request_path = directory / manifest["materialization"][
                    "request_filename"
                ]
                request_path.write_text(
                    json.dumps(request, ensure_ascii=False, sort_keys=True) + "\n",
                    encoding="utf-8",
                )
                path = directory / manifest["materialization"]["report_filename"]
                path.write_text(
                    json.dumps(report, ensure_ascii=False, sort_keys=True) + "\n",
                    encoding="utf-8",
                )
                written[(configuration, backend)] = path
        return manifest, install_values, git, written

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

        report_matrix_schema = copy.deepcopy(
            evaluation_schema["properties"]["evidence"]["properties"][
                "materialization_reports"
            ]
        )
        report_matrix_schema["$defs"] = copy.deepcopy(evaluation_schema["$defs"])
        materialization_reports = [
            {
                "configuration": configuration,
                "backend": backend,
                "report_digest": "sha256:" + digit * 64,
            }
            for configuration, digit in (("static", "5"), ("shared", "6"))
            for backend in ("memory", "sqlite")
        ]
        release.validate_schema(
            materialization_reports,
            report_matrix_schema,
            "evaluation materialization matrix",
        )
        duplicate_combination = copy.deepcopy(materialization_reports)
        duplicate_combination[3].update(
            {"configuration": "static", "backend": "memory"}
        )
        with self.assertRaisesRegex(
            release.ReleaseQualificationError, "schema validation failed"
        ):
            release.validate_schema(
                duplicate_combination,
                report_matrix_schema,
                "evaluation materialization matrix",
            )

        report_set_schema = copy.deepcopy(
            evaluation_schema["properties"]["evidence"]["properties"][
                "materialization_report_sets"
            ]
        )
        report_set_schema["$defs"] = copy.deepcopy(evaluation_schema["$defs"])
        materialization_report_sets = [
            {
                "configuration": configuration,
                "report_set_digest": "sha256:" + digit * 64,
            }
            for configuration, digit in (("static", "7"), ("shared", "8"))
        ]
        release.validate_schema(
            materialization_report_sets,
            report_set_schema,
            "evaluation materialization report sets",
        )
        duplicate_report_set = copy.deepcopy(materialization_report_sets)
        duplicate_report_set[1]["configuration"] = "static"
        with self.assertRaisesRegex(
            release.ReleaseQualificationError, "schema validation failed"
        ):
            release.validate_schema(
                duplicate_report_set,
                report_set_schema,
                "evaluation materialization report sets",
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
                "materialization_report_set_digest": "sha256:" + digit * 64,
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

    def test_materialization_report_matrix_is_exact_install_bound_and_parity_checked(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, install_values, git, written = self.make_materialization_matrix(
                evidence
            )
            reordered_path = written[("static", "sqlite")]
            reordered = release.load(reordered_path)
            reordered["task_results"].reverse()
            reordered_path.write_text(
                json.dumps(reordered, sort_keys=True) + "\n", encoding="utf-8"
            )
            reports, report_sets = release.verify_materialization_reports(
                ROOT, manifest, evidence, install_values, git
            )
            self.assertEqual(set(reports), set(release.MATERIALIZATION_MATRIX))
            self.assertEqual(set(report_sets), {"static", "shared"})
            self.assertEqual(len(set(report_sets.values())), 2)
            for configuration in ("static", "shared"):
                self.assertEqual(
                    report_sets[configuration],
                    release.materialization_report_set_digest(
                        reports, configuration
                    ),
                )
            static_report = reports[("static", "memory")]["value"]
            shared_report = reports[("shared", "memory")]["value"]
            self.assertNotEqual(
                {
                    row["provider_execution_id"]
                    for row in static_report["task_results"]
                },
                {
                    row["provider_execution_id"]
                    for row in shared_report["task_results"]
                },
            )
            self.assertEqual(
                static_report["base_claims"], shared_report["base_claims"]
            )
            for report in (static_report, shared_report):
                self.assertEqual(len(report["task_results"]), 2)
                self.assertEqual(
                    len(
                        {
                            row["provider_task_id"]
                            for row in report["task_results"]
                        }
                    ),
                    1,
                )
                self.assertEqual(
                    len(
                        {
                            release.materialization_task_execution_key(row)
                            for row in report["task_results"]
                        }
                    ),
                    2,
                )

    def test_release_rejects_catalog_local_selection_census_drift(self) -> None:
        mutations = (
            (
                "selected-census",
                lambda report: report["task_results"][0].update(
                    {
                        "selected_catalog_compile_unit_id": "catalog-unit:unselected"
                    }
                ),
                "selected catalog compile-unit census differs",
            ),
            (
                "catalog-census-digest",
                lambda report: report["project"].update(
                    {
                        "catalog_compile_unit_census_digest": "semantic-v2:sha256:"
                        + "0" * 64
                    }
                ),
                "report catalog-local compile-unit census differs",
            ),
        )
        for label, mutate, message in mutations:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                evidence = pathlib.Path(temporary)
                manifest, install_values, git, written = (
                    self.make_materialization_matrix(evidence)
                )
                path = written[("static", "memory")]
                report = release.load(path)
                mutate(report)
                path.write_text(
                    json.dumps(report, sort_keys=True) + "\n", encoding="utf-8"
                )
                with self.assertRaisesRegex(
                    release.ReleaseQualificationError, message
                ):
                    release.verify_materialization_reports(
                        ROOT, manifest, evidence, install_values, git
                    )

    def test_release_rejects_missing_extra_or_duplicate_composite_execution(
        self,
    ) -> None:
        def duplicate_execution(report: dict) -> None:
            duplicate = copy.deepcopy(report["task_results"][0])
            duplicate["side_channel_digest"] = "sha256:" + "0" * 64
            report["task_results"].append(duplicate)

        def missing_execution(report: dict) -> None:
            if len(report["task_results"]) > 1:
                report["task_results"].pop()
            else:
                report["task_results"][0]["provider_execution_id"] = (
                    "provider-execution:missing-expected-extra-observed"
                )

        def extra_execution(report: dict) -> None:
            extra = copy.deepcopy(report["task_results"][0])
            extra["provider_execution_id"] = "provider-execution:extra"
            report["task_results"].append(extra)

        for label, mutate in (
            ("duplicate", duplicate_execution),
            ("missing", missing_execution),
            ("extra", extra_execution),
        ):
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                evidence = pathlib.Path(temporary)
                manifest, install_values, git, written = (
                    self.make_materialization_matrix(evidence)
                )
                path = written[("shared", "sqlite")]
                report = release.load(path)
                mutate(report)
                path.write_text(
                    json.dumps(report, sort_keys=True) + "\n", encoding="utf-8"
                )
                with self.assertRaisesRegex(
                    release.ReleaseQualificationError,
                    "composite task/input/execution result census differs",
                ):
                    release.verify_materialization_reports(
                        ROOT, manifest, evidence, install_values, git
                    )

    def test_materialization_assignment_transition_is_exact_and_scope_independent(
        self,
    ) -> None:
        key = scope.SurfaceKey(
            "distribution.installed-tool", "cxxlens-clang22-materialize"
        )
        scope_manifest = release.load(
            ROOT / "schemas/cxxlens_ng_production_scope_closure.yaml"
        )
        current_assignment = next(
            assignment
            for assignment in scope_manifest["assignments"]
            if assignment["id"] == release.MATERIALIZATION_ASSIGNMENT_ID
        )
        self.assertEqual(
            current_assignment,
            release.materialization_assignment_shape("tracked-gap"),
        )
        current_model = mock.Mock(assignments={key: current_assignment})
        with mock.patch.object(
            scope, "validate_repository", return_value=current_model
        ):
            current = release.materialization_assignment_transition(ROOT)
        self.assertEqual(current["assignment_state"], "tracked-gap")
        self.assertEqual(
            current["materialization_evidence"],
            {
                "state": "tracked-gap-empty",
                "request_count": 0,
                "report_count": 0,
                "report_set_count": 0,
                "owner_issue": "#181",
                "feedback": ["DF-0182", "DF-0187"],
            },
        )

        qualified_model = mock.Mock(
            assignments={key: release.materialization_assignment_shape("qualified")},
            closure_status="classified-with-gaps",
        )
        with mock.patch.object(
            scope, "validate_repository", return_value=qualified_model
        ):
            qualified = release.materialization_assignment_transition(ROOT)
        self.assertEqual(qualified["assignment_state"], "qualified")
        self.assertEqual(
            qualified["materialization_evidence"]["state"], "exact-matrix"
        )

        mutations = (
            ("owner", lambda row: row.update({"owner_issue": "#999"})),
            ("feedback", lambda row: row.update({"feedback": ["DF-0174"]})),
            ("scope", lambda row: row.update({"scope": "unresolved"})),
        )
        for label, mutate in mutations:
            with self.subTest(label=label):
                assignment = release.materialization_assignment_shape("tracked-gap")
                mutate(assignment)
                model = mock.Mock(assignments={key: assignment})
                with mock.patch.object(
                    scope, "validate_repository", return_value=model
                ), self.assertRaisesRegex(
                    release.ReleaseQualificationError,
                    "neither the exact #181/DF-0182/DF-0187 tracked gap",
                ):
                    release.materialization_assignment_transition(ROOT)

    def test_tracked_gap_collect_requires_zero_and_exempts_only_tool_binary(
        self,
    ) -> None:
        manifest = release.load(ROOT / release.MANIFEST)
        transition = {
            "assignment_state": "tracked-gap",
            "materialization_evidence": {
                "state": "tracked-gap-empty",
                "request_count": 0,
                "report_count": 0,
                "report_set_count": 0,
                "owner_issue": "#181",
                "feedback": ["DF-0182", "DF-0187"],
            },
        }
        required = release.materialization_required_install_files(
            manifest, transition
        )
        self.assertNotIn(release.MATERIALIZATION_TOOL_FILE, required)
        self.assertEqual(
            set(manifest["package"]["required_files"]) - set(required),
            {release.MATERIALIZATION_TOOL_FILE},
        )
        schema_files = {
            path
            for path in manifest["package"]["required_files"]
            if path.startswith("share/cxxlens/schemas/")
        }
        self.assertTrue(schema_files)
        self.assertTrue(schema_files.issubset(required))
        self.assertEqual(
            release.materialization_required_install_files(
                manifest, {"assignment_state": "qualified"}
            ),
            manifest["package"]["required_files"],
        )

        git = {
            "revision": "1" * 40,
            "tree": "2" * 40,
            "branch": "main",
            "clean": True,
        }
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            reports, report_sets, binding = release.collect_materialization_evidence(
                ROOT, manifest, evidence, {}, git, transition
            )
            self.assertEqual(reports, {})
            self.assertEqual(report_sets, {})
            self.assertEqual(binding, transition["materialization_evidence"])

            request_path = evidence / manifest["materialization"][
                "request_filename"
            ]
            request_path.write_text("{}\n", encoding="utf-8")
            with self.assertRaisesRegex(
                release.ReleaseQualificationError,
                "requires zero materialization requests and reports",
            ):
                release.collect_materialization_evidence(
                    ROOT, manifest, evidence, {}, git, transition
                )

    def test_qualified_materializer_with_unrelated_gaps_requires_exact_matrix(
        self,
    ) -> None:
        key = scope.SurfaceKey(
            "distribution.installed-tool", "cxxlens-clang22-materialize"
        )
        qualified_model = mock.Mock(
            assignments={key: release.materialization_assignment_shape("qualified")},
            closure_status="classified-with-gaps",
        )
        with mock.patch.object(
            scope, "validate_repository", return_value=qualified_model
        ):
            transition = release.materialization_assignment_transition(ROOT)

        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            manifest, install_values, git, written = self.make_materialization_matrix(
                directory
            )
            reports, report_sets, binding = release.collect_materialization_evidence(
                ROOT,
                manifest,
                directory,
                install_values,
                git,
                transition,
            )
            self.assertEqual(set(reports), set(release.MATERIALIZATION_MATRIX))
            self.assertEqual(set(report_sets), {"static", "shared"})
            self.assertEqual(binding["state"], "exact-matrix")

            partial = written[("shared", "sqlite")]
            partial.unlink()
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "exactly four"
            ):
                release.collect_materialization_evidence(
                    ROOT,
                    manifest,
                    directory,
                    install_values,
                    git,
                    transition,
                )

            evidence = self.evaluation_evidence(
                directory, "classified-with-gaps"
            )
            evidence["materialization_reports"] = reports
            evidence["materialization_report_sets"] = report_sets
            evidence["materialization_evidence"] = binding
            with mock.patch.object(
                release, "collect_release_evidence", return_value=evidence
            ):
                evaluation = release.make_evaluation_report(
                    ROOT,
                    manifest,
                    directory,
                    evidence["security_path"],
                    "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
                    "1" * 40,
                    "2026-07-19T00:00:00Z",
                )
            self.assertEqual(evaluation["qualification"], "not-qualified")
            self.assertEqual(
                evaluation["evidence"]["materialization_evidence"]["state"],
                "exact-matrix",
            )
            self.assertEqual(
                len(evaluation["evidence"]["materialization_reports"]), 4
            )
            self.assertEqual(
                len(evaluation["evidence"]["materialization_report_sets"]), 2
            )

    def test_materialization_report_matrix_rejects_missing_and_duplicate_combination(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, install_values, git, written = self.make_materialization_matrix(
                evidence
            )
            written[("shared", "sqlite")].unlink()
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "exactly four"
            ):
                release.verify_materialization_reports(
                    ROOT, manifest, evidence, install_values, git
                )

        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, install_values, git, written = self.make_materialization_matrix(
                evidence
            )
            path = written[("static", "sqlite")]
            report = release.load(path)
            report["publication"]["backend"] = "memory"
            report["publication"]["sqlite_reopened"] = False
            path.write_text(json.dumps(report, sort_keys=True) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "duplicate materialization matrix"
            ):
                release.verify_materialization_reports(
                    ROOT, manifest, evidence, install_values, git
                )

    def test_materialization_report_rejects_source_authority_and_install_digest_drift(
        self,
    ) -> None:
        mutations = (
            (
                "source",
                lambda report: report["source"].update({"revision": "0" * 40}),
                "exact source",
            ),
            (
                "authority",
                lambda report: report["authority_digests"][0].update(
                    {"digest": "sha256:" + "0" * 64}
                ),
                "authority digest",
            ),
            (
                "manifest",
                lambda report: report["installation"].update(
                    {"prefix_manifest_digest": "sha256:" + "0" * 64}
                ),
                "install/prefix/tool/worker",
            ),
            (
                "tool",
                lambda report: report["installation"].update(
                    {"tool_digest": "sha256:" + "0" * 64}
                ),
                "install/prefix/tool/worker",
            ),
            (
                "worker",
                lambda report: report["installation"].update(
                    {"worker_digest": "sha256:" + "0" * 64}
                ),
                "install/prefix/tool/worker",
            ),
        )
        for label, mutate, message in mutations:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                evidence = pathlib.Path(temporary)
                manifest, install_values, git, written = (
                    self.make_materialization_matrix(evidence)
                )
                path = written[("static", "memory")]
                report = release.load(path)
                mutate(report)
                path.write_text(
                    json.dumps(report, sort_keys=True) + "\n", encoding="utf-8"
                )
                with self.assertRaisesRegex(
                    release.ReleaseQualificationError, message
                ):
                    release.verify_materialization_reports(
                        ROOT, manifest, evidence, install_values, git
                    )

    def test_materialization_request_and_report_bytes_use_strict_json_policy(
        self,
    ) -> None:
        cases = (
            ("request-bom", "request", lambda raw: b"\xef\xbb\xbf" + raw),
            ("report-invalid-utf8", "report", lambda raw: raw + b"\xff"),
            (
                "request-nested-duplicate",
                "request",
                lambda raw: raw.replace(
                    b'"tool": {',
                    b'"tool": {"executable": "forged-duplicate", ',
                    1,
                ),
            ),
            (
                "report-nonfinite",
                "report",
                lambda raw: raw.replace(
                    b'"committed_transaction_count": 1',
                    b'"committed_transaction_count": NaN',
                    1,
                ),
            ),
            ("report-second-json", "report", lambda raw: raw + b"\n{}\n"),
            ("request-trailing-bytes", "request", lambda raw: raw + b" trailing"),
            ("request-non-object", "request", lambda _raw: b"[]\n"),
        )
        for label, artifact, mutate in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                evidence = pathlib.Path(temporary)
                manifest, install_values, git, written = (
                    self.make_materialization_matrix(evidence)
                )
                report_path = written[("shared", "memory")]
                path = (
                    report_path.with_name(
                        manifest["materialization"]["request_filename"]
                    )
                    if artifact == "request"
                    else report_path
                )
                original = path.read_bytes()
                mutated = mutate(original)
                self.assertNotEqual(mutated, original)
                path.write_bytes(mutated)
                with self.assertRaisesRegex(
                    release.ReleaseQualificationError,
                    "strict materialization JSON",
                ):
                    release.verify_materialization_reports(
                        ROOT, manifest, evidence, install_values, git
                    )

    def test_strict_release_closes_span_observation_and_unresolved_censuses(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, install_values, git, written = self.make_materialization_matrix(
                evidence
            )
            path = written[("static", "memory")]
            report = release.load(path)
            report["span_validation"]["observed_bundle_count"] = 0
            path.write_text(
                json.dumps(report, sort_keys=True) + "\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "observed plus absent"
            ):
                release.verify_materialization_reports(
                    ROOT, manifest, evidence, install_values, git
                )

        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, install_values, git, written = self.make_materialization_matrix(
                evidence
            )
            path = written[("shared", "sqlite")]
            report = release.load(path)
            report["span_validation"].update(
                {
                    "absent_bundle_count": 1,
                    "call_absent_bundle_count": 1,
                    "absent_bundle_unresolved_count": 1,
                    "source_dependent_canonical_omission_count": 1,
                }
            )
            call_descriptor = "frontend.clang22.call_observation.v2"
            next(
                batch
                for batch in report["task_results"][0]["batches"]
                if batch["descriptor_id"] == call_descriptor
            )["row_count"] += 1
            next(
                stage
                for stage in report["claim_stages"]
                if stage["descriptor_id"] == call_descriptor
            )["claim_count"] += 1
            report["side_channels"]["coverage"]["state_counts"].update(
                {"covered": 2, "unresolved": 1}
            )
            report["side_channels"]["unresolved"].update(
                {
                    "record_count": 1,
                    "categories": ["missing_input"],
                    "category_counts": {"missing_input": 1},
                }
            )
            report["side_channels"]["guarantee"]["approximation"] = (
                "under_approximation"
            )
            path.write_text(
                json.dumps(report, sort_keys=True) + "\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(
                release.ReleaseQualificationError,
                "lack exact typed unresolved category accounting",
            ):
                release.verify_materialization_reports(
                    ROOT, manifest, evidence, install_values, git
                )

    def test_strict_release_binds_six_base_claim_census_and_matrix_parity(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, install_values, git, written = self.make_materialization_matrix(
                evidence
            )
            path = written[("static", "memory")]
            report = release.load(path)
            project = next(
                row
                for row in report["base_claims"]["descriptor_results"]
                if row["descriptor_id"] == "build.project.v1"
            )
            project["row_count"] += 1
            project["claim_count"] += 1
            report["base_claims"]["total_row_count"] += 1
            report["base_claims"]["total_claim_count"] += 1
            path.write_text(
                json.dumps(report, sort_keys=True) + "\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(
                release.ReleaseQualificationError,
                "base-claim descriptor census differs",
            ):
                release.verify_materialization_reports(
                    ROOT, manifest, evidence, install_values, git
                )

        parity_mutations = (
            (
                "per-descriptor-set",
                lambda base: base["descriptor_results"][0].update(
                    {"row_set_digest": "sha256:" + "0" * 64}
                ),
            ),
            (
                "total-base-set",
                lambda base: base.update(
                    {"claim_set_digest": "sha256:" + "0" * 64}
                ),
            ),
        )
        for label, mutate in parity_mutations:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                evidence = pathlib.Path(temporary)
                manifest, install_values, git, written = (
                    self.make_materialization_matrix(evidence)
                )
                path = written[("shared", "sqlite")]
                report = release.load(path)
                mutate(report["base_claims"])
                path.write_text(
                    json.dumps(report, sort_keys=True) + "\n", encoding="utf-8"
                )
                with mock.patch.object(
                    release.materialization, "validate_report"
                ), self.assertRaisesRegex(
                    release.ReleaseQualificationError,
                    "base-claim count/set-digest parity differs",
                ):
                    release.verify_materialization_reports(
                        ROOT, manifest, evidence, install_values, git
                    )

    def test_release_accepts_canonical_base_dedup_across_catalog_local_ids(
        self,
    ) -> None:
        request = release.materialization.sample_request(
            ROOT,
            translation_unit_count=2,
        )
        second_catalog_id = request["project"]["catalog_compile_units"][1][
            "catalog_compile_unit_id"
        ]
        first_entry = request["project"]["catalog_compile_units"][0]
        request["project"]["catalog_compile_units"][1].update(
            {
                key: first_entry[key]
                for key in (
                    "effective_invocation_digest",
                    "source_digest",
                    "environment_digest",
                )
            }
        )
        release.materialization.bind_project_catalog_identity(request["project"])
        relations = release.materialization.base_registry_relations(ROOT)
        project_row = {
            "catalog": request["project"]["catalog_id"],
            "catalog_digest": request["project"]["catalog_digest"],
            "logical_root": request["project"]["logical_root"],
            "environment_digest": request["project"][
                "catalog_environment_digest"
            ],
        }
        project_id = release.materialization.derive_base_row_identity(
            relations["build.project.v1"], project_row
        )
        request["project"]["project_id"] = project_id

        request["tasks"][1] = copy.deepcopy(request["tasks"][0])
        request["tasks"][1]["selected_catalog_compile_unit_id"] = second_catalog_id
        for task in request["tasks"]:
            task["project_id"] = project_id
            task["catalog_id"] = request["project"]["catalog_id"]
            task["catalog_digest"] = request["project"]["catalog_digest"]
            variant_row = {
                "project": project_id,
                "toolchain": task["toolchain_context_id"],
                **task["variant"],
            }
            task["build_variant_id"] = (
                release.materialization.derive_base_row_identity(
                    relations["build.variant.v1"], variant_row
                )
            )
            source = task["source"]
            source_row = {
                "file": source["file_id"],
                "project": project_id,
                "logical_path": source["logical_path"],
                "content": source["content_digest"],
                "size": source["size_bytes"],
                "encoding": source["encoding"],
                "line_index": source["line_index_id"],
                "read_only": source["read_only"],
            }
            source["source_snapshot_id"] = (
                release.materialization.derive_base_row_identity(
                    relations["source.file.v1"], source_row
                )
            )
            compile_unit_row = {
                "project": project_id,
                "main_source": source["source_snapshot_id"],
                "variant": task["build_variant_id"],
                "toolchain": task["toolchain_context_id"],
                "effective_invocation_digest": task[
                    "normalized_invocation_digest"
                ],
                "language": task["language"],
                "working_directory": task["working_directory"],
            }
            task["compile_unit_id"] = (
                release.materialization.derive_base_row_identity(
                    relations["build.compile_unit.v1"], compile_unit_row
                )
            )
        release.materialization.bind_provider_task_identities(request)
        release.materialization.bind_task_execution_identities(request)
        release.materialization.bind_request_identity(request)

        report = release.materialization.sample_report(ROOT, request)
        release.materialization.validate_request(ROOT, request)
        release.materialization.validate_report(ROOT, request, report)
        release.validate_release_base_claims(request, report)
        compile_units = next(
            row
            for row in report["base_claims"]["descriptor_results"]
            if row["descriptor_id"] == "build.compile_unit.v1"
        )
        self.assertEqual(compile_units["row_count"], 1)
        self.assertEqual(len(request["tasks"]), 2)
        self.assertEqual(
            len({task["compile_unit_id"] for task in request["tasks"]}),
            1,
        )

    def test_materialization_report_rejects_reopen_request_and_semantic_parity_drift(
        self,
    ) -> None:
        mutations = (
            (
                "sqlite-reopen",
                ("static", "sqlite"),
                lambda report: report["publication"].update(
                    {"sqlite_reopened": False}
                ),
                "schema validation failed",
            ),
            (
                "semantic-output",
                ("shared", "sqlite"),
                lambda report: report["semantic_verification"].update(
                    {"query_digest": "sha256:" + "0" * 64}
                ),
                "semantic parity differs",
            ),
        )
        for label, key, mutate, message in mutations:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                evidence = pathlib.Path(temporary)
                manifest, install_values, git, written = (
                    self.make_materialization_matrix(evidence)
                )
                path = written[key]
                report = release.load(path)
                mutate(report)
                path.write_text(
                    json.dumps(report, sort_keys=True) + "\n", encoding="utf-8"
                )
                with self.assertRaisesRegex(
                    release.ReleaseQualificationError, message
                ):
                    release.verify_materialization_reports(
                        ROOT, manifest, evidence, install_values, git
                    )

        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, install_values, git, written = self.make_materialization_matrix(
                evidence
            )
            report_path = written[("static", "sqlite")]
            request_path = report_path.with_name(
                manifest["materialization"]["request_filename"]
            )
            request = release.load(request_path)
            request["tasks"][0]["budget"]["rows"] += 1
            release.materialization.bind_task_execution_identities(request)
            release.materialization.bind_request_identity(request)
            request_path.write_text(
                json.dumps(request, sort_keys=True) + "\n", encoding="utf-8"
            )
            report = release.materialization.sample_report(ROOT, request)
            report["installation"]["platform"] = (
                f"linux-{release.platform.machine().lower()}-static"
            )
            report_path.write_text(
                json.dumps(report, sort_keys=True) + "\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(
                release.ReleaseQualificationError,
                "base-claim count/set-digest parity differs",
            ):
                release.verify_materialization_reports(
                    ROOT, manifest, evidence, install_values, git
                )

    def test_materialization_request_pair_is_required_and_recomputed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, install_values, git, written = self.make_materialization_matrix(
                evidence
            )
            written[("static", "memory")].with_name(
                manifest["materialization"]["request_filename"]
            ).unlink()
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "exactly four co-located"
            ):
                release.verify_materialization_reports(
                    ROOT, manifest, evidence, install_values, git
                )

        with tempfile.TemporaryDirectory() as temporary:
            evidence = pathlib.Path(temporary)
            manifest, install_values, git, written = self.make_materialization_matrix(
                evidence
            )
            request_path = written[("shared", "sqlite")].with_name(
                manifest["materialization"]["request_filename"]
            )
            request = release.load(request_path)
            request["tasks"][0]["budget"]["rows"] += 1
            request_path.write_text(
                json.dumps(request, sort_keys=True) + "\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "request binding is invalid"
            ):
                release.verify_materialization_reports(
                    ROOT, manifest, evidence, install_values, git
                )

    def test_materialization_report_set_digest_changes_production_tuple_evidence(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = self.evaluation_evidence(pathlib.Path(temporary), "qualified")
            manifest = release.load(ROOT / release.MANIFEST)
            baseline = release.production_support_tuples(ROOT, manifest, evidence)
            changed = copy.deepcopy(evidence)
            changed["materialization_reports"][("static", "memory")]["digest"] = (
                "sha256:" + "0" * 64
            )
            changed["materialization_report_sets"]["static"] = (
                release.materialization_report_set_digest(changed["materialization_reports"], "static")
            )
            updated = release.production_support_tuples(ROOT, manifest, changed)
            baseline_by_platform = {
                row["platform"]: row["evidence_digest"] for row in baseline
            }
            updated_by_platform = {
                row["platform"]: row["evidence_digest"] for row in updated
            }
            self.assertNotEqual(
                baseline_by_platform[
                    f"linux-{release.platform.machine().lower()}-static"
                ],
                updated_by_platform[
                    f"linux-{release.platform.machine().lower()}-static"
                ],
            )
            self.assertEqual(
                baseline_by_platform[
                    f"linux-{release.platform.machine().lower()}-shared"
                ],
                updated_by_platform[
                    f"linux-{release.platform.machine().lower()}-shared"
                ],
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

    def test_tracked_materialization_gap_allows_only_explicit_empty_evidence(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            evidence = self.evaluation_evidence(
                directory, "classified-with-gaps"
            )
            evidence["materialization_reports"] = {}
            evidence["materialization_report_sets"] = {}
            evidence["materialization_evidence"] = {
                "state": "tracked-gap-empty",
                "request_count": 0,
                "report_count": 0,
                "report_set_count": 0,
                "owner_issue": "#181",
                "feedback": ["DF-0182", "DF-0187"],
            }
            with mock.patch.object(
                release, "collect_release_evidence", return_value=evidence
            ):
                report = release.make_evaluation_report(
                    ROOT,
                    release.load(ROOT / release.MANIFEST),
                    directory,
                    evidence["security_path"],
                    "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
                    "1" * 40,
                    "2026-07-19T00:00:00Z",
                )
            self.assertEqual(report["qualification"], "not-qualified")
            self.assertEqual(report["evidence"]["materialization_reports"], [])
            self.assertEqual(
                report["evidence"]["materialization_report_sets"], []
            )
            self.assertEqual(
                report["evidence"]["materialization_evidence"]["owner_issue"],
                "#181",
            )

            wrong_owner = copy.deepcopy(report)
            wrong_owner["evidence"]["materialization_evidence"]["owner_issue"] = (
                "#999"
            )
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "schema validation failed"
            ):
                release.validate_schema(
                    wrong_owner,
                    release.load(ROOT / release.EVALUATION_REPORT_SCHEMA),
                    "tracked-gap evaluation",
                )

            falsely_qualified = copy.deepcopy(report)
            falsely_qualified["qualification"] = "qualified"
            falsely_qualified["qualified"] = True
            falsely_qualified["scope_inventory"] = self.scope_inventory("qualified")
            falsely_qualified["strict_report"]["eligible"] = True
            falsely_qualified["reason_codes"] = []
            with self.assertRaisesRegex(
                release.ReleaseQualificationError, "schema validation failed"
            ):
                release.validate_schema(
                    falsely_qualified,
                    release.load(ROOT / release.EVALUATION_REPORT_SCHEMA),
                    "qualified empty materialization evaluation",
                )

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

    def test_release_independently_rejects_non_exact_observation_census(self) -> None:
        request = release.materialization.sample_request(ROOT)
        report = release.materialization.sample_report(ROOT, request)
        type_batch = next(
            batch
            for batch in report["task_results"][0]["batches"]
            if batch["descriptor_id"]
            == "frontend.clang22.type_observation.v2"
        )
        row = type_batch["observation_equivalence_census"]["rows"][0]
        row.update(
            {
                "exact_equivalence": False,
                "limitation": "type sugar cannot be reconstructed exactly",
                "limitation_digest": release.materialization.content_digest(
                    b"type sugar cannot be reconstructed exactly"
                ),
            }
        )
        type_batch["row_bindings"][0].update(
            {
                "exact_equivalence": False,
                "limitation_digest": row["limitation_digest"],
            }
        )
        report["side_channels"]["guarantee"]["approximation"] = (
            "under_approximation"
        )
        release.materialization.rebind_report_digest_chain(ROOT, request, report)
        release.materialization.validate_report(ROOT, request, report)
        with self.assertRaisesRegex(
            release.ReleaseQualificationError,
            "contains a non-exact observation",
        ):
            release.validate_release_observation_equivalence(report)

    def test_release_independently_rejects_observation_row_binding_drift(self) -> None:
        request = release.materialization.sample_request(ROOT)
        report = release.materialization.sample_report(ROOT, request)
        type_batch = next(
            batch
            for batch in report["task_results"][0]["batches"]
            if batch["descriptor_id"]
            == "frontend.clang22.type_observation.v2"
        )
        limitation = "forged row-binding limitation"
        type_batch["row_bindings"][0].update(
            {
                "exact_equivalence": False,
                "limitation_digest": release.materialization.content_digest(
                    limitation.encode("utf-8")
                ),
            }
        )
        with self.assertRaisesRegex(
            release.ReleaseQualificationError,
            "observation equivalence row binding differs",
        ):
            release.validate_release_observation_equivalence(report)


if __name__ == "__main__":
    unittest.main()
