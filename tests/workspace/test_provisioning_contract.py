#!/usr/bin/env python3
"""Validate incremental provisioning projections and normative contract."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys

import jsonschema
import yaml


binary = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
documents = subprocess.run(
    [str(binary), "--emit"], check=True, capture_output=True, text=True
).stdout.splitlines()
if len(documents) != 3:
    raise SystemExit("provisioning emitter did not produce trace/capabilities/doctor")

schemas = (
    "cxxlens_provisioning_trace.schema.yaml",
    "cxxlens_capabilities.schema.yaml",
    "cxxlens_workspace_doctor.schema.yaml",
)
for document, schema_name in zip(documents, schemas, strict=True):
    schema = yaml.safe_load((root / "schemas" / schema_name).read_text())
    jsonschema.Draft202012Validator.check_schema(schema)
    jsonschema.Draft202012Validator(schema).validate(json.loads(document))

for schema_name in (
    "cxxlens_fact_requirement.schema.yaml",
    "cxxlens_coverage_delta.schema.yaml",
):
    jsonschema.Draft202012Validator.check_schema(
        yaml.safe_load((root / "schemas" / schema_name).read_text())
    )

contract = yaml.safe_load(
    (root / "schemas/cxxlens_incremental_provisioning_contract.yaml").read_text()
)
assert contract["incremental"]["warm_equivalent_request_schedules"] == 0
assert contract["transaction"]["invalid_batch_commits"] is False
assert contract["coverage_accounting"]["terminal_rows_are_authoritative"] is True
print("validated provisioning trace, capabilities, doctor, and incremental contract")
