#!/usr/bin/env python3
"""Enforce the single deterministic cross-TU and variant reduction boundary."""

from __future__ import annotations

import pathlib
import re
import sys


root = pathlib.Path(sys.argv[1])
source = (root / "src/facts/reducer.cpp").read_text()
header = (root / "src/facts/reducer.hpp").read_text()
test = (root / "tests/unit/facts/reducer/reducer_test.cpp").read_text()

for marker in (
    "std::map<std::string, std::vector<observation_ref>> groups",
    "domain_payload",
    "group_key",
    "same_variant_conflict",
    "definition_source_conflict",
    "reduction_decision::variant_split",
    "reduction_decision::conflict",
    "conflict-has-arbitrary-winner",
    "sort_unique",
    "identity::identity_service",
    "cxxlens.fact-id.v1",
    "absolute-root-in-observation",
    "operational_key",
    "coverage.validate()",
    "facts::validate(fact)",
):
    if marker not in source:
        raise SystemExit(f"fact reducer contract marker is missing: {marker}")

if "unordered_" in source or "unordered_" in header:
    raise SystemExit("unordered iteration is present in reducer state")
if re.search(r"(?:front|back)\(\).*winner|first-wins|last-wins", source, re.IGNORECASE):
    raise SystemExit("reducer contains a winner-selection shortcut")
if "clang::" in header or "llvm::" in header:
    raise SystemExit("reducer stores a native frontend object")

for marker in (
    "permutation/partition/completion order or retry changed reduction",
    "equivalent TU contributors were not sorted/unique",
    "qualified-name-equal semantic identities merged",
    "variant divergence was first-win collapsed",
    "authoritative conflict selected a winner",
    "ODR-like definition conflict silently chose a source",
    "diagnostic prose changed fact identity",
    "absolute checkout root entered fact identity",
    "invalid observation was repaired by reducer",
):
    if marker not in test:
        raise SystemExit(f"fact reducer acceptance seam is missing: {marker}")

print("validated deterministic reducer grouping, conflicts, variants, provenance, and coverage")
