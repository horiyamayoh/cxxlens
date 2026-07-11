#!/usr/bin/env python3
"""Validate configuration schema, canonical bytes, and root/locale determinism."""

from __future__ import annotations

import hashlib
import json
import locale
import pathlib
import subprocess
import sys
import tempfile

import jsonschema
import yaml


DOCUMENT_A = """schema: cxxlens.config.v1
workspace:
  root: .
  generated_code: {patterns: [z/**, a/**], default: exclude}
execution: {memory_budget_mb: 512, per_tu_timeout_seconds: 30}
output: {deterministic: true, path_style: project_relative}
profiles:
  ci:
    execution: {memory_budget_mb: 768}
"""

DOCUMENT_B = """profiles:
  ci:
    execution:
      memory_budget_mb: 768
output: {path_style: project_relative, deterministic: true}
execution:
  per_tu_timeout_seconds: 30
  memory_budget_mb: 512
workspace:
  generated_code: {default: exclude, patterns: [z/**, a/**]}
  root: .
schema: cxxlens.config.v1
"""


def run(executable: pathlib.Path, root: pathlib.Path, document: str, locale_name: str) -> bytes:
    root.mkdir(parents=True)
    config = root / "config.yaml"
    config.write_text(document, encoding="utf-8")
    environment = {"LC_ALL": locale_name, "LANG": locale_name}
    completed = subprocess.run(
        [str(executable), str(config)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
    )
    return completed.stdout


def main() -> int:
    executable = pathlib.Path(sys.argv[1])
    root = pathlib.Path(sys.argv[2])
    input_schema = yaml.safe_load((root / "schemas/cxxlens.config.schema.yaml").read_text())
    resolved_schema = yaml.safe_load(
        (root / "schemas/cxxlens.config.resolved.schema.yaml").read_text()
    )
    explain_schema = yaml.safe_load(
        (root / "schemas/cxxlens.config.explain.schema.yaml").read_text()
    )
    jsonschema.validate(yaml.safe_load(DOCUMENT_A), input_schema)
    jsonschema.validate(yaml.safe_load(DOCUMENT_B), input_schema)

    locales = ["C"]
    try:
        locale.setlocale(locale.LC_ALL, "C.UTF-8")
        locales.append("C.UTF-8")
    except locale.Error:
        pass
    with tempfile.TemporaryDirectory() as directory:
        temporary = pathlib.Path(directory)
        outputs = []
        for index, locale_name in enumerate(locales):
            outputs.append(run(executable, temporary / f"a-{index}", DOCUMENT_A, locale_name))
            outputs.append(run(executable, temporary / f"b-{index}", DOCUMENT_B, locale_name))
    if any(output != outputs[0] for output in outputs[1:]):
        raise AssertionError("map order, checkout root, or locale changed configuration bytes")

    lines = outputs[0].decode("utf-8").splitlines()
    if len(lines) != 2:
        raise AssertionError("configuration process emitted an unexpected document count")
    resolved, explanation = map(json.loads, lines)
    jsonschema.validate(resolved, resolved_schema)
    jsonschema.validate(explanation, explain_schema)
    for document, original in ((resolved, lines[0]), (explanation, lines[1])):
        canonical = json.dumps(document, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        if canonical != original:
            raise AssertionError("configuration output is not canonical JSON")
    forbidden = ("elapsed", "timestamp", "pid", "thread", str(temporary))
    if any(token in outputs[0].decode("utf-8") for token in forbidden):
        raise AssertionError("operational or absolute-root metadata leaked")

    digest = hashlib.sha256(outputs[0]).hexdigest()
    expected = (root / "tests/golden/config/resolved_v1.sha256").read_text().strip()
    if digest != expected:
        raise AssertionError(f"configuration golden mismatch: {digest} != {expected}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
