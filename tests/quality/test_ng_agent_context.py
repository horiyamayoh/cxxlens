#!/usr/bin/env python3
"""Contract and fail-closed tests for the bounded #277 context slice."""

from __future__ import annotations

import copy
import os
import pathlib
import subprocess
import sys
import tempfile
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


def git_value_for_root(root: pathlib.Path, expression: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), "rev-parse", expression],
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
        # status / implementation_disposition / review_status / resolution_refs
        # are DF-0261's live lifecycle fields, which legitimately advance over
        # time (see check_ng_design_feedback.py's own acceptance gate) --
        # DF-0261 itself moved from proposed/blocked/pending to
        # accepted/may-proceed/complete via real, independently reviewed #261
        # progress. Bind against the record's live content rather than a
        # point-in-time snapshot that would otherwise go stale.
        live_metadata, _ = agent.design_feedback.split_front_matter(
            ROOT / agent.DF_0261_RECORD_PATH
        )
        self.assertEqual(
            packet["design_feedback_records"][0]["status"], live_metadata["status"]
        )
        self.assertEqual(
            packet["design_feedback_records"][0]["implementation_disposition"],
            live_metadata["implementation_disposition"],
        )
        self.assertEqual(
            packet["design_feedback_records"][0]["review_status"],
            live_metadata["review"]["status"],
        )
        self.assertEqual(
            packet["design_feedback_records"][0]["resolution_refs"],
            list(live_metadata["resolution_refs"]),
        )
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
        relative = agent.READINESS_SCHEMA_PATH.as_posix()
        path = ROOT / agent.READINESS_SCHEMA_PATH
        original = path.read_bytes()
        try:
            path.write_bytes(original + b"\n# hidden tracked authority mutation\n")
            subprocess.run(
                ["git", "-C", str(ROOT), "update-index", "--assume-unchanged", "--", relative],
                check=True,
            )
            self.assertEqual(
                subprocess.run(
                    [
                        "git",
                        "-C",
                        str(ROOT),
                        "status",
                        "--porcelain=v1",
                        "--untracked-files=all",
                    ],
                    check=True,
                    capture_output=True,
                    text=True,
                ).stdout,
                "",
            )
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
        finally:
            path.write_bytes(original)
            subprocess.run(
                [
                    "git",
                    "-C",
                    str(ROOT),
                    "update-index",
                    "--no-assume-unchanged",
                    "--",
                    relative,
                ],
                check=True,
            )

    def test_adversarial_replacement_after_read_is_rejected(self) -> None:
        relative = "AGENTS.md"
        path = ROOT / relative
        original = path.read_bytes()
        original_mode = path.stat().st_mode
        replaced = False
        leaf_fd: int | None = None
        real_open = agent.git_authority.os.open
        real_read = agent.git_authority.os.read

        def capture_open(name: str, flags: int, *args: object, **kwargs: object) -> int:
            nonlocal leaf_fd
            descriptor = real_open(name, flags, *args, **kwargs)
            if name == relative and kwargs.get("dir_fd") is not None:
                leaf_fd = descriptor
            return descriptor

        def read_then_replace(fd: int, size: int) -> bytes:
            nonlocal replaced
            chunk = real_read(fd, size)
            if fd == leaf_fd and not replaced:
                replacement = path.with_name(path.name + ".replacement")
                replacement.write_bytes(original)
                os.chmod(replacement, original_mode)
                os.replace(replacement, path)
                replaced = True
            return chunk

        try:
            with mock.patch.object(
                agent.git_authority.os, "open", side_effect=capture_open
            ), mock.patch.object(
                agent.git_authority.os, "read", side_effect=read_then_replace
            ), self.assertRaisesRegex(
                agent.git_authority.GitAuthorityError,
                r"git-authority.path-replaced",
            ):
                agent.git_authority.bind_head_blob(ROOT, relative)
        finally:
            path.write_bytes(original)
            os.chmod(path, original_mode)
            path.with_name(path.name + ".replacement").unlink(missing_ok=True)

    def test_fifo_replacement_is_rejected_without_blocking_before_fstat(self) -> None:
        relative = "AGENTS.md"
        path = ROOT / relative
        original = path.read_bytes()
        try:
            path.unlink()
            os.mkfifo(path, mode=0o644)
            with self.assertRaisesRegex(
                agent.git_authority.GitAuthorityError,
                r"git-authority.path-not-regular-file",
            ):
                agent.git_authority.bind_head_blob(ROOT, relative)
        finally:
            path.unlink(missing_ok=True)
            path.write_bytes(original)
            os.chmod(path, 0o644)

    def test_head_snapshot_replacement_is_rejected_after_all_paths_bind(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            subprocess.run(["git", "-C", str(root), "init", "--quiet"], check=True)
            (root / "first.txt").write_text("first\n", encoding="utf-8")
            (root / "second.txt").write_text("second\n", encoding="utf-8")
            subprocess.run(
                ["git", "-C", str(root), "add", "first.txt", "second.txt"],
                check=True,
            )
            git_identity = [
                "-c",
                "user.name=authority-test",
                "-c",
                "user.email=authority-test@example.invalid",
            ]
            subprocess.run(
                ["git", "-C", str(root), *git_identity, "commit", "--quiet", "-m", "first"],
                check=True,
            )
            first = git_value_for_root(root, "HEAD")
            subprocess.run(
                [
                    "git",
                    "-C",
                    str(root),
                    *git_identity,
                    "commit",
                    "--quiet",
                    "--allow-empty",
                    "-m",
                    "second",
                ],
                check=True,
            )
            second = git_value_for_root(root, "HEAD")
            subprocess.run(
                ["git", "-C", str(root), "checkout", "--quiet", "--detach", first],
                check=True,
            )
            real_read = agent.git_authority._read_bound_worktree_file
            replaced = False

            def read_then_advance(
                read_root: pathlib.Path, path_name: str, expected_mode: str
            ) -> bytes:
                nonlocal replaced
                content = real_read(read_root, path_name, expected_mode)
                if not replaced:
                    subprocess.run(
                        ["git", "-C", str(root), "update-ref", "HEAD", second],
                        check=True,
                    )
                    replaced = True
                return content

            with mock.patch.object(
                agent.git_authority,
                "_read_bound_worktree_file",
                side_effect=read_then_advance,
            ), self.assertRaisesRegex(
                agent.git_authority.GitAuthorityError,
                r"git-authority.head-snapshot-changed",
            ):
                agent.git_authority.require_head_bound_records(
                    root, ("first.txt", "second.txt")
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

    def test_design_feedback_metadata_binds_by_id_only(self) -> None:
        metadata, _ = agent.design_feedback.split_front_matter(
            ROOT / agent.DF_0261_RECORD_PATH
        )

        mutated = copy.deepcopy(metadata)
        mutated["id"] = "DF-0001"
        with self.assertRaisesRegex(
            agent.AgentContextError,
            r"agent-context\.design-feedback-record-id-mismatch",
        ):
            agent.validate_design_feedback_metadata(mutated)

        # status / implementation_disposition / review.status / resolution_refs
        # are deliberately NOT pinned: this function's job is to confirm the
        # packet is bound to the right record (id == DF-0261), not to freeze
        # that record's lifecycle stage. Whether #261's capability is still
        # blocked is tracked separately via the packet's own
        # constructibility.disposition and capability_path fields. DF-0261
        # legitimately moved through proposed/blocked/pending,
        # accepted/may-proceed/complete via real, independently reviewed
        # #261 progress, and both stages -- along with accumulated
        # resolution_refs -- must keep validating cleanly as long as the id
        # matches.
        for status, disposition, review_status, resolution_refs in (
            ("proposed", "blocked", "pending", []),
            ("proposed", "blocked", "pending", ["docs/design/README.md"]),
            ("accepted", "may-proceed", "complete", ["docs/design/README.md"]),
        ):
            with self.subTest(
                status=status,
                implementation_disposition=disposition,
                review_status=review_status,
            ):
                mutated = copy.deepcopy(metadata)
                mutated["status"] = status
                mutated["implementation_disposition"] = disposition
                mutated["review"]["status"] = review_status
                mutated["resolution_refs"] = resolution_refs
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
