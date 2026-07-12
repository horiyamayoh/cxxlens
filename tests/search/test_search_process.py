#!/usr/bin/env python3
"""Public search/explain schema, projection, and process determinism acceptance."""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

import jsonschema
import yaml


def run(
    executable: pathlib.Path,
    order: str,
    jobs: str,
    mode: str,
    cwd: pathlib.Path,
    seed: str,
    backend: str,
) -> str:
    environment = dict(os.environ)
    environment["CXXLENS_TEST_SEED"] = seed
    environment["CXXLENS_SEARCH_BACKEND"] = backend
    return subprocess.run(
        [str(executable), order, jobs, mode],
        cwd=cwd,
        env=environment,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    executable = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="cxxlens-search-root-") as temporary:
        dimensions = [
            (order, jobs, pathlib.Path(cwd), seed, backend)
            for order in ("forward", "reverse")
            for jobs in ("1", "2", "8")
            for cwd in (root, temporary)
            for seed in ("0", "816926")
            for backend in ("memory", "sqlite")
        ]
        json_outputs = {
            run(executable, order, jobs, "json", cwd, seed, backend)
            for order, jobs, cwd, seed, backend in dimensions
        }
        markdown_outputs = {
            run(executable, order, jobs, "markdown", cwd, seed, backend)
            for order, jobs, cwd, seed, backend in dimensions
        }
    if len(json_outputs) != 1 or len(markdown_outputs) != 1:
        raise AssertionError("search projections changed with order, root, seed, jobs, or backend")

    output = json_outputs.pop()
    markdown = markdown_outputs.pop()
    documents = [json.loads(line) for line in output.splitlines()]
    schema_paths = [
        "cxxlens_search_report.schema.yaml",
        "cxxlens_explanation.schema.yaml",
        "cxxlens_agent_task_card.schema.yaml",
    ]
    if len(documents) != len(schema_paths):
        raise AssertionError("search process emitted an unexpected artifact count")
    for document, name in zip(documents, schema_paths, strict=True):
        schema = yaml.safe_load((root / "schemas" / name).read_text())
        jsonschema.Draft202012Validator(schema).validate(document)

    report, explanation, card = documents
    evidence_schema = yaml.safe_load((root / "schemas/cxxlens_evidence.schema.yaml").read_text())
    coverage_schema = yaml.safe_load((root / "schemas/cxxlens_coverage.schema.yaml").read_text())
    unresolved_schema = yaml.safe_load((root / "schemas/cxxlens_unresolved.schema.yaml").read_text())
    source_schema = yaml.safe_load((root / "schemas/cxxlens_source_span.schema.yaml").read_text())
    jsonschema.Draft202012Validator(coverage_schema).validate(report["coverage"])
    for match in report["matches"]:
        jsonschema.Draft202012Validator(evidence_schema).validate(match["evidence"])
        jsonschema.Draft202012Validator(source_schema).validate(match["location"])
        for unresolved in match["unresolved"]:
            jsonschema.Draft202012Validator(unresolved_schema).validate(unresolved)
    for unresolved in report["unresolved"] + explanation["unresolved"]:
        jsonschema.Draft202012Validator(unresolved_schema).validate(unresolved)
    jsonschema.Draft202012Validator(evidence_schema).validate(explanation["evidence"])

    accounting = report["accounting"]
    if accounting["considered"] != sum(
        accounting[key] for key in ("matched", "rejected", "unresolved")
    ):
        raise AssertionError("public report candidate accounting is not balanced")
    if accounting["matched"] != len(report["matches"]):
        raise AssertionError("public report match count differs from authoritative accounting")
    if f"Matches: {len(report['matches'])}" not in markdown:
        raise AssertionError("Markdown is not a projection of the authoritative report")
    if "search.open-world-virtual-target" not in markdown:
        raise AssertionError("Markdown omitted unresolved semantic uncertainty")
    if "cxxlens::search::calls" not in card["api_calls"]:
        raise AssertionError("agent task card omitted the executable API entry point")

    fingerprints = "\n".join(
        hashlib.sha256(value.encode()).hexdigest()
        for value in (*output.splitlines(), markdown)
    ) + "\n"
    golden = (root / "tests/golden/search/canonical_v1.sha256").read_text()
    if fingerprints != golden:
        raise AssertionError("public search artifacts differ from the canonical golden")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
