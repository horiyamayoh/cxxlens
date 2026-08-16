#!/usr/bin/env python3
"""Contract and fail-closed tests for the bounded #277 context slice."""

from __future__ import annotations

import copy
import pathlib
import subprocess
import sys
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/quality"))

import check_ng_agent_context as agent  # noqa: E402


USE_CASE_ID = "repository-semantic-query.explain-translation-unit.v1"


def git_value(expression: str) -> str:
    return subprocess.run(
        ["git", "-C", str(ROOT), "rev-parse", expression],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


class AgentContextTests(unittest.TestCase):
    def packet(self) -> dict:
        with mock.patch.object(agent, "worktree_status", return_value=[]), mock.patch.object(
            agent.catalog, "reject_dirty_source_files"
        ):
            return agent.build_context(
                ROOT,
                use_case_id=USE_CASE_ID,
                issue="#261",
                revision=git_value("HEAD"),
                tree=git_value("HEAD^{tree}"),
            )

    def test_main_packet_is_schema_valid_and_machine_bound(self) -> None:
        packet = self.packet()
        schema = agent.load_yaml(ROOT / agent.CONTEXT_SCHEMA_PATH)
        agent.validate_schema(packet, schema, "test agent-context")
        with mock.patch.object(agent, "worktree_status", return_value=[]), mock.patch.object(
            agent.catalog, "reject_dirty_source_files"
        ):
            agent.validate_context(
                ROOT,
                packet,
                use_case_id=USE_CASE_ID,
                issue="#261",
                revision=packet["binding"]["revision"],
                tree=packet["binding"]["tree"],
            )
        self.assertEqual(packet["schema"], "cxxlens.ng-agent-context.v1")
        self.assertEqual(packet["role"], "bounded-non-authoritative-context-projection")
        self.assertEqual(packet["authority_scope"], "non-authoritative-projection")
        self.assertEqual(packet["release_authority"], "none")
        self.assertEqual(packet["demand_source"]["tracking_issue"], "#275")
        self.assertEqual(packet["constructibility"]["gate_issue"], "#276")
        self.assertEqual(packet["binding"]["worktree"], "clean")
        self.assertEqual(packet["binding"]["generator"], agent.GENERATOR_PATH.as_posix())
        self.assertEqual(packet["design_feedback_records"][0]["path"], agent.DF_0261_RECORD_PATH.as_posix())
        self.assertEqual(packet["design_feedback_records"][0]["status"], "proposed")
        self.assertEqual(packet["design_feedback_records"][0]["implementation_disposition"], "blocked")
        self.assertEqual(packet["design_feedback_records"][0]["review_status"], "pending")
        self.assertEqual(packet["design_feedback_records"][0]["resolution_refs"], [])
        self.assertEqual(
            packet["binding"]["constructibility_authority_digest"],
            packet["constructibility"]["authority_digest"],
        )
        self.assertEqual(
            [row["id"] for row in packet["capability_path"]],
            [
                "input.source-closure.v1",
                "input.effective-invocation.v1",
                "provider.clang22-materialization.v2_1",
                "artifact.semantic-snapshot.v1",
                "recipe.explain-translation-unit.v1",
            ],
        )
        self.assertIn("## Completion plan", agent.render_markdown(packet))

    def test_untemplated_admitted_family_fails_closed(self) -> None:
        with self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.use-case-not-admitted:semantic-graph-navigation\.v1",
        ):
            with mock.patch.object(agent, "worktree_status", return_value=[]), mock.patch.object(
                agent.catalog, "reject_dirty_source_files"
            ):
                agent.build_context(
                    ROOT,
                    use_case_id="semantic-graph-navigation.v1",
                    issue="#278",
                    revision=git_value("HEAD"),
                    tree=git_value("HEAD^{tree}"),
                )

    def test_unknown_or_forward_capability_dependency_fails_closed(self) -> None:
        readiness = agent.load_yaml(ROOT / agent.READINESS_PATH)
        family = copy.deepcopy(
            readiness["product_direction"]["roadmap"]["use_case_families"][0]
        )
        family["capability_path"][1]["requires"] = ["input.synthetic.v1"]
        with self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.capability-dependency-unknown",
        ):
            agent.validate_capability_path(family)

        family = copy.deepcopy(
            readiness["product_direction"]["roadmap"]["use_case_families"][0]
        )
        family["capability_path"][0]["requires"] = [
            "input.effective-invocation.v1"
        ]
        with self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.capability-dependency-forward",
        ):
            agent.validate_capability_path(family)

        family = copy.deepcopy(
            readiness["product_direction"]["roadmap"]["use_case_families"][0]
        )
        family["capability_path"][0].pop("owner_issue")
        with self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.capability-owner-or-dependency-missing",
        ):
            agent.validate_capability_path(family)

    def test_overlapping_write_scope_fails_closed(self) -> None:
        with self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.write-path-overlap",
        ):
            agent.validate_path_set(
                ["src/llvm/clang22", "src/llvm/clang22/worker"],
                "write-path",
                reject_overlap=True,
            )

    def test_stale_binding_is_rejected(self) -> None:
        packet = self.packet()
        packet["binding"]["tree"] = "0" * 40
        with self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.stale-or-not-machine-derived",
        ):
            with mock.patch.object(agent, "worktree_status", return_value=[]), mock.patch.object(
                agent.catalog, "reject_dirty_source_files"
            ):
                agent.validate_context(
                    ROOT,
                    packet,
                    use_case_id=USE_CASE_ID,
                    issue="#261",
                    revision=packet["binding"]["revision"],
                    tree=git_value("HEAD^{tree}"),
                )

    def test_demand_projection_drift_is_rejected(self) -> None:
        with mock.patch.object(agent.catalog, "reject_dirty_source_files"):
            report = copy.deepcopy(agent.catalog.build_report(ROOT))
        next(
            entry
            for entry in report["use_cases"]
            if entry["id"] == "repository-semantic-query"
        )["capabilities"].append("synthetic-capability")
        with mock.patch.object(agent.catalog, "build_report", return_value=report):
            with self.assertRaisesRegex(
                agent.AgentContextError,
                r"agent-context\.demand-closure-capability-drift",
            ):
                self.packet()

    def test_constructibility_promotion_is_rejected(self) -> None:
        readiness = agent.load_yaml(ROOT / agent.READINESS_PATH)
        family = copy.deepcopy(
            readiness["product_direction"]["roadmap"]["use_case_families"][0]
        )
        template = copy.deepcopy(readiness["product_direction"]["agent_context"]["first_packet"])
        template["constructibility"]["disposition"] = "constructible"
        with mock.patch.object(agent.catalog, "reject_dirty_source_files"):
            report = agent.catalog.build_report(ROOT)
        with mock.patch.object(
            agent,
            "select_source",
            return_value=(family, template, report),
        ):
            with self.assertRaisesRegex(
                agent.AgentContextError,
                r"agent-context\.constructibility-promotion-forbidden",
            ):
                self.packet()

    def test_blocking_feedback_reference_is_required(self) -> None:
        readiness = agent.load_yaml(ROOT / agent.READINESS_PATH)
        family = copy.deepcopy(
            readiness["product_direction"]["roadmap"]["use_case_families"][0]
        )
        template = copy.deepcopy(readiness["product_direction"]["agent_context"]["first_packet"])
        template["known_design_feedback"].remove("DF-0261")
        with mock.patch.object(agent.catalog, "reject_dirty_source_files"):
            report = agent.catalog.build_report(ROOT)
        with mock.patch.object(
            agent,
            "select_source",
            return_value=(family, template, report),
        ):
            with self.assertRaisesRegex(
                agent.AgentContextError,
                r"agent-context\.design-feedback-binding-missing",
            ):
                self.packet()

    def test_tracked_and_untracked_worktree_fail_closed(self) -> None:
        revision = git_value("HEAD")
        tree = git_value("HEAD^{tree}")
        for marker in (" M tracked.py", "?? untracked.py"):
            with self.subTest(marker=marker), mock.patch.object(
                agent, "worktree_status", return_value=[marker]
            ), self.assertRaisesRegex(
                agent.AgentContextError, r"agent-context\.worktree-dirty"
            ):
                agent.build_context(
                    ROOT,
                    use_case_id=USE_CASE_ID,
                    issue="#261",
                    revision=revision,
                    tree=tree,
                )

    def test_authority_reading_paths_are_individually_bound(self) -> None:
        packet = self.packet()
        self.assertEqual(
            [row["path"] for row in packet["authority_reading_bindings"]],
            packet["authority_reading_set"],
        )
        for row in packet["authority_reading_bindings"]:
            self.assertTrue((ROOT / row["path"]).is_file())
            mode, blob, content = agent.git_authority.bind_head_blob(ROOT, row["path"])
            self.assertEqual(row["mode"], mode)
            self.assertEqual(row["blob"], blob)
            self.assertEqual(row["digest"], agent.git_authority.sha256_digest(content))
        with self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.authority-reading-path-missing",
        ):
            agent.bind_authority_reading(ROOT, ["AGENTS.md", "missing-authority.md"])

    def test_ambient_untracked_authority_is_rejected(self) -> None:
        ambient = ROOT / "agent-context-ambient-authority.md"
        ambient.write_text("ambient authority\n", encoding="utf-8")
        self.addCleanup(lambda: ambient.unlink(missing_ok=True))
        with self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.authority-reading-path-not-tracked",
        ):
            agent.bind_authority_reading(ROOT, [ambient.relative_to(ROOT).as_posix()])

    def test_git_metadata_and_non_blob_authority_are_rejected(self) -> None:
        with self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.authority-reading-set-noncanonical",
        ):
            agent.bind_authority_reading(ROOT, [".git/HEAD"])
        with self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.authority-reading-path-not-blob",
        ):
            agent.bind_authority_reading(ROOT, ["docs"])

    def test_symlink_authority_is_rejected(self) -> None:
        with mock.patch.object(pathlib.Path, "is_symlink", return_value=True), self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.authority-reading-path-symlink",
        ):
            agent.bind_authority_reading(ROOT, ["AGENTS.md"])

    def test_head_blob_content_drift_is_rejected_even_when_status_is_clean(self) -> None:
        path = ROOT / agent.READINESS_SCHEMA_PATH
        original = path.read_bytes()
        path.write_bytes(original + b"\n# hidden tracked authority mutation\n")
        self.addCleanup(lambda: path.write_bytes(original))
        with mock.patch.object(agent, "worktree_status", return_value=[]), mock.patch.object(
            agent.catalog, "reject_dirty_source_files"
        ), self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.authority-source-.*path-content-mismatch",
        ):
            agent.build_context(
                ROOT,
                use_case_id=USE_CASE_ID,
                issue="#261",
                revision=git_value("HEAD"),
                tree=git_value("HEAD^{tree}"),
            )

    def test_index_authority_flags_are_rejected(self) -> None:
        relative = agent.READINESS_SCHEMA_PATH.as_posix()
        flag_commands = (
            ("--assume-unchanged", "--no-assume-unchanged", "path-assume-unchanged"),
            ("--skip-worktree", "--no-skip-worktree", "path-skip-worktree"),
        )
        for enable, disable, error_code in flag_commands:
            with self.subTest(flag=enable):
                try:
                    subprocess.run(
                        [
                            "git",
                            "-C",
                            str(ROOT),
                            "update-index",
                            enable,
                            "--",
                            relative,
                        ],
                        check=True,
                    )
                    with self.assertRaisesRegex(
                        agent.AgentContextError,
                        rf"agent-context\.authority-reading-{error_code}",
                    ):
                        agent.bind_authority_reading(ROOT, [relative])
                finally:
                    subprocess.run(
                        [
                            "git",
                            "-C",
                            str(ROOT),
                            "update-index",
                            disable,
                            "--",
                            relative,
                        ],
                        check=True,
                    )

    def test_design_feedback_metadata_is_exactly_blocked_pending(self) -> None:
        metadata, _ = agent.design_feedback.split_front_matter(
            ROOT / agent.DF_0261_RECORD_PATH
        )
        for field, value in (
            ("status", "accepted"),
            ("implementation_disposition", "may-proceed"),
        ):
            mutated = copy.deepcopy(metadata)
            mutated[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(
                agent.AgentContextError, r"agent-context\.design-feedback-"
            ):
                agent.validate_design_feedback_metadata(mutated)

        mutated = copy.deepcopy(metadata)
        mutated["review"]["status"] = "complete"
        with self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.design-feedback-review-status-mismatch",
        ):
            agent.validate_design_feedback_metadata(mutated)

        mutated = copy.deepcopy(metadata)
        mutated["resolution_refs"] = ["docs/design/README.md"]
        with self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.design-feedback-resolution-refs-mismatch",
        ):
            agent.validate_design_feedback_metadata(mutated)

    def test_machine_contract_and_witness_digest_drift_fail_closed(self) -> None:
        readiness = agent.load_yaml(ROOT / agent.READINESS_PATH)
        product = readiness["product_direction"]
        family = product["roadmap"]["use_case_families"][0]
        path = agent.validate_capability_path(family)
        gate = product["constructibility_gate"]
        context_authority = product["agent_context"]
        with self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.machine-contract-authority-invalid",
        ):
            agent.authority_contract_ids(
                product,
                family,
                path,
                context_authority,
                {**gate, "contract": "not-a-contract"},
            )

        packet = self.packet()
        packet["constructibility"]["required_witnesses"].append("synthetic-witness")
        with mock.patch.object(agent, "worktree_status", return_value=[]), mock.patch.object(
            agent.catalog, "reject_dirty_source_files"
        ), self.assertRaisesRegex(
            agent.AgentContextError, r"agent-context\.stale-or-not-machine-derived"
        ):
            agent.validate_context(
                ROOT,
                packet,
                use_case_id=USE_CASE_ID,
                issue="#261",
                revision=packet["binding"]["revision"],
                tree=packet["binding"]["tree"],
            )

    def test_authority_marker_promotion_fails_closed(self) -> None:
        packet = self.packet()
        packet["authority_scope"] = "readiness-authority"
        with mock.patch.object(agent, "worktree_status", return_value=[]), mock.patch.object(
            agent.catalog, "reject_dirty_source_files"
        ), self.assertRaisesRegex(
            agent.AgentContextError, r"agent-context\.stale-or-not-machine-derived"
        ):
            agent.validate_context(
                ROOT,
                packet,
                use_case_id=USE_CASE_ID,
                issue="#261",
                revision=packet["binding"]["revision"],
                tree=packet["binding"]["tree"],
            )


if __name__ == "__main__":
    unittest.main()
