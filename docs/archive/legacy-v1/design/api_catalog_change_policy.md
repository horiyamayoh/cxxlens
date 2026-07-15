---
cxxlens_document_status: archived
cxxlens_authority: non-normative
cxxlens_replacement: docs/design/cxxlens_next_generation_integrated_design_ja.md
cxxlens_removal_issue: "#72"
---
# API catalog change policy

> [!CAUTION]
> Issue #57 以降、この方針は旧124 API baseline の同一性を保つための移行規則であり、
> 新規 API を追加する authority ではない。次世代 API は新しい relation/registry contract
> に従って定義する。

`schemas/cxxlens_public_api_contract.yaml` is the sole machine-readable authority for public API
IDs, symbols, declarations, state, dependencies, and atomic implementation units. Chapter 40 of the
integrated design points to the generated `api_catalog_inventory.md` projection; hand-maintained
operation summaries are non-authoritative.

## Changes

- API IDs are permanent. A rename or remap adds an explicit record to `migrations`; it is never a
  silent edit.
- `contract_maturity` describes design stability. `implementation_state` changes only with paths to
  implementation and conformance evidence.
- Package `contract.state` is an independent Phase B axis. Draft or unresolved contracts can move
  to candidate only through their #43-#51 owner; only #54 can move candidate to frozen. Exact
  declarations and conformant implementations do not imply this freeze.
- An exact declaration records its authoritative source, canonical signature, and SHA-256
  fingerprint. An unresolved declaration has null signature fields and cannot be `ready`.
- Fact, capability, expression, and API dependencies use their typed registries. Dependency
  expressions expand to sorted unique concrete fact kinds.
- An API family is one indivisible atomic unit. Every API assigned to the same unit has one readiness
  state; partial release is invalid.
- Catalog changes regenerate `api_catalog_inventory.md`, update the migration notes when identity or
  declaration meaning changes, and pass `cxxlens-api-contract-check` and its negative tests.
- Signature, semantics, public ownership, dependencies, schemas, decision tables, or evidence
  changes invalidate convention/ownership fingerprints and any Phase C authorization.

## v1 to v2 reconciliation

All 123 v1 API IDs and symbols are preserved. `maturity` became `contract_maturity`; the new
`implementation_state` identifies only `API-CORE-001` as conformant from repository evidence. The
four former placeholder facts (`selector_dependent`, `recipe_dependent`, `requested_profile`, and
`method_inspection`) are typed dependency expressions. No API ID or symbol migration occurred.
