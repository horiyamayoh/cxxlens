---
cxxlens_document_status: archived
cxxlens_authority: non-normative
cxxlens_replacement: docs/design/cxxlens_next_generation_integrated_design_ja.md
cxxlens_removal_issue: "#72"
---
# Issue #47 rules・report package Contract Candidate

## Authority and unchanged ownership

This document and `schemas/cxxlens_package_contract_candidates.yaml#issue-47` are normative for the
nine assigned APIs. All nine remain unimplemented/blocked. `core` (#43) continues to own
`finding`/`finding_set`; #48, #49, and #51 own edit, generation, and review values. #47 owns only
rule evaluation, suppression, and pure rendering. #53 integrates headers and #54 freezes contracts.

## Immutable rule identity and construction

A rule identity is `(normalized rule id, semantic version)`. Title, description, diagnostic text,
severity, confidence, enable state, or execution order never enters identity. IDs are namespaced
lower snake/dot/hyphen strings; duplicate identity with equal canonical payload deduplicates and a
different payload fails `rules.invalid-metadata`. Unknown fields/versions and incomplete builders
fail rather than defaulting.

`rule_builder` is immutable shared value state. `when` and `unless` consume normalized semantic
selectors; requirements are the deterministic union of every selector branch, scope, fix proposal,
fact, capability, and precision requirement. Negation does not remove prerequisites. Diagnostic
templates accept only registered typed match-property placeholders. No arbitrary expression,
retained callback, exception-crossing callback, or prose-based control is permitted. `fix` stores an
immutable #48 codemod proposal and never edits source or claims fix safety.

## Execution, reduction, and rule packs

Each run binds one rule/pack version, workspace catalog/fact snapshot, scope, suppression policy,
and execution budgets before provisioning. Required facts/query subexpressions are unioned and
hash-consed across a pack; one rule never reparses a TU independently. Candidate evaluation may be
parallel, but rules order by `(id, version)`, matches by canonical semantic/source identity, and
findings by the #43 finding order. Worker completion order is irrelevant.

Finding ID is constructed before suppression. Same-ID equal findings merge evidence contributors;
conflicting payload is a structured failure. A rule failure, candidate unresolved, provider
unavailability, cancellation/deadline/budget, and partial workspace coverage remain distinct.
Complete zero findings requires complete coverage. Fix unavailable/unsafe/ambiguous is evidence on
an otherwise valid finding and cannot erase the finding.

`rule_pack` is immutable. Nested packs flatten in canonical identity order. Duplicate equal rules
deduplicate; identity conflicts reject. Enable/disable patterns are exact v1 globs, last matching
overlay within one provenance layer wins, and layer order is defaults, profile, project, invocation.
Unknown rule patterns produce diagnostics. `fail_fast` returns no partial success; `continue_partial`
preserves successful findings with failed/unresolved coverage and downgraded guarantee.

## Suppression authority and accounting

Suppression keys are typed file/range/symbol/rule/category/tag/minimum-severity/baseline finding ID
fields. File identity is normalized relative to the workspace root; symbol and finding use full
typed IDs. Macro spelling/expansion, generated/system origin, and build variant remain explicit.
Name-only symbol matching, absolute-root identity, and first variant wins are forbidden.

Precedence is inline source, external configuration, then baseline; the winning entry and every
rejected contender retain provenance. Each entry has a stable canonical ID, justification, optional
owner, and expiry. Invalid/expired/unused/unknown-rule suppressions produce typed diagnostics.
Suppressed findings remain authoritative rows with suppression state/evidence; user projections may
hide their display, but coverage conserves matched, emitted, and suppressed counts. Redaction occurs
before serialization and never changes match identity.

## Deterministic report projection

`render` consumes an already validated authoritative object and never reruns a query, rule, plan,
review, validator, or suppression decision. Supported pairings are finding set to JSON/Markdown/
SARIF, edit plan to JSON/Markdown/unified diff, generation plan to JSON/Markdown, and review report
to JSON/Markdown/SARIF. Unsupported pair/schema/version is `report.format-unsupported`; no nearest
format or schema fallback exists.

Output is UTF-8 with LF newline, canonical escaping/order, fixed `C` locale, no timestamps, and no
ambient root/environment. Paths are project-relative, redacted tokens, or basenames; absolute path
output is intentionally absent. Source excerpts are bounded and redacted. `output_budget_bytes`
applies after canonical row ordering at a record boundary. Truncation sets `truncated`, exact omitted
count, typed budget unresolved, and downgraded guarantee. `rendered_report` carries media type,
source schema, bytes, unresolved, and guarantee. Rendering has no filesystem side effect.

## Shared boundaries and #52 spike

- #43 owns finding identity/evidence/coverage/order; rules/report are read-only consumers.
- #45 owns selector normalization/query execution; rules reuse it without a second matcher.
- #48/#49/#51 own plan/review validity and schemas; report only checks declared compatible versions.
- `rules` owns rule/suppression schemas, evaluator/reducer, enable overlay, and suppression matcher.
- `report` owns format/options/envelope registries and renderer; format adapters share one canonical
  projection IR rather than recomputing authority.
- #52 must spike duplicate/version conflicts, shared provisioning, jobs/root/cache determinism,
  partial-rule continuation, suppression precedence/expiry/unused diagnostics, SARIF fidelity,
  path/snippet redaction, byte-boundary truncation, and unsupported consumer versions.

Positive, negative, and ambiguous fixtures for every API bind these rules to exact signatures,
owner/provider/schema edges, and Doxygen obligations.

## Issue #52 validation backlink

Validated by `docs/design/high_risk_contract_validation.md#decisions`: review/rule/report compatibility,
six-state baseline classification, bounded diagnostics and indeterminate partial gate were reproduced.
Fingerprint `sha256:139632fb5016e56fbf7085ca6a93e7cc23e40bdfd0063ce18b2910e5e742c956` was unchanged.
