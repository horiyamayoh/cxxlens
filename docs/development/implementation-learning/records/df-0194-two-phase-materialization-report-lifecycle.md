---
id: DF-0194
title: Split materialization report construction across publication
status: accepted
kind: contract-contradiction
impact: invariant
confidence: high
implementation_disposition: may-proceed
scope:
  - provider.clang22-installed-materialization-report-lifecycle
  - store.snapshot-publication-boundary
  - release.materialization-response-authority
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_report.schema.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
tracking_issue: '#194'
implementation_issues:
  - '#181'
resolution_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - docs/design/catalogs/README.md
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml
  - schemas/cxxlens_ng_clang22_materialization_report.schema.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
  - include/cxxlens/sdk/store.hpp
  - tools/quality/check_ng_clang22_materialization.py
  - tests/quality/test_ng_clang22_materialization.py
review:
  mode: independent
  status: complete
  author: codex-agent-root
  reviewer: codex-agent-issue181-sealed-runtime
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/194#issuecomment-5018678943
created: '2026-07-20'
---

# Split materialization report construction across publication

## Observation

ADR 0096 and the materialization machine contract require the complete schema-valid
detailed report bytes to be constructed in a bounded private spool before Store
publication. The same detailed report requires the exact publication record returned by
`snapshot_writer::publish()`, including its transaction-assigned physical generation,
and exactly three receipts obtained from the committed or reopened Store.

Those values do not exist before publication. The SQLite Store allocates physical
generation inside its database-wide writer transaction, where concurrent publication or
compaction can change the next value. The `current(selector)`,
`open_publication(candidate)`, and `open(candidate snapshot)` receipts can only report
committed reader-visible state, and SQLite qualification additionally requires closing and
reopening the Store first. A placeholder, predicted generation, fabricated receipt, or
report spool patched after publication is not a complete truthful report constructed
before publication.

## Working mental model

Report construction has two bounded phases separated by the irreversible Store
publication boundary. Before publication the materializer can complete and validate the
sealed materialization DAG, validate the Store candidate, construct and independently
validate every publication-independent detailed-report projection, and reserve bounded
capacity for the largest permitted postpublication tail. It cannot complete the final
report or claim actual publication values at that point.

After the one publish attempt, the materializer uses only the actual SDK outcome,
publication record, close/reopen observations, and three path receipts to finalize one
private response spool. It then performs full report-schema and independent semantic
validation before the first stdout byte. A failure after the publication attempt cannot
be downgraded to a compact zero-effect response. It exits 2 with no authoritative
response; if commit occurred, the committed Store record is the sole recovery authority.

## Mismatch or opportunity

The current prepublication-complete wording and the required postpublication evidence
cannot both be satisfied by any implementation. Keeping the wording would force the
materializer either to publish before satisfying an explicit invariant or to invent
future Store observations. Both choices would make release evidence untruthful at the
irreversible transaction boundary.

The contradiction is in lifecycle ordering, not in the request/report v2 JSON shape,
identity algorithms, or public Store API. A bounded two-phase erratum can therefore keep
those surfaces unchanged while making the effect boundary implementable and testable.

## Evidence

- ADR 0096 says the complete schema-valid report bytes are built before publication.
- `surface.resource_limits.report_construction` says
  `bounded-spool-before-publication`.
- A passed report requires `invocation_committed_record` with
  `physical_generation` and a reopened Store projection with exactly three path receipts.
- The SQLite Store allocates physical generation after beginning the writer transaction
  from database-wide committed state; the value cannot be reserved truthfully by the
  materializer.
- The required SQLite path receipts are obtained only after closing and reopening the
  Store, which necessarily follows publication.
- The current checker creates fixture generations and receipts directly. Fixture schema
  validity therefore does not prove a realizable runtime ordering.
- Independent pre-change review reproduced the contradiction and found no execution
  sequence satisfying both requirements.

## Alternatives and trade-offs

1. Adopt a bounded two-phase lifecycle. Before publication, validate the candidate and
   all publication-independent report projections and reserve the bounded tail. After
   publication, use actual records and receipts to finalize and fully validate one report
   spool before stdout. This retains request/report v2 and the public Store API.
2. Introduce a new request/report v3 lifecycle and shape. This is unnecessary because no
   wire field, identity, or public API needs to change, and it would create an avoidable
   migration surface.
3. Add a Store preview or generation-reservation API. This still cannot produce
   postcommit reopen receipts and would expand public API without resolving the full
   contradiction.
4. Make the publication record or path receipts optional, or synthesize them in advance.
   This weakens accepted Store evidence and permits false release qualification.

Alternative 1 is recommended. The repository owner explicitly selected it for
Issue #181. Compact `report-construction` failure remains available only before the publish
attempt with zero invocation commits. Once the attempt begins, finalization, validation,
or stdout failure is an exit-2/no-response condition, and no release evidence is emitted.

## Recommendation

Amend ADR 0096 and machine contract v2 with an explicit bounded two-phase lifecycle.
Require publication-independent projection validation and response-tail capacity
reservation before the publish call; forbid completed-report, placeholder generation, or
fabricated receipt claims at that phase. After the call, require actual outcome and reopen
receipts, one final private spool, full schema and semantic validation, and no stdout bytes
before validation completes. Reject reintroduction of the old
`bounded-spool-before-publication` marker and any compact-failure path after publication
was attempted.

Treat this as a pre-qualification correction within machine contract and request/report
v2 `2.0.0`: no conforming installed runtime or release evidence exists to preserve, the
wire union and all required fields are unchanged, and the changed contract/schema bytes
remain exact authority-digest inputs. Replace the old scalar report-construction marker
with a schema-closed lifecycle object and add exact process-exit markers. Add fail-closed
checker mutations for lifecycle order, capacity reservation, prepublication forbidden
claims, actual-SDK-only publication values, post-attempt compact prohibition,
partial-stdout non-authority, and exit-2/no-response recovery rules.

## Disposition

2026-07-20: Proposed as a blocking publication-lifecycle invariant correction. Issue #181 may
continue codec, validation, sealing, and other reversible prepublication work, but Store
publication/report integration remains blocked until the normative erratum and independent
review are complete.

2026-07-20: Accepted after independent adversarial review. The reviewer confirmed that
the schema-closed bounded two-phase lifecycle is satisfiable, retains exact v2 `2.0.0`
wire and public Store surfaces, reserves capacity across every applicable detailed
outcome, uses only actual SDK publication values, fixes the publication-attempt and reopen
order, and makes post-attempt/partial-output failures non-authoritative. The checker and
negative mutations reject the legacy scalar, prose reintroduction, capacity/reopen drift,
predicted publication evidence, late attempt boundary, OS atomicity claims, and compact
downgrade after an attempt. Issue #181 may proceed with Store publication/report
integration under the amended authority.
