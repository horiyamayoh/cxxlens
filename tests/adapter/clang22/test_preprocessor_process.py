#!/usr/bin/env python3
"""Validate real Clang 22 source/preprocessor golden and source-span schemas."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys

import jsonschema
import yaml


binary = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
golden = pathlib.Path(sys.argv[3]).read_text()
outputs = [
    subprocess.run([binary, "--emit"], check=True, text=True, capture_output=True).stdout
    for _ in range(3)
]
assert outputs[0] == outputs[1] == outputs[2], "preprocessor process output is unstable"
assert outputs[0] == golden, "preprocessor canonical golden changed"
assert "0x" not in outputs[0] and "clang::" not in outputs[0]

span_output = subprocess.run(
    [binary, "--emit-spans"], check=True, text=True, capture_output=True
).stdout
spans = [json.loads(line) for line in span_output.splitlines()]
schema = yaml.safe_load((root / "schemas/cxxlens_source_span.schema.yaml").read_text())
validator = jsonschema.Draft202012Validator(schema)
for span in spans:
    validator.validate(span)
assert {span["origin"] for span in spans} >= {
    "directly_spelled",
    "generated_file",
    "system_header",
    "macro_argument",
    "macro_body",
    "macro_expansion",
}
assert all(not span["read_only"] for span in spans if span["origin"] == "directly_spelled")
assert all(span["read_only"] for span in spans if span["origin"].startswith("macro_"))
print("validated real Clang 22 source/macro/include golden and source-span schemas")
