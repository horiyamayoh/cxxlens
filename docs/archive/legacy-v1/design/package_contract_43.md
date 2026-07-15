---
cxxlens_document_status: archived
cxxlens_authority: non-normative
cxxlens_replacement: docs/design/cxxlens_next_generation_integrated_design_ja.md
cxxlens_removal_issue: "#72"
---
# Issue #43 package Contract Candidate

This document is normative for the `core`, `configuration`, and `testing` package candidate owned
exclusively by Issue #43. The machine-readable authority is
`schemas/cxxlens_package_contract_candidates.yaml#issue-43`; global meaning and transition rules
come from `cxxlens.global-contract-conventions.v1`. The candidate fingerprint is
`sha256:12ff14d27a96a78edb062e7263f4221f8b602e7347dc556c549715d3cd12cb4c`.

The state is `candidate`, not `frozen`. Issue #54 alone may freeze it. Candidate declaration
fragments under `contracts/candidates/43/` are non-installed inputs for Issue #53. No production
algorithm, implementation conformance, installed header, or write behavior is claimed here.

## Scope and exact-declaration evidence

The group owns exactly 17 catalog entries. All are exact and have one indivisible atomic unit.
Existing implementation states remain 13 `conformant` and 4 `unimplemented`; the newly exact
`API-CORE-005`, `API-CORE-006`, `API-TEST-003`, and `API-TEST-004` are still blocked on Issue #53
integration, and the plan assertions additionally depend on the #48/#49 contract candidates.

| Package | API IDs | Exact source |
|---|---|---|
| core | `API-CORE-001`–`006` | Existing core/evidence headers for 001–004; `contracts/candidates/43/core_findings.hpp` for 005–006 |
| configuration | `API-CFG-001`–`004` | `include/cxxlens/configuration.hpp` |
| testing | `API-TEST-001`–`007` | Existing testing header except candidate plan assertions 003–004 |

The manifest records every exact declaration and SHA-256 signature fingerprint, every family
member, ownership/lifetime/concurrency rule, six result outcomes, deterministic order and duplicate
policy, all five version axes, dependencies, and positive/negative/ambiguous acceptance cases.
The validator compares these records byte-for-byte with the catalog and rejects a split family,
dangling owner, missing API, or stale group fingerprint.

## Shared type and registry ownership

`core` owns the shared value/failure/evidence/schema substrate. A consumer may copy a value or hold
the documented const borrow; it cannot redefine stable IDs, error/unresolved meaning, coverage,
guarantee, registry lookup, or projection authority.

| Surface | Contract owner | Consumers and boundary |
|---|---|---|
| `result<T>`, `error`, `unresolved`, `execution_context` | `AU-CORE-001`, `integration.package.core` | All packages consume the stable value contract; no prose-based control or ambient port access |
| version/capability values | `AU-CORE-001` / `AU-CORE-002` | Registry snapshots are detached values; missing, incompatible, unsupported, and empty are distinct |
| `evidence`, `coverage_report` | `AU-CORE-003` / `AU-CORE-004` | Producers populate authoritative rows; reports and explanations are pure projections |
| `finding`, `finding_set` | Issue #43 (`AU-CORE-005` / `006`) | #47 rules/report, review, and QA are producer/read-only consumers and do not re-own identity or projection semantics |
| configuration values/provenance | Issue #43 (`AU-CFG-001`–`004`) | CLI/API adapters supply explicit layers; ambient environment cannot change analysis meaning |
| testing fixture/assertion values | Issue #43 (`AU-TEST-001`–`007`) | Helpers exercise production paths and preserve producer evidence/coverage |
| edit/generation plans | #48 transform / #49 generate | Testing uses forward-declared const references and owns only assertion semantics |

Stable error namespaces and schema/capability registry entries have exactly one owner in the
manifest. Unknown or duplicate entries reject. Testing consumes edit-plan and generation-plan
schemas rather than registering competing definitions.

## Finding and finding-set decisions

### Identity and authoritative data

Finding identity includes the rule/recipe ID, full typed semantic subject, normalized semantic file
key and half-open range, variant signature, and explicitly identity-relevant parameters. Message,
severity, checkout root, diagnostic prose, jobs, timing, backend selection, and hash seed are
excluded. The factory and independent validator recompute identity and reject invalid evidence,
coverage, precision/guarantee, or collision state.

`why()`, `unresolved_items()`, and `coverage()` expose authoritative structured data. The
`explanation_input()` boundary returns renderer-neutral views; it does not synthesize prose as a
second authority. JSON is canonical. Markdown and SARIF are pure projections of the same finding
rows.

### Collection decisions

| Condition | Decision |
|---|---|
| Same ID and same material payload | Collapse to exactly one row |
| Same ID and different material payload | `core.internal-invariant-violation`; never first/last-wins |
| Different variant signature | Preserve separate rows in canonical order |
| Confidence/severity threshold | Inclusive pure filter; preserve the full selected payload |
| Empty set with complete producer coverage | Successful empty result |
| Empty set with unsupported/failed producer | Error or incomplete coverage; never silent empty success |

