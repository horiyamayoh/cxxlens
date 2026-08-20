#!/usr/bin/env python3
"""Validate the proposed dedicated source-closure transport authority."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONTRACT = pathlib.Path("schemas/cxxlens_ng_source_closure_transport.yaml")
SCHEMA = pathlib.Path("schemas/cxxlens_ng_source_closure_transport.schema.yaml")
PROTOCOL = pathlib.Path("schemas/cxxlens_ng_provider_protocol.yaml")
REQUEST = pathlib.Path("schemas/cxxlens_ng_clang22_materialization_request_v2_2.schema.yaml")
TASK = pathlib.Path("schemas/cxxlens_ng_provider_task_v4.schema.yaml")
ADR = pathlib.Path("docs/design/adr/0102-dedicated-source-closure-transport.md")
REVIEW_REF = re.compile(
    r"^https://github\.com/horiyamayoh/cxxlens/issues/261#issuecomment-[1-9][0-9]*$"
)


class SourceClosureTransportError(ValueError):
    """A fail-closed source-closure transport contract violation."""


def load(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise SourceClosureTransportError(f"expected mapping: {path}")
    return value


def validate(root: pathlib.Path) -> dict[str, Any]:
    contract = load(root / CONTRACT)
    schema = load(root / SCHEMA)
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(contract)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise SourceClosureTransportError(
            f"transport schema validation failed: {error.message}"
        ) from error

    protocol = load(root / PROTOCOL)
    request = load(root / REQUEST)
    task = load(root / TASK)
    adr = (root / ADR).read_text(encoding="utf-8")

    legacy_ids = [entry["id"] for entry in protocol["message_types"]["registry"]]
    proposed = contract["message_registry"]["proposed"]
    proposed_ids = [entry["id"] for entry in proposed]
    if len(legacy_ids) != len(set(legacy_ids)) or len(proposed_ids) != len(set(proposed_ids)):
        raise SourceClosureTransportError("duplicate message ID")
    if set(legacy_ids).intersection(proposed_ids):
        raise SourceClosureTransportError("proposed message ID collides with accepted protocol")
    if proposed_ids != list(range(24, 30)):
        raise SourceClosureTransportError("source-closure message IDs must be contiguous 24 through 29")
    if contract["message_registry"]["preserved"] != {"heartbeat": 23}:
        raise SourceClosureTransportError("heartbeat 23 is not preserved")

    versions = contract["versions"]
    if versions != {
        "provider_protocol": {"legacy": 1.1, "proposed": 1.2, "downgrade": "reject"},
        "request": {"legacy": 2.1, "proposed": 2.2},
        "task": {
            "legacy": "cxxlens.clang22.task.v3",
            "proposed": "cxxlens.clang22.task.v4",
        },
    }:
        raise SourceClosureTransportError("version or downgrade contract drift")
    if request["properties"]["required_features"].get("const") != [
        "task-input-chunks-v1",
        "task-source-closure-v1",
    ]:
        raise SourceClosureTransportError("request 2.2 omits exact source-closure capability")
    request_text = (root / REQUEST).read_text(encoding="utf-8")
    task_text = (root / TASK).read_text(encoding="utf-8")
    if "content_base64" in request_text or "content_base64" in task_text:
        raise SourceClosureTransportError("request/task embeds closure blob bytes")

    success = contract["state_machine"]["success_path"]
    required_states = [
        "task-v4-sealed",
        "manifest-open",
        "manifest-streaming",
        "manifest-validated",
        "blob-open",
        "blob-streaming",
        "blob-sealed",
        "closure-sealed",
        "closure-acknowledged",
        "task-accepted",
    ]
    if success != required_states:
        raise SourceClosureTransportError("source-closure success state machine drift")
    if contract["cache"] != {
        "cross_task_v1": "forbidden",
        "transfer": "complete-per-task",
        "future_activation": "separate-accepted-capability-and-adr",
    }:
        raise SourceClosureTransportError("cross-task cache was activated")
    if contract["limits"]["maximum_resident_transport_bytes"] > 1310720:
        raise SourceClosureTransportError("resident transport bound exceeds 1.25 MiB")

    maturity = contract["maturity"]
    review = contract["authority"]["review"]
    adr_status = "Accepted" if "- Status: Accepted" in adr else "Proposed"
    if maturity == "proposed":
        if adr_status != "Proposed" or review != {
            "status": "required",
            "reviewer": None,
            "ref": None,
            "exact_main_commit": None,
        }:
            raise SourceClosureTransportError("proposed authority has premature acceptance")
    else:
        if adr_status != "Accepted" or review["status"] != "complete":
            raise SourceClosureTransportError("accepted authority lacks completed review")
        if not isinstance(review["reviewer"], str) or not review["reviewer"]:
            raise SourceClosureTransportError("accepted authority lacks reviewer")
        if not isinstance(review["ref"], str) or not REVIEW_REF.fullmatch(review["ref"]):
            raise SourceClosureTransportError("accepted authority lacks canonical review reference")
        if not isinstance(review["exact_main_commit"], str) or not re.fullmatch(
            r"[0-9a-f]{40}", review["exact_main_commit"]
        ):
            raise SourceClosureTransportError("accepted authority lacks exact main commit")
    return contract


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    arguments = parser.parse_args()
    try:
        validate(arguments.root.resolve())
    except (OSError, SourceClosureTransportError) as error:
        print(f"source-closure-transport: {error}", file=sys.stderr)
        return 1
    print("source-closure-transport: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
