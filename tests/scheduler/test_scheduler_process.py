#!/usr/bin/env python3
"""Validate scheduler trace schema, golden bytes, and process determinism."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys

import jsonschema
import yaml


binary = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
golden = pathlib.Path(sys.argv[3]).read_text().strip()
outputs: list[str] = []
for repeat in range(2):
    for jobs in (1, 2, 8):
        for seed in (0, 1, 816926):
            order = "reverse" if (repeat + seed) % 2 else "forward"
            completed = subprocess.run(
                [binary, "--emit", str(jobs), str(seed), order],
                check=True,
                text=True,
                capture_output=True,
            )
            outputs.append(completed.stdout.strip())

assert outputs and all(value == outputs[0] for value in outputs), (
    "scheduler output changed with jobs/seed/order/process"
)
assert outputs[0] == golden, "scheduler canonical golden changed"
document = json.loads(outputs[0])
schema = yaml.safe_load((root / "schemas/cxxlens_scheduler_trace.schema.yaml").read_text())
jsonschema.Draft202012Validator(schema).validate(document)
coverage = document["coverage"]
assert coverage["requested"] == sum(
    coverage[key]
    for key in (
        "succeeded",
        "failed",
        "cancelled",
        "deadline_exceeded",
        "budget_exhausted",
        "output_limited",
        "dependency_failed",
    )
)
assert document["tasks"] == sorted(document["tasks"], key=lambda row: row["task_key"])
for task in document["tasks"]:
    frontend = task["frontend_coverage"]
    assert frontend["requested"] == sum(
        frontend[key] for key in ("parsed", "failed", "cancelled")
    )
assert document["trace"] == sorted(
    document["trace"], key=lambda row: (row["task_key"], row["event"], row["detail"])
)
print("validated scheduler schema/golden and jobs/seed/order/process determinism")
