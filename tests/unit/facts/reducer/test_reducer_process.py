#!/usr/bin/env python3
from __future__ import annotations

import json
import pathlib
import subprocess
import sys

import jsonschema
import yaml


binary = pathlib.Path(sys.argv[1])
golden = pathlib.Path(sys.argv[2])
schema = pathlib.Path(sys.argv[3])
outputs = [
    subprocess.run([binary, "--emit"], check=True, text=True, capture_output=True).stdout
    for _ in range(5)
]
if len(set(outputs)) != 1:
    raise SystemExit("reducer output changed across process runs")
if outputs[0] != golden.read_text(encoding="utf-8"):
    raise SystemExit("reducer output does not match golden")
document = json.loads(outputs[0])
jsonschema.Draft202012Validator(yaml.safe_load(schema.read_text(encoding="utf-8"))).validate(
    document
)
if document["schema"] != "cxxlens.reduction-trace.v1":
    raise SystemExit("reducer trace schema version is missing")
if len(document["facts"]) != 12 or len(document["trace"]) != 12:
    raise SystemExit("reducer golden lost an M1 fact-kind fixture")
coverage = document["coverage"]
if coverage["requested"] != sum(
    coverage[key] for key in ("covered", "excluded", "failed", "unresolved", "not_applicable")
):
    raise SystemExit("reducer coverage equation is unbalanced")
