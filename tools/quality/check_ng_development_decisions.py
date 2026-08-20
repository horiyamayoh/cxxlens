#!/usr/bin/env python3
"""Validate the repository development decision and review register."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
REGISTER = pathlib.Path("schemas/cxxlens_ng_development_decision_register.yaml")
SCHEMA = pathlib.Path("schemas/cxxlens_ng_development_decision_register.schema.yaml")
HIGH_RISK = {"contract", "invariant", "security", "compatibility", "irreversible", "resource-bound"}
REVIEW_REF = re.compile(
    r"^https://github\.com/horiyamayoh/cxxlens/issues/[1-9][0-9]*#issuecomment-[1-9][0-9]*$"
)


class DecisionRegisterError(ValueError):
    """A fail-closed development decision register violation."""


class UniqueKeyLoader(yaml.SafeLoader):
    """YAML loader that rejects duplicate mapping keys."""


def _mapping(loader: UniqueKeyLoader, node: yaml.MappingNode, deep: bool = False) -> dict[Any, Any]:
    loader.flatten_mapping(node)
    result: dict[Any, Any] = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in result:
            raise DecisionRegisterError(f"duplicate YAML key: {key}")
        result[key] = loader.construct_object(value_node, deep=deep)
    return result


UniqueKeyLoader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _mapping)


def _load(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = yaml.load(path.read_text(encoding="utf-8"), Loader=UniqueKeyLoader)
    except (OSError, UnicodeError, yaml.YAMLError) as error:
        raise DecisionRegisterError(f"cannot load {path}: {error}") from error
    if not isinstance(value, dict):
        raise DecisionRegisterError(f"expected mapping: {path}")
    return value


def validate(root: pathlib.Path) -> dict[str, Any]:
    register = _load(root / REGISTER)
    schema = _load(root / SCHEMA)
    raw_decisions = register.get("decisions")
    if isinstance(raw_decisions, list):
        raw_identifiers = [
            entry.get("id") for entry in raw_decisions if isinstance(entry, dict)
        ]
        if len(raw_identifiers) != len(set(raw_identifiers)):
            raise DecisionRegisterError("duplicate decision IDs")
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(register)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise DecisionRegisterError(f"decision register schema validation failed: {error.message}") from error

    decisions = register["decisions"]
    identifiers = [entry["id"] for entry in decisions]
    required = {
        "decision.delivery.direct-main",
        "decision.source-closure.dedicated-transport",
        "decision.store.streaming-candidate",
        "decision.sqlite.unified-lifecycle",
        "decision.provider.ng1-after-source-closure-registry",
        "decision.sdk-doctor.capability-boundary",
    }
    if set(identifiers) != required:
        raise DecisionRegisterError("decision inventory differs from the accepted repository set")

    for entry in decisions:
        for reference in entry["authority_refs"]:
            if not (root / reference).is_file():
                raise DecisionRegisterError(f"decision authority does not exist: {entry['id']}:{reference}")
        review = entry["review"]
        if entry["risk"] in HIGH_RISK:
            if review["mode"] != "independent" or review["status"] == "not-required":
                raise DecisionRegisterError(f"high-risk decision lacks independent review: {entry['id']}")
            if review["status"] == "complete":
                if review["reviewer"] in (None, review["author"]):
                    raise DecisionRegisterError(f"reviewer is not independent: {entry['id']}")
                if not review["refs"] or not all(REVIEW_REF.fullmatch(ref) for ref in review["refs"]):
                    raise DecisionRegisterError(f"review reference is not canonical: {entry['id']}")
        if entry["implementation_status"] == "complete" and review["status"] != "complete":
            raise DecisionRegisterError(f"implementation completed before review: {entry['id']}")
        if entry["qualification_status"] == "qualified" and entry["implementation_status"] != "complete":
            raise DecisionRegisterError(f"qualification precedes implementation: {entry['id']}")

    wip_refs = [entry["ref"] for entry in register["preserved_wip"]]
    if len(wip_refs) != len(set(wip_refs)):
        raise DecisionRegisterError("duplicate preserved WIP refs")
    return register


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    args = parser.parse_args()
    try:
        validate(args.root.resolve())
    except DecisionRegisterError as error:
        print(f"development-decision-register: {error}", file=sys.stderr)
        return 1
    print("development-decision-register: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
