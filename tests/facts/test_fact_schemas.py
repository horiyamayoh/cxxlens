#!/usr/bin/env python3
"""Validate fact schemas, conformance vectors, and process determinism."""

from __future__ import annotations

import json
import pathlib
import re
import subprocess
import sys

import jsonschema
import yaml


binary = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
profile_schema = yaml.safe_load((root / "schemas/cxxlens_fact_profile.schema.yaml").read_text())
outputs = [subprocess.run([binary, "--emit"], check=True, text=True, capture_output=True).stdout for _ in range(3)]
assert outputs[0] == outputs[1] == outputs[2]
jsonschema.validate(json.loads(outputs[0]), profile_schema)

observation_schema = yaml.safe_load((root / "schemas/cxxlens_observation.schema.yaml").read_text())
base_observation = {
    "schema": "cxxlens.observation.v1",
    "adapter": {"id": "clang22.semantic", "version": "1.0.0", "llvm_major": 22},
    "compile_unit_id": "cu_" + "a" * 64,
    "variant_id": "variant_" + "b" * 64,
    "kind": "symbol",
    "payload": {"version": 1, "fields": {"semantic_key": "symbol-a"}},
    "diagnostics": [],
    "coverage_contributions": [],
    "name_identity": {"display_qualified_name": "ns::f", "usr": "c:@N@ns@F@f#"},
}
jsonschema.validate(base_observation, observation_schema)
for forbidden in ("clang_ast_pointer", "llvm_address", "source_manager"):
    invalid = json.loads(json.dumps(base_observation))
    invalid["payload"]["fields"] = {forbidden: "0x1234"}
    try:
        jsonschema.validate(invalid, observation_schema)
    except jsonschema.ValidationError:
        pass
    else:
        raise AssertionError(f"native payload accepted: {forbidden}")
native_value = json.loads(json.dumps(base_observation))
native_value["payload"]["fields"] = {"semantic_key": {"nested": "clang::Decl* 0x12345678"}}
try:
    jsonschema.validate(native_value, observation_schema)
except jsonschema.ValidationError:
    pass
else:
    raise AssertionError("nested native pointer value accepted")

pretty_name = json.loads(json.dumps(base_observation))
pretty_name["name_identity"] = {"display_qualified_name": "ns::f"}
try:
    jsonschema.validate(pretty_name, observation_schema)
except jsonschema.ValidationError:
    pass
else:
    raise AssertionError("qualified-name-only identity accepted")

contract = yaml.safe_load((root / "schemas/cxxlens_fact_contract.yaml").read_text())
vectors = yaml.safe_load((root / "schemas/cxxlens_fact_conformance.yaml").read_text())
contract_schema = yaml.safe_load((root / "schemas/cxxlens_fact_contract.schema.yaml").read_text())
vector_schema = yaml.safe_load((root / "schemas/cxxlens_fact_conformance.schema.yaml").read_text())
jsonschema.validate(contract, contract_schema)
jsonschema.validate(vectors, vector_schema)
assert len(vectors["vectors"]) >= 8
assert json.loads(outputs[0])["kinds"] == contract["profiles"]["semantic_search"]
assert contract["collections"]["duplicates"] == "reject"
assert contract["relations"]["inheritance"] == "direct_edge_only"
assert contract["relations"]["override"] == "direct_edge_only"

catalog = yaml.safe_load((root / "schemas/cxxlens_public_api_contract.yaml").read_text())
dependencies = {
    fact
    for package in catalog["packages"]
    for api in package["apis"]
    for fact in api.get("requires", {}).get("facts", [])
}
resolvable = set(contract["public_fact_kinds"]) | set(contract["dependency_aliases"])
assert dependencies <= resolvable, sorted(dependencies - resolvable)
fact_schema = yaml.safe_load((root / "schemas/cxxlens_fact.schema.yaml").read_text())
assert "adapter" not in fact_schema["properties"]
assert "diagnostics" not in fact_schema["properties"]
fact_document = {
    "schema": "cxxlens.fact.v1",
    "id": "fact_" + "c" * 64,
    "kind": "call",
    "stable_key": "call:src/main.cpp:1",
    "provenance": {
        "compile_units": ["cu_" + "a" * 64],
        "variants": ["variant_" + "b" * 64],
        "extractor_id": "clang22.call",
        "extractor_version": "1.0.0",
    },
    "payload": {"version": 1, "fields": {}},
    "call": {
        "kind": "virtual_member",
        "direct_callee": "symbol_" + "d" * 64,
        "possible_callees": ["symbol_" + "e" * 64],
        "receiver_static_type": "type_" + "f" * 64,
        "dispatch": "virtual_candidate_set",
        "confidence": "high",
        "guarantee": "sound_over_approximation",
        "evidence": {"items": [{"kind": "call_resolution"}]},
    },
}
jsonschema.validate(fact_document, fact_schema)
assert set(fact_document["call"]) == set(fact_schema["properties"]["call"]["required"])
assert not re.search(r"clang::|llvm::|\b(?:AST|Decl|QualType)\s*\*", (root / "include/cxxlens/facts.hpp").read_text())
print("validated fact schemas, identity negatives, dependency registry, and determinism")
