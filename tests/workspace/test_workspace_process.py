#!/usr/bin/env python3
"""Validate workspace schemas and repeat-process determinism."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys

import jsonschema
import yaml


binary = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
documents: list[list[dict]] = []
for _ in range(3):
    completed = subprocess.run([binary, "--emit"], check=True, text=True, capture_output=True)
    documents.append([json.loads(line) for line in completed.stdout.splitlines()])

assert documents[0] == documents[1] == documents[2], "workspace process output is unstable"
scope_schema = yaml.safe_load((root / "schemas/cxxlens_analysis_scope.schema.yaml").read_text())
context_schema = yaml.safe_load((root / "schemas/cxxlens_workspace_context.schema.yaml").read_text())
jsonschema.validate(documents[0][0], scope_schema)
jsonschema.validate(documents[0][1], context_schema)
assert documents[0][1]["compatible"] is False
assert documents[0][1]["stale_inputs"] == ["compilation_database", "configuration", "src/main.cpp"]
assert documents[0][1]["path_mapping"]["policy"] == "project-relative-v2"
assert documents[0][1]["path_mapping"]["project_root_origin"] == "explicit"
assert documents[0][1]["path_mapping"]["external_source_policy"] == "reject"
assert len(documents[0][1]["units"]) == 2
print("validated workspace schemas and repeat-process determinism")
