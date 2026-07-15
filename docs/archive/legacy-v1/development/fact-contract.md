---
cxxlens_document_status: archived
cxxlens_authority: non-normative
cxxlens_replacement: docs/design/cxxlens_next_generation_integrated_design_ja.md
cxxlens_removal_issue: "#72"
---
# Observation and immutable fact contract

Frontend adapters emit versioned, TU/variant-owned observations. Observations may contain local
adapter metadata, diagnostics, and coverage contributions, but cannot contain Clang/LLVM objects,
addresses, pointers, or unversioned opaque payloads. They are valid only for the frontend job.

Reducers convert observations into detached immutable facts. A name uses a USR or the complete
structural fallback tuple; a qualified display name alone is invalid. A type uses structural TypeIR
components; pretty spelling alone is invalid. Contributor compile units and variants are sorted and
unique, and direct inheritance/override rows are distinct from derived transitive closures.

Call facts keep the direct callee, possible callees, receiver static type, dispatch kind, confidence,
guarantee, and evidence as separate fields. The schemas and negative vectors are authoritative:

- `schemas/cxxlens_observation.schema.yaml`
- `schemas/cxxlens_fact.schema.yaml`
- `schemas/cxxlens_fact_contract.yaml`
- `schemas/cxxlens_fact_conformance.yaml`

Run the focused contract with:

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --test-dir build/dev-clang -R fact --output-on-failure
```
