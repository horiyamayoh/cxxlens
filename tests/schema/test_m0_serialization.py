#!/usr/bin/env python3
"""Validate M0 canonical documents, schemas, metadata separation, and process stability."""

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


SCHEMAS = {
    "cxxlens.source-span.v1": "cxxlens_source_span.schema.yaml",
    "cxxlens.error.v1": "cxxlens_error.schema.yaml",
    "cxxlens.unresolved.v1": "cxxlens_unresolved.schema.yaml",
    "cxxlens.evidence.v1": "cxxlens_evidence.schema.yaml",
    "cxxlens.coverage.v1": "cxxlens_coverage.schema.yaml",
    "cxxlens.diagnostic.v1": "cxxlens_diagnostic.schema.yaml",
    "cxxlens.finding.v1": "cxxlens_finding.schema.yaml",
}
FORBIDDEN_METADATA = ("timestamp", "elapsed", "pid", "thread", "cache_hit", "/home/")


def run(executable: pathlib.Path, cwd: pathlib.Path, locale: str) -> bytes:
    environment = os.environ.copy()
    environment["LC_ALL"] = locale
    return subprocess.run(
        [executable], cwd=cwd, env=environment, check=True, stdout=subprocess.PIPE
    ).stdout


def validate_summary(value: dict) -> None:
    counts = {key: 0 for key in ("covered", "excluded", "failed", "unresolved", "not_applicable")}
    for row in value["units"]:
        counts[row["state"]] += 1
    if value["summary"] != counts:
        raise ValueError("coverage summary diverges from authoritative rows")
    expected_complete = counts["failed"] == 0 and counts["unresolved"] == 0
    if value["complete"] != expected_complete:
        raise ValueError("coverage complete flag diverges from authoritative rows")


def main() -> int:
    executable = pathlib.Path(sys.argv[1]).resolve()
    root = pathlib.Path(sys.argv[2]).resolve()
    expected_digest = (root / "tests/golden/json/m0_documents_v1.sha256").read_text().strip()
    with tempfile.TemporaryDirectory(prefix="cxxlens-json-a-") as first:
        with tempfile.TemporaryDirectory(prefix="cxxlens-json-b-") as second:
            outputs = {
                run(executable, pathlib.Path(first), "C"),
                run(executable, pathlib.Path(second), "C.UTF-8"),
            }
    if len(outputs) != 1:
        raise ValueError("M0 documents changed across root/locale/process")
    raw = outputs.pop()
    if hashlib.sha256(raw).hexdigest() != expected_digest:
        raise ValueError("M0 canonical document golden digest changed")
    text = raw.decode("utf-8")
    if any(token in text for token in FORBIDDEN_METADATA):
        raise ValueError("operational metadata entered semantic projection")

    documents = [json.loads(line) for line in text.splitlines()]
    if {document["schema"] for document in documents} != set(SCHEMAS):
        raise ValueError("M0 document/schema set mismatch")
    for document in documents:
        if document["semantics_version"] != "1.0.0" or not document["library_version"]:
            raise ValueError("document version axes are missing")
        schema = yaml.safe_load((root / "schemas" / SCHEMAS[document["schema"]]).read_text())
        jsonschema.Draft202012Validator(schema).validate(document)
        canonical = json.dumps(document, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
        if canonical != next(line for line in text.splitlines() if json.loads(line) == document):
            raise ValueError(f"{document['schema']}: non-canonical round-trip")

    coverage = next(document for document in documents if document["schema"] == "cxxlens.coverage.v1")
    validate_summary(coverage)
    tampered = json.loads(json.dumps(coverage))
    tampered["summary"]["covered"] += 1
    try:
        validate_summary(tampered)
    except ValueError:
        pass
    else:
        raise ValueError("tampered summary was accepted")

    malformed = ("{", "[1,", '"\\x"', "{\"a\":1,}")
    if any(_accepts_json(value) for value in malformed):
        raise ValueError("malformed JSON fuzz-smoke input accepted")
    return 0


def _accepts_json(value: str) -> bool:
    try:
        json.loads(value)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return False
    return True


if __name__ == "__main__":
    raise SystemExit(main())
