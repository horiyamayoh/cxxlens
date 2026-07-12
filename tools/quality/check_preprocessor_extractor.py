#!/usr/bin/env python3
"""Enforce Clang 22 source/preprocessor extraction and detached lifetime boundaries."""

from __future__ import annotations

import pathlib
import re
import sys


root = pathlib.Path(sys.argv[1])
source_map = (root / "src/llvm/clang22/source_map_adapter.cpp").read_text()
extractor = (root / "src/llvm/clang22/preprocessor_extractor.cpp").read_text()
frontend = (root / "src/llvm/clang22/frontend_job.cpp").read_text()
test = (root / "tests/adapter/clang22/preprocessor_extractor_test.cpp").read_text()

for marker in (
    "getFileLoc",
    "getLocForEndOfToken",
    "source_range_kind::token",
    "output.spelling",
    "output.expansion",
    "output.macro_stack",
    "source_origin::macro_argument",
    "source_origin::macro_body",
    "source_origin::macro_expansion",
    "isWrittenInScratchSpace",
    "output.read_only = true",
    "runtime::fnv1a_hash_adapter",
    "identity::identity_service",
):
    if marker not in source_map:
        raise SystemExit(f"source map contract marker is missing: {marker}")

for marker in (
    "class callbacks final : public clang::PPCallbacks",
    "InclusionDirective",
    "MacroDefined",
    "MacroUndefined",
    "MacroExpands",
    "conditional.context",
    "include.resolved",
    "include.angled",
    "include.system",
    "macro.argument_index",
    "coverage_state::covered",
    "facts::validate(observation)",
    "std::ranges::sort(observations_",
):
    if marker not in extractor:
        raise SystemExit(f"preprocessor extractor contract marker is missing: {marker}")

for marker in (
    "BeginSourceFileAction",
    "attach_preprocessor_extractor",
    "EndSourceFileAction",
    "batch.observations = std::move(observations)",
    "batch.validate()",
):
    if marker not in frontend:
        raise SystemExit(f"frontend extraction integration marker is missing: {marker}")

if re.search(r'payload\s*\[.*(?:pointer|address|source_manager)', extractor, re.IGNORECASE):
    raise SystemExit("native lifetime data is written to an observation payload")
if "source_origin::macro_expansion" not in test or "!builtin->source->is_directly_editable()" not in test:
    raise SystemExit("macro expansion edit-safety acceptance is missing")
for marker in (
    "macro argument/index mapping was lost",
    "nested macro stack was flattened",
    "angled system include metadata was lost",
    "build variants were collapsed",
    "root/VFS insertion order changed canonical observations",
    "jobs/seed/enqueue order changed preprocessor observations",
    "invalid UTF-8 source was omitted or crashed extraction",
):
    if marker not in test:
        raise SystemExit(f"preprocessor acceptance seam is missing: {marker}")

print("validated source/macro/include normalization, determinism, and detached lifetime boundary")
