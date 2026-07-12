#!/usr/bin/env python3
"""Selector schema, registry, and process/root/order determinism acceptance."""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import tempfile

import jsonschema
import yaml


def run(executable: pathlib.Path, argument: str, cwd: pathlib.Path, seed: str) -> str:
    environment = dict(os.environ)
    environment["CXXLENS_TEST_SEED"] = seed
    return subprocess.run(
        [str(executable), argument],
        cwd=cwd,
        env=environment,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    executable = pathlib.Path(sys.argv[2]).resolve()
    schema = yaml.safe_load((root / "schemas/cxxlens_selector.schema.yaml").read_text())
    reasons = yaml.safe_load((root / "schemas/cxxlens_selector_reason_codes.yaml").read_text())

    with tempfile.TemporaryDirectory(prefix="cxxlens-selector-root-") as temporary:
        outputs = {
            run(executable, order, pathlib.Path(cwd), seed)
            for order in ("forward", "reverse")
            for cwd in (root, temporary)
            for seed in ("0", "1", "816926")
        }
    if len(outputs) != 1:
        raise AssertionError("selector JSON changed with insertion order, root, seed, or process")
    output = outputs.pop()
    golden = (root / "tests/golden/selectors/m2_v1.jsonl").read_text()
    if output != golden:
        raise AssertionError("selector canonical JSON differs from the M2 golden")
    documents = [json.loads(line) for line in output.splitlines()]
    for document in documents:
        jsonschema.Draft202012Validator(schema).validate(document)
        if document["normalization_version"] != reasons["normalization_version"]:
            raise AssertionError("normalization version registry mismatch")

    declared = {(row["name"], row["reason_code"]) for row in reasons["predicates"]}

    def visit(expression: dict) -> None:
        if expression["op"] == "predicate":
            if (expression["name"], expression["reason_code"]) not in declared:
                raise AssertionError(f"undeclared predicate reason: {expression['name']}")
            for operand in expression["operands"]:
                visit(operand)
        elif expression["op"] in {"all", "any"}:
            for operand in expression["operands"]:
                visit(operand)
        elif expression["op"] == "negate":
            visit(expression["operand"])

    for document in documents:
        visit(document["expression"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
