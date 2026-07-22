---
id: DF-0198
title: Represent failed Store head observation authentically
status: accepted
kind: contract-contradiction
impact: invariant
confidence: high
implementation_disposition: may-proceed
scope:
  - provider.clang22-materialization-compact-store-head
  - store.materialization-head-observation-ledger
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_report.schema.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
tracking_issue: '#198'
implementation_issues:
  - '#181'
resolution_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml
  - schemas/cxxlens_ng_clang22_materialization_report.schema.yaml
  - tools/quality/check_ng_clang22_materialization.py
  - tests/quality/test_ng_clang22_materialization.py
review:
  mode: independent
  status: complete
  author: codex-agent-df0198-resolution
  reviewer: codex-agent-df0198-independent-review
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/198#issuecomment-5021863751
created: '2026-07-20'
---

# Represent failed Store head observation authentically

## Observation

A prepublication `snapshot_store::current(selector)` call can return a typed SDK error other
than `store.current-not-found`. The materialization Store adapter then truthfully retains
`head_current` as the first operation, `current-selector` as its access path, an `sdk_error`
head receipt, no head record, no writer, and no publication attempt.

The accepted materialization contract includes `head_current` among compact `store-stage`
failure operations and requires the exact first SDK cause. Its compact effect matrix also
requires the head observation at `store-stage` to be either `absent` or `present`. The report
schema exposes only `not-observed`, `absent`, and `present`, and its compact Store-failure branch
requires `access_path` to be null.

## Working mental model

A Store head lookup has two independent axes: whether the lookup was attempted and whether it
returned an absent/present value or an SDK error. An SDK error is not evidence of absence and
does not provide a present record. A phase-authentic effect ledger therefore needs a closed
failed/unavailable state, or the authority must explicitly classify this path as having no
authoritative response. The first Store cause should retain its authenticated operation and
applicable access path; diagnostic prose remains non-authoritative.

## Mismatch or opportunity

No currently schema-valid compact response is effect-authentic for this execution:

- `absent` infers absence from an SDK error;
- `present` invents a head record;
- `not-observed` contradicts the accepted `store-stage` effect matrix;
- preserving `current-selector` violates the compact report schema; and
- erasing the path or changing the operation to `store_open` loses the exact first cause.

The contradiction is narrower than DF-0196. That record established operation-first Store
outcome classification but did not model a failed prepublication head lookup in the compact
effect ledger. Other prepublication compact paths have an authentic representation.

## Evidence

- `schemas/cxxlens_ng_clang22_materialization_contract.yaml::report.compact_failure` includes
  `head_current` in the prepublication Store cause operations.
- The same contract's `compact_effect_matrix.store-stage` requires a discarded draft and an
  absent or present head observation.
- `schemas/cxxlens_ng_clang22_materialization_report.schema.yaml::$defs.effect_ledger` has no
  failed head-observation state, and the compact Store-failure conditional requires a null
  access path.
- `src/llvm/clang22/materialization_store.cpp::capture_head` retains a failed `current()` call as
  an SDK-error receipt, while `prepare_materialization_store` records the exact first
  `head_current` failure with the `current_selector` path.
- `src/llvm/clang22/materialization_execution_journal.cpp::record_store_preparation` can encode
  Store-open and later stage failures, but must reject this path to avoid fabricating or erasing
  the observation.
- Minimal counterexample: make `current(selector)` return `store.sqlite-failure`; Store open has
  succeeded, no head value exists, and no writer or publish call has occurred.

## Alternatives and trade-offs

1. Add a typed failed/unavailable head-observation state and permit `current-selector` for a
   `head_current` compact cause. This preserves the complete observation, but changes the accepted
   report contract/schema and requires versioning plus independent review.
2. Specify exit 2 with no authoritative response for this path. This avoids a wire change but
   weakens structured prepublication diagnostics and requires an explicit contract carve-out.
3. Treat every head SDK error as absence. This is rejected because it violates no-inferred-absence
   and can hide corruption or I/O failure.
4. Reclassify the failure as `store_open` or erase the access path. This is rejected because it
   makes the phase/cause ledger non-authentic.

## Recommendation

Review alternatives 1 and 2 against the report versioning policy. Prefer the smallest authority
change that keeps the authenticated operation, path, and failed lookup state reconstructible.
Until that decision is accepted, fail closed with exit 2 and no authoritative response for a
non-`current-not-found` `head_current` SDK error. Do not infer absence, invent a record, erase the
path, or emit a compact report that contradicts the effect matrix.

## Proposed authority resolution

2026-07-20: Alternative 1 is proposed with a closed four-state compact head observation:
`not-observed`, `absent`, `present`, and `sdk-error`. A `head_current` cause retains exact access
path `current-selector`. Exact `store.current-not-found` maps to `absent`; every other typed SDK
error maps to `sdk-error`; both require `observed_head_publication: null`. All other compact
prepublication Store operations continue to require a null access path. The schema and independent
checker enforce both directions, so `sdk-error` cannot appear without the matching cause and the
matching cause cannot be relabeled as absent or present.

The proposal keeps machine/report version `2.1.0`. That version is the not-yet-merged,
not-yet-qualified successor being authored by active Issue #181; protected `main` still carries the
superseded v1 authority. This is therefore a correction inside the same unreleased v2.1 rollout,
not a reinterpretation of a released 2.1 artifact. Independent review must reject this versioning
decision if any canonical or qualified 2.1 producer/consumer exists outside the active unit.

The request schema, public C++ API, detailed report branch, Store outcome categories, publication
boundary, and diagnostic-prose policy do not change. The existing `store-stage` draft-discard rule
also remains unchanged: it describes disposal of the invocation's unpublished materialization
draft, not proof that `snapshot_store::begin()` returned a writer.

Independent review must additionally try to falsify all of the following:

- a non-`current-not-found` `head_current` SDK error is schema-valid only as `sdk-error`, with null
  observed publication and exact `current-selector` cause path;
- `store.current-not-found` remains distinguishable as `absent` and cannot be relabeled
  `sdk-error`;
- a non-head Store operation cannot acquire `current-selector`, and a non-Store compact failure
  cannot acquire a Store cause or `sdk-error` head state;
- contract/schema drift that removes the fourth state, path binding, or effect-matrix rule fails
  closed; and
- the later C++ implementation derives this ledger only from the retained typed Store receipt and
  does not accept caller-forged phase/effect values.

## Disposition

2026-07-20: Investigation opened from Issue #181 while implementing the source-private compact
execution journal. Only compact response construction/classification for a failed
`head_current` lookup is blocked. Store-open failures, writer/stage/validate failures after an
authentic absent or present head, other compact phases, and postpublication no-compact enforcement
may proceed.

2026-07-20: The minimal four-state/path-binding authority amendment and adversarial checker tests
are proposed. The record remains blocked and is not accepted until an independent reviewer records
canonical Issue #198 review evidence and the reviewed authority/test set passes unchanged.

2026-07-20: Accepted after independent falsification review. The final authority binds
`sdk-error` bidirectionally to an exact non-`store.current-not-found` `head_current` cause with
`current-selector` and null observed publication, while `store.current-not-found` remains
`absent`. It also defines `store_draft_state` as the invocation-logical unpublished snapshot-draft
lifecycle, so a Store-open-adopted draft is released and reported `discarded` before compact
authority even when head lookup fails before writer begin. C++ output now preserves opaque SDK
diagnostic text with its exact UTF-8 byte count and digest, and independent Python schema/cause
validation covers both empty head-error detail and nonempty Store-open detail. Version `2.1.0`
remains valid because this is an unqualified, unreleased correction within active Issue #181.
