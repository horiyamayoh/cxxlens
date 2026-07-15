---
cxxlens_document_status: archived
cxxlens_authority: non-normative
cxxlens_replacement: docs/design/cxxlens_next_generation_integrated_design_ja.md
cxxlens_removal_issue: "#72"
---
# Issue #45 select・search・explain package Contract Candidate

## Authority and scope

This document and `schemas/cxxlens_package_contract_candidates.yaml#issue-45` are the normative
Phase B candidate for 25 APIs: `select` 11, `search` 10, and `explain` 4. The baseline remains 14
conformant/complete and 11 unimplemented/blocked. Candidate fragments are non-installed; #53 owns
stable-header integration and #54 alone owns freezing.

## Typed selector algebra

Selectors are immutable shared values over a normalized typed expression tree, not AST matcher
wrappers. Copy/move shares immutable nodes; every builder returns a new value. Domains are `file`,
`symbol`, `type`, `expression`, `reference`, `call`, `conversion`, `include`, and `macro`.

| Decision | Contract |
|---|---|
| Constants/composition | An unconstrained selector is typed true. Empty `any` is false, empty `all` is true. Commutative operands sort/unique, constants fold, and double negation disappears. |
| Type safety | Operators and nested operands have one declared domain. Domain mismatch is `select.type-mismatch`; there is no coercion by field/name coincidence. |
| Names | v1 supports exact, qualified exact, unqualified exact, and glob. Semantic resolution establishes identity; a name/pretty type alone never does. Regex is explicitly unsupported in v1 and fails `select.invalid-expression`. |
| Encoding/pattern | Inputs are UTF-8 scalar strings. Invalid UTF-8/glob syntax fails at builder/parse validation; no locale or case-fold fallback exists. |
| Macro/implicit/template | Policies are explicit enum predicates and enter canonical JSON. Missing origin/instantiation data becomes unresolved, not a negative match. |
| Variants/dispatch | Variant and dispatch policies are explicit. Divergence is per-variant rows, ambiguity, or rejection according to policy; never first-wins. |
| Type erasure | `semantic(typed)` preserves domain and canonical normalized bytes. Equality/identity is canonical JSON plus schema/normalization version. |
| Serialization | `from_json` accepts exact v1 (or declared v0 migration), rejects unknown schema/operator/value/field, and round-trips to one canonical v1 form. |

`requirements()` is the deterministic union of every reachable branch and nested selector:
concrete fact kinds sorted by the fact registry, maximum required precision, and sorted unique
capability IDs. Negation never removes a requirement. Defaults and unsupported predicates remain
explicit. The selector reason-code registry owns one stable code per predicate and outcome.

## Search snapshot and options

Every search binds selector normalization, exact requirements, one workspace catalog version, one
immutable fact snapshot, scope, variants, and budgets before execution. It may call `ensure()` only
for the computed missing requirement delta; a warm complete snapshot does zero parsing. All rows in
one report come from the same committed snapshot. User source is never mutated.

`search_options` does not duplicate selector macro/implicit/template policy. Ordering is fixed by
contract rather than caller-configurable. Refinement is required only for candidates that facts
cannot decide and cannot be disabled into a weaker silent result. `execution_context` owns all
candidate/refinement/result/output budgets. `result_limit` is applied after canonical dedup/order;
coverage and accounting still describe the full requested universe and truncation creates typed
unresolved plus a downgraded guarantee.

## Result rows, open world, and coverage

| Outcome | Authoritative representation |
|---|---|
| complete zero match | Empty `matches`, complete coverage, zero unresolved, exact/conservative guarantee. |
| predicate rejected | Rejection reason code/count; not an error and not missing coverage. |
| missing fact/precision/capability | Typed unresolved and incomplete coverage, or structured error under `strict_coverage`. |
| unsupported selector/schema | Structured error; never an empty report or nearest-version execution. |
| ambiguous identity | All semantic candidates with ambiguity evidence/unresolved; no display-name selection. |
| partial producer/refinement | Valid rows survive with failed/unresolved coverage and downgraded guarantee. |
| open-world call | Static/direct target, possible targets, per-variant candidates, and open-world unresolved stay separate. |

Rows use full semantic/fact ID, variant signature, normalized source/macro origin, and relation role.
Call rows additionally preserve caller, static/direct target, receiver canonical type, dispatch,
possible virtual/indirect candidates, evidence, confidence, and guarantee. Canonical order is result
kind, full semantic ID, variant, normalized source range, then full row ID. Same-ID equal rows merge
contributors; conflicts fail. Cross-TU/variant data is never discarded. `considered = matched +
rejected + unresolved` and coverage conservation are independently validated before limit projection.

`search_report<T>` owns immutable backing. Accessor spans/references borrow until report destruction;
JSON/Markdown/query explanation derive from the same rows, accounting, evidence, coverage,
unresolved, precision, and guarantee and cannot recompute or upgrade them.

## Explanation and bounded projections

`selector`, `finding`, `coverage`, `edit_plan`, and `generation_plan` consume existing authoritative
values. They do not execute queries, revalidate plans, infer decisions, or parse prose. `why_not_matched`
executes one normalized selector against a declared candidate universe and reports matched,
predicate-rejected, and unresolved counts plus deterministic bounded examples. Unknown target and
empty candidate universe remain distinct.

Detail level controls projection only. Every output budget has deterministic truncation order,
`truncated=true`, omitted count, typed `core.budget-exhausted` unresolved, and a downgraded guarantee;
silent omission is forbidden. Redaction happens before ordering/serialization and cannot expose
absolute roots, secrets, raw argv, source text, or native addresses.

Agent task cards project exact headers/API IDs, requirements, preconditions, outputs, stable failure
codes, forbidden shortcuts, and verification steps. They never concatenate shell commands.
`api_catalog_json()` is canonical API-catalog/freeze-version data ordered by package/API ID; it
contains no runtime capability state, timestamps, checkout paths, environment, or implementation
probe results.

## Shared owners and dependency boundary

- `select` owns normalization, typed operator and reason-code registries, requirement expansion,
  selector schema, and type erasure.
- `search` owns shared query planning/execution, snapshot binding, result-row/report ordering,
  limit/accounting, and virtual resolver consumption. APIs do not duplicate engines.
- `explain` owns bounded pure projections and explanation/task-card/catalog schemas.
- `facts/workspace` (#44) own provisioning and snapshots; `graph` (#46) owns graph semantics;
  `core` (#43) owns finding/evidence/coverage; transform/generate (#48/#49) own plans.
  Consumer edges do not transfer or redefine those contracts.

## API disposition and evidence

All 25 manifest rows bind exact declaration/fingerprint, atomic family, policy, ownership/provider/
schema references, catalog API dependencies, traceability, Doxygen obligations, and positive/
negative/ambiguous cases. The candidate validator rejects omission, drift, dangling references,
duplicate registry ownership, or public-header pre-emption. The candidate usage is C++23
syntax-checked without linking production implementations.

## Issue #52 validation backlink

Validated as a graph/search dependency by `docs/design/high_risk_contract_validation.md#decisions`:
selector identities and canonical ordering support bounded graph expansion without first-wins. Fingerprint
`sha256:9bb0cb68c4121996a9dd550df67e070c1d0c04d3609e1399d777c2705b5f7008` was unchanged.
