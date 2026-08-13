#!/usr/bin/env python3
"""Positive and fail-closed tests for implementation design feedback."""

from __future__ import annotations

import copy
import pathlib
import shutil
import sys
import tempfile
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_design_feedback import (  # noqa: E402
    DesignFeedbackError,
    INDEX,
    load_records,
    render_index,
    validate_documents,
    validate_issue_ready,
)


class NgDesignFeedbackTest(unittest.TestCase):
    def copied_root(self, temporary: str) -> pathlib.Path:
        root = pathlib.Path(temporary)
        shutil.copytree(
            ROOT / "docs/development/implementation-learning",
            root / "docs/development/implementation-learning",
        )
        for relative in (
            ".github/ISSUE_TEMPLATE/design-feedback.yml",
            "schemas/cxxlens_ng_design_feedback_record.schema.yaml",
        ):
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        for relative in (
            "docs/design/cxxlens_next_generation_integrated_design_ja.md",
            "docs/design/authority.md",
            "docs/design/adr/9999-test-resolution.md",
            "docs/design/adr/9998-proposed-resolution.md",
            "docs/design/adr/README.md",
            "reviews/independent.md",
        ):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            status = (
                "\n- Status: Accepted\n"
                if relative.endswith("9999-test-resolution.md")
                else "\n- Status: Proposed\n"
                if relative.endswith("9998-proposed-resolution.md")
                else ""
            )
            path.write_text(f"# {relative}\n{status}", encoding="utf-8")
        repository_records = load_records(ROOT)
        for record in repository_records:
            metadata = record.metadata
            references = [
                *metadata["authority_refs"],
                *metadata["resolution_refs"],
                *metadata["review"]["refs"],
            ]
            for reference in references:
                if reference.startswith(("https://", "http://")):
                    continue
                relative = pathlib.PurePosixPath(reference.split("#", 1)[0])
                source = ROOT / relative
                if not source.is_file():
                    continue
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
        return root

    @staticmethod
    def metadata(number: int = 300) -> dict:
        return {
            "id": f"DF-{number:04d}",
            "title": "Synthetic design observation",
            "status": "observed",
            "kind": "design-opportunity",
            "impact": "local",
            "confidence": "medium",
            "implementation_disposition": "may-proceed",
            "scope": ["synthetic.contract"],
            "authority_refs": [
                "docs/design/cxxlens_next_generation_integrated_design_ja.md"
            ],
            "tracking_issue": f"#{number}",
            "implementation_issues": ["#300"],
            "resolution_refs": [],
            "review": {
                "mode": "self",
                "status": "pending",
                "author": "agent:synthetic-author",
                "reviewer": None,
                "refs": [],
            },
            "created": "2026-07-19",
        }

    @staticmethod
    def body(title: str = "Synthetic design observation") -> str:
        sections = (
            "Observation",
            "Working mental model",
            "Mismatch or opportunity",
            "Evidence",
            "Alternatives and trade-offs",
            "Recommendation",
            "Disposition",
        )
        text = [f"# {title}", ""]
        for heading in sections:
            text.extend((f"## {heading}", "", f"Content for {heading}.", ""))
        return "\n".join(text).rstrip() + "\n"

    def write_record(
        self,
        root: pathlib.Path,
        metadata: dict,
        *,
        slug: str = "synthetic",
        body: str | None = None,
    ) -> pathlib.Path:
        number = int(metadata["id"].split("-", 1)[1])
        path = (
            root
            / "docs/development/implementation-learning/records"
            / f"df-{number:04d}-{slug}.md"
        )
        front = yaml.safe_dump(metadata, sort_keys=False, allow_unicode=True).strip()
        path.write_text(
            f"---\n{front}\n---\n\n{body or self.body(metadata['title'])}",
            encoding="utf-8",
        )
        return path

    @staticmethod
    def refresh_index(root: pathlib.Path) -> None:
        records = load_records(root)
        (root / INDEX).write_text(render_index(records), encoding="utf-8")

    def test_repository_contract_is_valid(self) -> None:
        validate_documents(ROOT)

    def test_valid_lifecycle_states_are_accepted(self) -> None:
        cases = (
            ("observed", {}),
            (
                "accepted",
                {
                    "resolution_refs": ["docs/design/authority.md"],
                    "review": {
                        "mode": "self",
                        "status": "complete",
                        "author": "agent:synthetic-author",
                        "reviewer": "agent:synthetic-author",
                        "refs": [],
                    },
                },
            ),
            ("rejected", {}),
            (
                "deferred",
                {"follow_up_issue": "#400", "reevaluate_when": "Clang 23 support"},
            ),
        )
        for status, additions in cases:
            with self.subTest(status=status), tempfile.TemporaryDirectory() as temporary:
                root = self.copied_root(temporary)
                metadata = self.metadata()
                metadata["status"] = status
                metadata.update(additions)
                self.write_record(root, metadata)
                self.refresh_index(root)
                identifiers = {
                    record.metadata["id"] for record in validate_documents(root)
                }
                self.assertIn("DF-0300", identifiers)

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            successor = self.metadata(301)
            self.write_record(root, successor, slug="successor")
            original = self.metadata(300)
            original.update({"status": "superseded", "superseded_by": "DF-0301"})
            self.write_record(root, original)
            self.refresh_index(root)
            identifiers = {
                record.metadata["id"] for record in validate_documents(root)
            }
            self.assertTrue({"DF-0300", "DF-0301"}.issubset(identifiers))

    def test_duplicate_id_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            metadata = self.metadata()
            self.write_record(root, metadata, slug="first")
            self.write_record(root, copy.deepcopy(metadata), slug="second")
            with self.assertRaisesRegex(DesignFeedbackError, "duplicate IDs"):
                load_records(root)

    def test_id_tracking_issue_mismatch_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            metadata = self.metadata()
            metadata["tracking_issue"] = "#201"
            self.write_record(root, metadata)
            with self.assertRaisesRegex(DesignFeedbackError, "tracking issue mismatch"):
                load_records(root)

    def test_noncanonical_zero_padding_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            metadata = self.metadata()
            metadata["id"] = "DF-000300"
            path = self.write_record(root, metadata)
            canonical_name = path.with_name("df-000300-synthetic.md")
            path.rename(canonical_name)
            with self.assertRaisesRegex(DesignFeedbackError, "expected DF-0300"):
                load_records(root)

    def test_missing_section_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            body = self.body().replace("## Evidence\n\nContent for Evidence.\n\n", "")
            self.write_record(root, self.metadata(), body=body)
            with self.assertRaisesRegex(DesignFeedbackError, "missing or out of order"):
                load_records(root)

    def test_duplicate_front_matter_keys_are_rejected(self) -> None:
        for key, duplicate in (
            ("status", "status: proposed\n"),
            (
                "implementation_disposition",
                "implementation_disposition: blocked\n",
            ),
        ):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as temporary:
                root = self.copied_root(temporary)
                path = self.write_record(root, self.metadata())
                text = path.read_text(encoding="utf-8")
                marker = f"{key}: "
                position = text.index(marker)
                text = text[:position] + duplicate + text[position:]
                path.write_text(text, encoding="utf-8")
                with self.assertRaisesRegex(DesignFeedbackError, "duplicate key"):
                    load_records(root)

    def test_record_requires_an_implementation_issue(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            metadata = self.metadata()
            metadata["implementation_issues"] = []
            self.write_record(root, metadata)
            with self.assertRaisesRegex(DesignFeedbackError, "schema validation failed"):
                load_records(root)

    def test_open_high_risk_record_must_block_implementation(self) -> None:
        for status in ("observed", "investigating", "proposed"):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as temporary:
                root = self.copied_root(temporary)
                metadata = self.metadata()
                metadata.update({"status": status, "impact": "contract"})
                self.write_record(root, metadata)
                with self.assertRaisesRegex(DesignFeedbackError, "schema validation failed"):
                    load_records(root)

    def test_missing_authority_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            metadata = self.metadata()
            metadata["authority_refs"] = ["docs/design/missing.md"]
            self.write_record(root, metadata)
            with self.assertRaisesRegex(DesignFeedbackError, "does not name"):
                load_records(root)

    def test_non_normative_and_archived_authority_refs_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            archived = root / "docs/archive/historical.md"
            archived.parent.mkdir(parents=True, exist_ok=True)
            archived.write_text("# Historical\n", encoding="utf-8")
            for reference in (
                "docs/development/implementation-learning/README.md",
                "docs/archive/historical.md",
                "reviews/independent.md",
            ):
                with self.subTest(reference=reference):
                    metadata = self.metadata()
                    metadata["authority_refs"] = [reference]
                    self.write_record(root, metadata)
                    with self.assertRaisesRegex(
                        DesignFeedbackError, "not a normative authority"
                    ):
                        load_records(root)

    def test_review_refs_cannot_escape_the_repository(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            for reference in ("/etc/passwd", "../outside.md"):
                with self.subTest(reference=reference):
                    metadata = self.metadata()
                    metadata["review"]["refs"] = [reference]
                    self.write_record(root, metadata)
                    with self.assertRaisesRegex(DesignFeedbackError, "review refs are invalid"):
                        load_records(root)

    def test_accepted_record_requires_resolution(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            metadata = self.metadata()
            metadata["status"] = "accepted"
            self.write_record(root, metadata)
            with self.assertRaisesRegex(DesignFeedbackError, "schema validation failed"):
                load_records(root)

    def test_deferred_record_requires_follow_up_and_trigger(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            metadata = self.metadata()
            metadata["status"] = "deferred"
            self.write_record(root, metadata)
            with self.assertRaisesRegex(DesignFeedbackError, "schema validation failed"):
                load_records(root)

    def test_superseded_record_requires_existing_successor(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            metadata = self.metadata()
            metadata.update({"status": "superseded", "superseded_by": "DF-0301"})
            self.write_record(root, metadata)
            with self.assertRaisesRegex(DesignFeedbackError, "no distinct successor"):
                load_records(root)

    def test_high_risk_acceptance_requires_independent_review_and_adr(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            metadata = self.metadata()
            metadata.update(
                {
                    "status": "accepted",
                    "impact": "contract",
                    "resolution_refs": ["docs/design/authority.md"],
                }
            )
            self.write_record(root, metadata)
            with self.assertRaisesRegex(DesignFeedbackError, "schema validation failed"):
                load_records(root)

            metadata["review"] = {
                "mode": "independent",
                "status": "complete",
                "author": "agent:synthetic-author",
                "reviewer": "agent:synthetic-author",
                "refs": ["reviews/independent.md"],
            }
            self.write_record(root, metadata)
            with self.assertRaisesRegex(DesignFeedbackError, "no accepted ADR resolution"):
                load_records(root)

            for reference in (
                "docs/design/adr/README.md",
                "docs/design/adr/9998-proposed-resolution.md",
            ):
                with self.subTest(reference=reference):
                    metadata["resolution_refs"] = [reference]
                    self.write_record(root, metadata)
                    with self.assertRaisesRegex(
                        DesignFeedbackError, "no accepted ADR resolution"
                    ):
                        load_records(root)

            metadata["resolution_refs"] = ["docs/design/adr/9999-test-resolution.md"]
            self.write_record(root, metadata)
            with self.assertRaisesRegex(DesignFeedbackError, "not independent"):
                load_records(root)

            metadata["review"]["reviewer"] = "agent:independent-reviewer"
            self.write_record(root, metadata)
            with self.assertRaisesRegex(DesignFeedbackError, "not bound"):
                load_records(root)

            metadata["review"]["refs"] = [
                "https://github.com/example/cxxlens/issues/300#issuecomment-1"
            ]
            self.write_record(root, metadata)
            with self.assertRaisesRegex(DesignFeedbackError, "review refs are invalid"):
                load_records(root)

            metadata["review"]["refs"] = [
                "https://github.com/horiyamayoh/cxxlens/issues/300#issuecomment-1"
            ]
            self.write_record(root, metadata)
            identifiers = {record.metadata["id"] for record in load_records(root)}
            self.assertIn("DF-0300", identifiers)

    def test_issue_ready_rejects_unresolved_blocker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            metadata = self.metadata()
            metadata["implementation_disposition"] = "blocked"
            self.write_record(root, metadata)
            records = load_records(root)
            with self.assertRaisesRegex(DesignFeedbackError, "unresolved blocking"):
                validate_issue_ready(records, "300")

            metadata["status"] = "rejected"
            metadata["implementation_disposition"] = "may-proceed"
            self.write_record(root, metadata)
            self.assertEqual(validate_issue_ready(load_records(root), "#300"), ["DF-0300"])

    def test_issue_ready_accepts_nonblocking_open_record(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.write_record(root, self.metadata())
            self.assertEqual(validate_issue_ready(load_records(root), "#300"), ["DF-0300"])

    def test_stale_index_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.write_record(root, self.metadata())
            with self.assertRaisesRegex(DesignFeedbackError, "index is stale"):
                validate_documents(root)

    def test_mental_model_requires_non_normative_banner(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            model = (
                root
                / "docs/development/implementation-learning/mental-models"
                / "authority-and-learning-loop.md"
            )
            model.write_text(
                model.read_text(encoding="utf-8").replace(
                    "> Status: Non-normative explanatory model.\n", ""
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(DesignFeedbackError, "non-normative banner"):
                validate_documents(root)

    def test_issue_template_requires_record_path(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            template_path = root / ".github/ISSUE_TEMPLATE/design-feedback.yml"
            template = yaml.safe_load(template_path.read_text(encoding="utf-8"))
            template["body"] = [
                row for row in template["body"] if row.get("id") != "record_path"
            ]
            template_path.write_text(
                yaml.safe_dump(template, sort_keys=False, allow_unicode=True),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(DesignFeedbackError, "template fields differ"):
                validate_documents(root)


if __name__ == "__main__":
    unittest.main()
