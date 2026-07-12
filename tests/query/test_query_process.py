#!/usr/bin/env python3
"""M2 query schema, golden, order, root, seed, and process determinism acceptance."""

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
    cwd: pathlib.Path,
    seed: str,
    jobs: str,
    backend: str,
) -> str:
    environment = dict(os.environ)
    environment["CXXLENS_TEST_SEED"] = seed
    environment["CXXLENS_QUERY_BACKEND"] = backend
    return subprocess.run(
        [str(executable), order, jobs],
        cwd=cwd,
        env=environment,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    executable = pathlib.Path(sys.argv[2]).resolve()
    schemas = [
        yaml.safe_load((root / "schemas/cxxlens_query_plan.schema.yaml").read_text()),
        yaml.safe_load(
            (root / "schemas/cxxlens_virtual_candidate_resolution.schema.yaml").read_text()
        ),
        yaml.safe_load((root / "schemas/cxxlens_query_trace.schema.yaml").read_text()),
        yaml.safe_load((root / "schemas/cxxlens_query_execution.schema.yaml").read_text()),
    ]
    with tempfile.TemporaryDirectory(prefix="cxxlens-query-root-") as temporary:
        outputs = {
            run(executable, order, pathlib.Path(cwd), seed, jobs, backend)
            for order in ("forward", "reverse")
            for cwd in (root, temporary)
            for seed in ("0", "1", "816926")
            for jobs in ("1", "2", "8")
            for backend in ("memory", "sqlite")
        }
    if len(outputs) != 1:
        raise AssertionError("query artifacts changed with order, root, seed, or process")
    output = outputs.pop()
    documents = [json.loads(line) for line in output.splitlines()]
    if len(documents) != len(schemas):
        raise AssertionError("query process emitted an unexpected artifact count")
    for document, schema in zip(documents, schemas, strict=True):
        jsonschema.Draft202012Validator(schema).validate(document)
    evidence_schema = yaml.safe_load(
        (root / "schemas/cxxlens_evidence.schema.yaml").read_text()
    )
    jsonschema.Draft202012Validator(evidence_schema).validate(documents[1]["evidence"])
    coverage_schema = yaml.safe_load(
        (root / "schemas/cxxlens_coverage.schema.yaml").read_text()
    )
    jsonschema.Draft202012Validator(coverage_schema).validate(documents[3]["coverage"])
    unresolved_schema = yaml.safe_load(
        (root / "schemas/cxxlens_unresolved.schema.yaml").read_text()
    )
    unresolved_validator = jsonschema.Draft202012Validator(unresolved_schema)
    for unresolved in documents[3]["unresolved"]:
        unresolved_validator.validate(unresolved)
    if documents[3]["trace"] != documents[2]:
        raise AssertionError("execution result changed its authoritative trace")
    for match in documents[3]["matches"]:
        jsonschema.Draft202012Validator(evidence_schema).validate(match["evidence"])
        for unresolved in match["unresolved"]:
            unresolved_validator.validate(unresolved)
    fingerprints = "\n".join(
        hashlib.sha256(line.encode()).hexdigest() for line in output.splitlines()
    ) + "\n"
    golden = (root / "tests/golden/query/canonical_v1.sha256").read_text()
    if fingerprints != golden:
        raise AssertionError("query artifacts differ from the M2 canonical golden")
    accounting = documents[2]["accounting"]
    if accounting["considered"] != (
        accounting["matched"] + accounting["rejected"] + accounting["unresolved"]
    ):
        raise AssertionError("query trace candidate accounting is not balanced")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
