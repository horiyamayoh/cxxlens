#!/usr/bin/env python3
"""Enforce structural semantic extraction and the no-native-lifetime boundary."""

from __future__ import annotations

import pathlib
import re
import sys


root = pathlib.Path(sys.argv[1])
symbols = (root / "src/llvm/clang22/symbol_type_extractor.cpp").read_text()
relations = (root / "src/llvm/clang22/call_relation_extractor.cpp").read_text()
header = (root / "src/llvm/clang22/symbol_type_extractor.hpp").read_text()
frontend = (root / "src/llvm/clang22/frontend_job.cpp").read_text()
test = (root / "tests/adapter/clang22/semantic_extractor_test.cpp").read_text()

for marker in (
    "generateUSRForDecl",
    "getCanonicalDecl",
    "semantic_owner",
    "signature_structure",
    "canonical_structure",
    "clang::PointerType",
    "clang::ReferenceType",
    "clang::FunctionProtoType",
    "clang::TemplateTypeParmType",
    "identity::identity_service",
    "declaration->bases()",
    "overridden_methods",
    "inheritance.direct",
    "override.direct",
    "facts::validate(observation)",
):
    if marker not in symbols:
        raise SystemExit(f"symbol/type/relation contract marker is missing: {marker}")

for marker in (
    "DeclRefExpr",
    "MemberExpr",
    "getDirectCallee",
    "getMethodDecl",
    "getImplicitObjectArgument",
    "static_member_target",
    "dependent-call-target",
    "indirect-call-target",
    "call.possible_callees",
    "coverage_state::unresolved",
):
    if marker not in relations:
        raise SystemExit(f"call/reference contract marker is missing: {marker}")

for marker in (
    "make_semantic_extractor",
    "semantic_->consume(context)",
    "semantic_->take()",
    "batch.validate()",
):
    if marker not in frontend:
        raise SystemExit(f"frontend semantic integration marker is missing: {marker}")

if "#include <clang/" in header or "#include <llvm/" in header:
    raise SystemExit("semantic extractor interface exposes native headers")
if re.search(r"std::(?:vector|optional|unique_ptr)<\s*clang::", header):
    raise SystemExit("semantic extractor detached values store native objects")
if re.search(r'payload\.emplace\("[^\"]*(?:pointer|address|qual_type|decl_ptr)',
             symbols + relations, re.IGNORECASE):
    raise SystemExit("semantic observation payload key exposes native lifetime terminology")
if "canonical_structure = input.getAsString" in symbols:
    raise SystemExit("pretty type spelling is used as canonical TypeIR")

for marker in (
    "same unqualified name collapsed semantic identity",
    "dependent overload set was first-selected or omitted",
    "pointer TypeIR relied on display spelling",
    "required call classifications were not emitted distinctly",
    "invented transitive inheritance row was emitted",
    "variant-dependent direct targets were first-wins collapsed",
    "jobs/seed/TU/variant order changed semantic observations",
    "semantic parse failure lost partial observations/diagnostics/coverage",
    "root/VFS/TU/variant ordering changed canonical semantic observations",
):
    if marker not in test:
        raise SystemExit(f"semantic acceptance seam is missing: {marker}")

print("validated structural symbol/type/call/relation extraction and detached lifetime boundary")
