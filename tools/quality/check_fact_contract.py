#!/usr/bin/env python3
"""Enforce schema-first immutable fact and observation contracts."""

from __future__ import annotations

import pathlib
import re
import sys

import yaml


root = pathlib.Path(sys.argv[1])
contract = yaml.safe_load((root / "schemas/cxxlens_fact_contract.yaml").read_text())
vectors = yaml.safe_load((root / "schemas/cxxlens_fact_conformance.yaml").read_text())
catalog = yaml.safe_load((root / "schemas/cxxlens_public_api_contract.yaml").read_text())
header = (root / "include/cxxlens/facts.hpp").read_text()
internal = (root / "src/facts/fact_contract.hpp").read_text()
validator = (root / "src/facts/fact_contract.cpp").read_text()
failures: list[str] = []

dependencies = {
    fact
    for package in catalog["packages"]
    for api in package["apis"]
    for fact in api.get("requires", {}).get("facts", [])
}
resolvable = set(contract["public_fact_kinds"]) | set(contract["dependency_aliases"])
if contract["public_fact_kinds"] != sorted(contract["public_fact_kinds"]):
    failures.append("public fact kind registry is not canonical")
for kind in contract["public_fact_kinds"]:
    if kind not in header or f'"{kind}"' not in validator:
        failures.append(f"public fact kind is not represented in C++: {kind}")
if dependencies - resolvable:
    failures.append(f"unresolved catalog fact dependencies: {sorted(dependencies - resolvable)}")
for alias, expansion in contract["dependency_aliases"].items():
    if not expansion or set(expansion) - set(contract["public_fact_kinds"]):
        failures.append(f"dependency alias {alias} has non-concrete expansion")
if len(vectors["vectors"]) < 8:
    failures.append("extractor/reducer/store conformance vectors are incomplete")
for expected in ("FACT-NAME-PRETTY-ONLY", "FACT-TYPE-PRETTY-ONLY", "FACT-OBSERVATION-POINTER"):
    if expected not in {vector["id"] for vector in vectors["vectors"]}:
        failures.append(f"negative conformance vector missing: {expected}")
for forbidden in ("clang::", "llvm::", "#include <clang", "#include <llvm"):
    if forbidden in header:
        failures.append(f"public facts header leaks {forbidden}")
if re.search(r"\b(?:ASTContext|Decl|QualType|SourceManager)\s*\*", internal):
    failures.append("observation contract persists a native AST pointer")
for branch in ("qualified-name-only", "pretty-string-only", "native-lifetime-payload",
               "not-canonical-sorted-unique", "required-for-candidate-dispatch"):
    if branch not in validator:
        failures.append(f"independent fact validator branch missing: {branch}")
facts_package = next(package for package in catalog["packages"] if package["id"] == "facts")
for api in facts_package["apis"]:
    if api["declaration"]["status"] != "exact" or api["declaration"]["source"] != "include/cxxlens/facts.hpp":
        failures.append(f"{api['id']} public signature is not frozen")

if failures:
    print("fact contract check failed:\n" + "\n".join(failures), file=sys.stderr)
    raise SystemExit(1)
print("validated fact kinds, dependencies, schemas, identity, provenance and API ownership")
