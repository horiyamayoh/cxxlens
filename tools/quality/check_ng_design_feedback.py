#!/usr/bin/env python3
"""Validate implementation-learning documents and design-feedback records."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from dataclasses import dataclass
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
LEARNING_ROOT = pathlib.Path("docs/development/implementation-learning")
HANDBOOK = LEARNING_ROOT / "README.md"
MENTAL_MODELS = LEARNING_ROOT / "mental-models"
RECORDS = LEARNING_ROOT / "records"
INDEX = RECORDS / "README.md"
TEMPLATE = RECORDS / "_template.md"
SCHEMA = pathlib.Path("schemas/cxxlens_ng_design_feedback_record.schema.yaml")
ISSUE_TEMPLATE = pathlib.Path(".github/ISSUE_TEMPLATE/design-feedback.yml")

RECORD_PATTERN = re.compile(r"^df-[0-9]{4,}-[a-z0-9]+(?:-[a-z0-9]+)*\.md$")
ISSUE_PATTERN = re.compile(r"^#?([1-9][0-9]*)$")
RECORD_HEADINGS = (
    "Observation",
    "Working mental model",
    "Mismatch or opportunity",
    "Evidence",
    "Alternatives and trade-offs",
    "Recommendation",
    "Disposition",
)
MENTAL_MODEL_HEADINGS = (
    "Normative anchors",
    "Scope",
    "Model",
    "Known tensions",
)
ACTIVE_STATUSES = ("observed", "investigating", "proposed")
RESOLVED_STATUSES = ("accepted", "rejected", "superseded")
HIGH_RISK_IMPACTS = {
    "contract",
    "invariant",
    "security",
    "compatibility",
    "irreversible",
}
CANONICAL_GITHUB_REPOSITORY = "horiyamayoh/cxxlens"
NORMATIVE_AUTHORITY_FILES = {
    "AGENTS.md",
    "docs/design/cxxlens_next_generation_integrated_design_ja.md",
    "docs/development/agent-api-development-goal.md",
}


class DesignFeedbackError(ValueError):
    """A fail-closed implementation-learning contract violation."""


def fail(message: str) -> None:
    raise DesignFeedbackError(message)


class UniqueKeyLoader(yaml.SafeLoader):
    """Safe YAML loader that rejects ambiguous duplicate mapping keys."""


def construct_unique_mapping(
    loader: UniqueKeyLoader, node: yaml.MappingNode, deep: bool = False
) -> dict[Any, Any]:
    loader.flatten_mapping(node)
    mapping: dict[Any, Any] = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        try:
            duplicate = key in mapping
        except TypeError as error:
            raise yaml.constructor.ConstructorError(
                "while constructing a mapping",
                node.start_mark,
                "found an unhashable key",
                key_node.start_mark,
            ) from error
        if duplicate:
            raise yaml.constructor.ConstructorError(
                "while constructing a mapping",
                node.start_mark,
                f"found duplicate key {key!r}",
                key_node.start_mark,
            )
        mapping[key] = loader.construct_object(value_node, deep=deep)
    return mapping


UniqueKeyLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG,
    construct_unique_mapping,
)


def load_yaml_value(text: str, label: str) -> Any:
    try:
        return yaml.load(text, Loader=UniqueKeyLoader)
    except yaml.YAMLError as error:
        fail(f"{label} YAML is invalid: {error}")


def load_mapping(path: pathlib.Path) -> dict[str, Any]:
    value = load_yaml_value(path.read_text(encoding="utf-8"), str(path))
    if not isinstance(value, dict):
        fail(f"expected mapping: {path}")
    return value


def validate_schema(document: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(
            schema,
            format_checker=jsonschema.Draft202012Validator.FORMAT_CHECKER,
        ).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"{label} schema validation failed: {error.message}")


def split_front_matter(path: pathlib.Path) -> tuple[dict[str, Any], str]:
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    if not lines or lines[0] != "---":
        fail(f"design feedback record has no opening front matter: {path}")
    try:
        closing = lines.index("---", 1)
    except ValueError:
        fail(f"design feedback record has no closing front matter: {path}")
    metadata = load_yaml_value(
        "\n".join(lines[1:closing]), f"design feedback front matter {path}"
    )
    if not isinstance(metadata, dict):
        fail(f"design feedback front matter is not a mapping: {path}")
    return metadata, "\n".join(lines[closing + 1 :]).strip() + "\n"


def section_contents(body: str, headings: tuple[str, ...], path: pathlib.Path) -> None:
    matches = list(re.finditer(r"(?m)^## ([^\n]+)$", body))
    actual = [match.group(1) for match in matches]
    selected = [heading for heading in actual if heading in headings]
    if selected != list(headings):
        fail(
            f"required sections are missing or out of order: {path}: "
            f"expected={list(headings)}, actual={selected}"
        )
    for heading in headings:
        if actual.count(heading) != 1:
            fail(f"required section must appear exactly once: {path}: {heading}")
        index = actual.index(heading)
        match = matches[index]
        end = matches[index + 1].start() if index + 1 < len(matches) else len(body)
        if not body[match.end() : end].strip():
            fail(f"required section is empty: {path}: {heading}")


def validate_repo_ref(root: pathlib.Path, reference: str, label: str) -> None:
    path_text = reference.split("#", 1)[0]
    path = root / path_text
    if not path.is_file():
        fail(f"{label} does not name a repository file: {reference}")


def validate_authority_ref(root: pathlib.Path, reference: str) -> None:
    validate_repo_ref(root, reference, "authority_refs entry")
    path_text = reference.split("#", 1)[0]
    is_schema_authority = path_text.startswith("schemas/") and pathlib.PurePosixPath(
        path_text
    ).suffix in {".yaml", ".json"}
    if path_text in NORMATIVE_AUTHORITY_FILES or is_schema_authority:
        return
    if is_accepted_adr_ref(root, reference):
        return
    if re.fullmatch(r"docs/design/adr/[0-9]{4}-[a-z0-9-]+\.md", path_text):
        fail(f"authority_refs ADR is not accepted: {reference}")
    fail(f"authority_refs entry is not a normative authority: {reference}")


def is_accepted_adr_ref(root: pathlib.Path, reference: str) -> bool:
    path_text = reference.split("#", 1)[0]
    if not re.fullmatch(r"docs/design/adr/[0-9]{4}-[a-z0-9-]+\.md", path_text):
        return False
    path = root / path_text
    return path.is_file() and re.search(
        r"(?m)^- Status: Accepted(?:\s|$)", path.read_text(encoding="utf-8")
    ) is not None


def local_review_ref_is_valid(root: pathlib.Path, reference: str) -> bool:
    path_text = reference.split("#", 1)[0]
    pure = pathlib.PurePosixPath(path_text)
    if not path_text or pure.is_absolute() or ".." in pure.parts or "\\" in path_text:
        return False
    candidate = (root / pure).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError:
        return False
    return candidate.is_file()


def review_ref_is_valid(root: pathlib.Path, reference: str) -> bool:
    repository = re.escape(CANONICAL_GITHUB_REPOSITORY)
    if re.fullmatch(
        rf"https://github\.com/{repository}/(?:issues|pull)/[1-9][0-9]*(?:#.*)?",
        reference,
    ):
        return True
    return local_review_ref_is_valid(root, reference)


def review_ref_is_bound_to_tracking_issue(reference: str, tracking_issue: str) -> bool:
    repository = re.escape(CANONICAL_GITHUB_REPOSITORY)
    match = re.fullmatch(
        rf"https://github\.com/{repository}/issues/([1-9][0-9]*)"
        rf"#issuecomment-[1-9][0-9]*",
        reference,
    )
    return match is not None and int(match.group(1)) == int(tracking_issue[1:])


@dataclass(frozen=True)
class Record:
    path: pathlib.Path
    metadata: dict[str, Any]


def record_paths(root: pathlib.Path) -> list[pathlib.Path]:
    directory = root / RECORDS
    if not directory.is_dir():
        fail(f"design feedback records directory is missing: {RECORDS}")
    paths = []
    for path in sorted(directory.glob("*.md")):
        if path.name in {INDEX.name, TEMPLATE.name}:
            continue
        if not RECORD_PATTERN.fullmatch(path.name):
            fail(f"design feedback record filename is invalid: {path.name}")
        paths.append(path)
    return paths


def validate_record(
    root: pathlib.Path, path: pathlib.Path, schema: dict[str, Any]
) -> Record:
    metadata, body = split_front_matter(path)
    relative = path.relative_to(root)
    validate_schema(metadata, schema, f"design feedback record {relative}")

    expected_prefix = metadata["id"].lower() + "-"
    if not path.name.startswith(expected_prefix):
        fail(
            f"design feedback filename/ID mismatch: {path.name} != "
            f"{metadata['id']}"
        )
    issue_number = int(metadata["tracking_issue"][1:])
    record_number = int(metadata["id"].split("-", 1)[1])
    expected_id = f"DF-{issue_number:04d}"
    if issue_number != record_number or metadata["id"] != expected_id:
        fail(
            f"design feedback ID/tracking issue mismatch: "
            f"{metadata['id']} != {metadata['tracking_issue']} "
            f"(expected {expected_id})"
        )

    title_match = re.search(r"(?m)^# ([^\n]+)$", body)
    if title_match is None or title_match.group(1) != metadata["title"]:
        fail(f"design feedback title/front matter mismatch: {relative}")
    section_contents(body, RECORD_HEADINGS, relative)

    for reference in metadata["authority_refs"]:
        validate_authority_ref(root, reference)
    for reference in metadata["resolution_refs"]:
        validate_repo_ref(root, reference, "resolution_refs entry")
    invalid_review_refs = [
        reference
        for reference in metadata["review"]["refs"]
        if not review_ref_is_valid(root, reference)
    ]
    if invalid_review_refs:
        fail(f"review refs are invalid: {relative}: {invalid_review_refs}")

    if metadata["status"] == "accepted" and not any(
        reference.startswith(("docs/design/", "schemas/", "tests/"))
        for reference in metadata["resolution_refs"]
    ):
        fail(f"accepted feedback has no authority/test resolution: {relative}")

    if metadata["status"] == "accepted" and metadata["impact"] in HIGH_RISK_IMPACTS:
        if not any(
            is_accepted_adr_ref(root, reference)
            for reference in metadata["resolution_refs"]
        ):
            fail(f"accepted high-risk feedback has no accepted ADR resolution: {relative}")
        review = metadata["review"]
        if review["author"].strip().casefold() == review["reviewer"].strip().casefold():
            fail(f"accepted high-risk feedback reviewer is not independent: {relative}")
        if not any(
            review_ref_is_bound_to_tracking_issue(
                reference, metadata["tracking_issue"]
            )
            for reference in review["refs"]
        ):
            fail(
                "accepted high-risk feedback review is not bound to a comment "
                f"on its tracking issue: {relative}"
            )
    return Record(path=path, metadata=metadata)


def validate_cross_record(records: list[Record]) -> None:
    identifiers = [record.metadata["id"] for record in records]
    if len(identifiers) != len(set(identifiers)):
        fail("design feedback records contain duplicate IDs")
    by_id = {record.metadata["id"]: record for record in records}
    for record in records:
        successor = record.metadata.get("superseded_by")
        if successor is not None:
            if successor == record.metadata["id"] or successor not in by_id:
                fail(
                    f"superseded feedback has no distinct successor: "
                    f"{record.metadata['id']} -> {successor}"
                )


def load_records(root: pathlib.Path) -> list[Record]:
    schema = load_mapping(root / SCHEMA)
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
    except jsonschema.SchemaError as error:
        fail(f"design feedback metadata schema is invalid: {error.message}")
    records = [validate_record(root, path, schema) for path in record_paths(root)]
    validate_cross_record(records)
    return records


def index_table(records: list[Record]) -> list[str]:
    rows = [
        "| ID | Status | Implementation | Scope | Title |",
        "| --- | --- | --- | --- | --- |",
    ]
    for record in sorted(records, key=lambda item: item.metadata["id"]):
        metadata = record.metadata
        scopes = ", ".join(f"`{scope}`" for scope in metadata["scope"])
        rows.append(
            f"| [{metadata['id']}]({record.path.name}) | {metadata['status']} | "
            f"{metadata['implementation_disposition']} | {scopes} | "
            f"{metadata['title']} |"
        )
    return rows


def render_index(records: list[Record]) -> str:
    lines = [
        "# Design feedback index",
        "",
        "Generated by `tools/quality/check_ng_design_feedback.py generate`. Do not edit by hand.",
        "",
    ]
    groups = (
        ("Active", [record for record in records if record.metadata["status"] in ACTIVE_STATUSES]),
        ("Deferred", [record for record in records if record.metadata["status"] == "deferred"]),
        ("Resolved", [record for record in records if record.metadata["status"] in RESOLVED_STATUSES]),
    )
    if not records:
        lines.append("No design feedback records.")
        return "\n".join(lines) + "\n"
    for title, selected in groups:
        lines.extend((f"## {title}", ""))
        if selected:
            lines.extend(index_table(selected))
        else:
            lines.append("None.")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def validate_mental_models(root: pathlib.Path) -> None:
    handbook = root / HANDBOOK
    if not handbook.is_file():
        fail(f"implementation-learning handbook is missing: {HANDBOOK}")
    handbook_text = handbook.read_text(encoding="utf-8")
    models = sorted((root / MENTAL_MODELS).glob("*.md"))
    if not models:
        fail("at least one curated mental model is required")
    for path in models:
        relative = path.relative_to(root)
        text = path.read_text(encoding="utf-8")
        if "> Status: Non-normative explanatory model." not in text:
            fail(f"mental model lacks non-normative banner: {relative}")
        section_contents(text, MENTAL_MODEL_HEADINGS, relative)
        link = path.relative_to(handbook.parent).as_posix()
        if link not in handbook_text:
            fail(f"mental model is not linked from handbook: {relative}")


def validate_issue_template(root: pathlib.Path) -> None:
    template = load_mapping(root / ISSUE_TEMPLATE)
    if template.get("name") != "Design feedback":
        fail("GitHub design-feedback issue template has the wrong name")
    body = template.get("body")
    if not isinstance(body, list):
        fail("GitHub design-feedback issue template body is missing")
    fields = {
        row.get("id"): row
        for row in body
        if isinstance(row, dict) and isinstance(row.get("id"), str)
    }
    required = {
        "source_issue",
        "record_path",
        "scope",
        "observation",
        "mental_model",
        "disposition",
        "alternatives",
        "contract",
    }
    if set(fields) != required:
        fail(
            "GitHub design-feedback issue template fields differ: "
            f"expected={sorted(required)}, actual={sorted(fields)}"
        )
    for identifier in required - {"contract"}:
        if fields[identifier].get("validations", {}).get("required") is not True:
            fail(f"GitHub design-feedback field is not required: {identifier}")
    source_description = fields["source_issue"].get("attributes", {}).get(
        "description", ""
    )
    if "standalone" in source_description.casefold():
        fail("GitHub design-feedback source issue cannot be standalone")
    options = fields["contract"].get("attributes", {}).get("options", [])
    if len(options) != 2 or any(option.get("required") is not True for option in options):
        fail("GitHub design-feedback authority acknowledgements are incomplete")


def validate_documents(root: pathlib.Path, *, check_index: bool = True) -> list[Record]:
    for relative in (SCHEMA, TEMPLATE, ISSUE_TEMPLATE):
        if not (root / relative).is_file():
            fail(f"implementation-learning contract asset is missing: {relative}")
    records = load_records(root)
    validate_mental_models(root)
    validate_issue_template(root)
    if check_index:
        expected = render_index(records)
        if not (root / INDEX).is_file() or (root / INDEX).read_text(encoding="utf-8") != expected:
            fail("design feedback index is stale; run generate")
    return records


def canonical_issue(raw: str) -> str:
    match = ISSUE_PATTERN.fullmatch(raw)
    if match is None:
        fail(f"invalid implementation issue: {raw}")
    return f"#{int(match.group(1))}"


def validate_issue_ready(records: list[Record], issue: str) -> list[str]:
    canonical = canonical_issue(issue)
    linked = [
        record
        for record in records
        if canonical in record.metadata["implementation_issues"]
    ]
    blockers = [
        record.metadata["id"]
        for record in linked
        if record.metadata["implementation_disposition"] == "blocked"
    ]
    if blockers:
        fail(
            f"implementation issue {canonical} has unresolved blocking design feedback: "
            f"{sorted(blockers)}"
        )
    return sorted(record.metadata["id"] for record in linked)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "generate", "issue-ready"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--issue")
    arguments = parser.parse_args()
    root = arguments.root.resolve()
    try:
        if arguments.command == "generate":
            records = validate_documents(root, check_index=False)
            (root / INDEX).write_text(render_index(records), encoding="utf-8")
        else:
            records = validate_documents(root)
            if arguments.command == "issue-ready":
                if arguments.issue is None:
                    fail("issue-ready requires --issue")
                linked = validate_issue_ready(records, arguments.issue)
                print(
                    f"implementation issue {canonical_issue(arguments.issue)} is ready; "
                    f"design-feedback records={linked}"
                )
                return 0
    except (DesignFeedbackError, OSError) as error:
        print(f"design feedback check failed: {error}", file=sys.stderr)
        return 1
    print("design feedback check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
