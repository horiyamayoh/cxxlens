#!/usr/bin/env python3
"""Validate the detached frontend batch and embedded observation contracts."""

from __future__ import annotations

import copy
import pathlib
import sys

import jsonschema
import yaml


root = pathlib.Path(sys.argv[1])
batch_schema = yaml.safe_load((root / "schemas/cxxlens_frontend_batch.schema.yaml").read_text())
observation_schema = yaml.safe_load((root / "schemas/cxxlens_observation.schema.yaml").read_text())
batch_schema["properties"]["observations"]["items"] = observation_schema
validator = jsonschema.Draft202012Validator(batch_schema)

observation = {
    "schema": "cxxlens.observation.v1",
    "adapter": {"id": "clang22.frontend", "version": "1.0.0", "llvm_major": 22},
    "compile_unit_id": "cu_" + "a" * 64,
    "variant_id": "variant_" + "b" * 64,
    "kind": "declaration",
    "payload": {"version": 1, "fields": {"semantic_key": "decl:main"}},
    "diagnostics": [],
    "coverage_contributions": [],
    "name_identity": {"display_qualified_name": "main", "usr": "c:@F@main#"},
}
batch = {
    "schema": "cxxlens.frontend-batch.v1",
    "adapter": {"id": "clang22.frontend", "version": "1.0.0", "llvm_major": 22},
    "compile_unit_id": "cu_" + "a" * 64,
    "variant_id": "variant_" + "b" * 64,
    "observations": [observation],
    "diagnostics": [
        {
            "id": "clang.diagnostic.100",
            "severity": "warning",
            "file": "src/main.cpp",
            "line": 1,
            "column": 2,
            "message": "fixture warning",
        }
    ],
    "coverage": {"requested": 1, "parsed": 1, "failed": 0, "cancelled": 0},
}
validator.validate(batch)
assert sum(batch["coverage"][key] for key in ("parsed", "failed", "cancelled")) == 1
assert batch["diagnostics"] == sorted(
    batch["diagnostics"], key=lambda row: tuple(row.values())
)

wrong_major = copy.deepcopy(batch)
wrong_major["adapter"]["llvm_major"] = 20
assert list(validator.iter_errors(wrong_major))

native = copy.deepcopy(batch)
native["observations"][0]["payload"]["fields"] = {"clang_ast_pointer": "0x1234"}
assert list(validator.iter_errors(native))

bad_coverage = copy.deepcopy(batch)
bad_coverage["coverage"] = {"requested": 1, "parsed": 1, "failed": 1, "cancelled": 0}
validator.validate(bad_coverage)
assert sum(bad_coverage["coverage"][key] for key in ("parsed", "failed", "cancelled")) != 1

print("validated Clang-free frontend batch and embedded observation schema")