The total order is rule/recipe, primary semantic file, begin/end offsets, semantic subject, variant,
then full finding ID. Input arrival, unordered containers, thread completion, and root paths are not
order inputs.

## Configuration resolution decisions

The immutable precedence order is:

1. explicit API option;
2. CLI option;
3. named profile;
4. config default;
5. safe built-in default.

Every resolved key retains its winning layer and all shadowed layers. Different values at the same
layer are `config.overlay-conflict`; duplicate YAML keys, unknown keys, invalid types/values, and
incompatible versions reject. No first-wins policy exists.

`load_nearest` uses canonical path identity, stops at `.git` or `.cxxlens-root`, and rejects symlink
escape and ambiguous roots. File and environment access cross explicit ports. Interpolation is
limited to schema-declared path placeholders; it cannot silently select profiles, flags, targets,
or semantic policies. Secret-like values are redacted from resolved and explanation projections.

| Outcome | Contract |
|---|---|
| Defaults | Fully validated value independent of ambient environment/root |
| Missing profile/file | Stable `config.profile-not-found` / `config.file-not-found` |
| Duplicate/same-layer conflict | Stable parse/conflict error, no fallback |
| Unsupported schema/interpolation | Explicit version/config error |
| Success | Complete immutable value; partial configurations never escape |

## Testing decisions

### Fixtures and production path

`workspace_fixture` and `fault_plan` are immutable value builders. Normalized duplicate paths,
variant names, and fault keys reject. Files/variants use canonical identity order; literal compiler
argument order is preserved as argv and never concatenated into a shell command. `open()` uses the
production catalog, frontend, reducer, store, and memory-filesystem port. It never substitutes a
fake semantic backend or manufactures facts.

### Result and plan assertions

`result_assertion` checks row count, operation error, evidence, coverage, unresolved, and canonical
schema bytes as separate channels. Zero rows, unresolved partial success, and failed operation are
not interchangeable. Mismatches return `testing.assertion-failed` with stable field paths and
expected/actual attributes.

`edit_plan_assertion` checks authoritative edit identities/ranges/digests, canonical diff, affected
variant reparse, and idempotence. `generation_plan_assertion` checks complete surface census,
mandatory decision axes, payload/artifact references, conflict state, and artifact reparse. Both use
isolated overlays. Neither invokes `apply()`, commits a transaction, emits an artifact, or ignores a
stale digest/conflict/reparse failure.

### Property, determinism, and schema checks

Property reports own seed, case index, canonical minimal failing input, reproduction string, and
executed count. Generator, predicate, shrinker, and renderer are call-scoped and invoked in
deterministic order; shrink candidates sort by rendered bytes and cycles cannot win.

Determinism compares canonical semantic output across parallelism, scheduler seed, reversed input
order, and workspace-root relocation. Operational timing/root data must be excluded from compared
bytes. `assert_schema_conforms` selects an exact registered ID/version under parser budgets;
unknown versions never select nearest/first schema, and invalid JSON remains distinct from schema
mismatch.

## Result and projection matrix

| Outcome | Required representation |
|---|---|
| Empty success | Value channel, complete coverage, no unresolved/error |
| Unresolved | Partial value with typed unresolved and incomplete coverage |
| Unsupported | Stable error, no manufactured value or fake fallback |
| Ambiguous | Preserved candidates/evidence plus typed unresolved, or a specified hard conflict error |
| Partial | Preserve all valid authoritative rows and downgrade guarantee |
| Failure | Stable error/field path; no diagnostic-substring control |

All JSON is canonical and schema-versioned. Unknown enum values reject. `cpp_api`, `public_schema`,
`fact_schema`, `semantics`, and `adapter` are independent axes. A change to signature, meaning,
ownership, dependency, schema, decision table, or evidence invalidates the candidate fingerprint.

## Integration and verification evidence

Issue #53 will merge the candidate declarations into stable headers, compile all Doxygen examples,
verify install consumption, and remove the `candidate_declaration_not_integrated` blockers. Until
then, CMake syntax-checks the candidate usage files directly without linking or production source
changes.

The package-candidate checker and negative suite prove exactly-once API/package ownership, exact
signature and family equality, complete six-outcome policy records, non-dangling public
type/component/provider/schema owners, dependency equality with the catalog, unique registry
owners, three bounded acceptance categories per API, non-public candidate headers, and semantic
fingerprint integrity. Task packets, ownership, readiness, and authorization artifacts are
regenerated after every catalog transition so the candidate state, dependency DAG, declaration
source, and fingerprint are propagated.

## Issue #52 validation backlink

Validated by `docs/design/high_risk_contract_validation.md#decisions`: the QA/process domain exercises
core result/evidence/coverage/schema values and finite failure projection. Fingerprint
`sha256:12ff14d27a96a78edb062e7263f4221f8b602e7347dc556c549715d3cd12cb4c` was unchanged; this is not implementation evidence.
