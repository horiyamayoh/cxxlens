# ADR 0103: Bounded Store candidate and report construction

- Status: Proposed for independent review
- Date: 2026-08-21
- Owner: #200
- Contract ID: `store.bounded-candidate-and-report.v1`
- Production qualification: not claimed

## Context

The production materializer must preserve ADR 0033's independent tamper detection without retaining
all tasks, a second complete candidate graph, and a complete public-report DOM simultaneously.
DF-0200 proves that moving existing vectors is insufficient. This ADR fixes the lifecycle needed
before implementation; it does not activate a production write path.

## Decision and state machine

Adopt an additive, move-only candidate port with the sole success path:

`idle -> candidate-open -> appending -> input-sealed -> independently-validating`
`-> validation-sealed -> cas-pending -> published -> report-streaming -> report-sealed`.

Any failure before `published` transitions through `aborting` to `aborted`. A report failure after
Store publication cannot erase the committed snapshot and returns typed
`materialization.report-failure` with the publication receipt; it never fabricates an all-success
report. Destruction of a nonterminal candidate performs bounded abort and records cleanup failure
without changing the original verdict.

The semantic operations are `begin_candidate(metadata, budgets, expected_head)`,
`append_partition` or `append_task`, `seal_input`, `validate`, `publish`, and `abort`. Names remain
additive; existing bulk APIs stay source/ABI compatible but are convenience adapters and are
forbidden on the production bounded path.

## Memory and storage contract

`W` is one task/source/output-validation window plus one actual and one independently generated
record, bounded codec/hash buffers, backend cursor state, and fixed counters. `F` is the single
immutable final memory-backend payload plus its query-serving indexes.

- SQLite peak process retention: `O(W)`, independent of task count and aggregate accepted output.
- Memory peak process retention: `F + O(W)`; no second complete `F` may coexist.
- Actual and expected external runs have separate namespaces and a declared finite disk quota.
- A candidate may own aggregate disk bytes but never exports a complete aggregate object graph.
- Report sections use private canonical spools; encoding is streaming and retains no complete DOM.

The scale witness is 4,096 tasks and 512 MiB aggregate admitted materializer output. Evidence records
peak RSS, SQLite page cache, temp/spool bytes, file count, and final memory payload separately. A
green functional test without these counters is not resource evidence.

## Independent validation

Backend staging contains complete claim envelopes. An independent generator reconstructs claim
semantic keys, assertions, content, rows, claim sets, coverage, partition content/count/completeness,
closure bindings, detached rows, annotations, unresolved records, and global identities bottom-up.
Actual and expected projections are separate canonical ordered cursors and are compared one complete
record at a time, byte for byte. Hashes may index and reject early but cannot replace comparison.
Encoder and validator may share canonical codecs, identity functions, and field validators only;
they must not share traversal, verdict, or projection-construction control flow.

Checksum-recomputed tamper, reordered input, duplicate/conflicting claims, omitted partitions,
detached rows, coverage/unresolved loss, and provenance/guarantee changes reject as `store.corrupt`.
Only an expected-head mismatch is `store.publication-conflict`.

## Phase-authentic values

| Phase | Values available | Values forbidden |
| --- | --- | --- |
| candidate-open | candidate ID, budgets, expected head | task/partition census, snapshot ID |
| appending | next input ordinal, observed bytes, per-window receipt | global completeness or identity |
| input-sealed | external request/journal census and spool receipts | validation verdict |
| validating | actual/expected cursor positions and bounded mismatch witness | publish/head receipt |
| validation-sealed | complete candidate identity and validation receipt | CAS/publication outcome |
| cas-pending | expected head and transaction-local candidate | public success |
| published | snapshot/head/publication receipt | report success |
| report-streaming | publication receipt, section offsets/counts/digests | sealed report digest |
| aborted | original typed failure and cleanup outcome | later-phase identities |

## Backend and compatibility rules

SQLite uses file-backed staging, bounded page cache, canonical ordered tables/cursors, one publication
transaction, and CAS after validation. Memory uses private spool input and constructs `F` once by
ownership transfer; indexes are part of `F` and cannot alter observable identity after publication.
Both backends yield byte-identical snapshot export/query/report semantics after SQLite reopen.

v1-v4 reads remain supported under existing legacy profiles. New production writes require a fully
revalidatable v5-or-later representation. Legacy amplification is measured separately and is not
authority to weaken the new write path.

## Crash/effect matrix

| Event | Public effect | Required outcome |
| --- | --- | --- |
| append/validation/disk-full/cancel | none | total candidate abort; typed original and cleanup receipts |
| crash before CAS commit | none | backend recovery removes private staging |
| CAS loss | none | rollback; `store.publication-conflict` only |
| crash during commit | atomic old or new head | ordinary Store recovery proves one state |
| report failure after commit | snapshot remains published | typed report failure bound to publication receipt |
| cleanup failure | never success | preserve original failure plus cleanup evidence |

## Counterexamples and acceptance

Reject a second complete candidate graph, digest-only comparison, shared traversal/verdict logic,
unbounded SQLite TEMP/page cache, full report DOM, report-before-publication, partial public report,
silent bulk-path use, non-v5 production writes, failure reclassification, and cleanup-as-success.

Acceptance requires an exact-main independent counterexample review covering the 4,096-task/512-MiB
witness, memory/SQLite parity, byte-exact tamper rejection, every crash/effect boundary, and a
checker that detects production calls into the bulk path. Implementation and release qualification
remain separate. #173 tracks the aggregate distribution decision, while the existing formal release
gate and terminal production-scope closure retain their accepted owners until coordinated authority
amendments are accepted.

## Independent review disposition

The review of exact commit `c69d9be74f3e4b2b42c455a7cd4bfeb30591b9e1` rejected acceptance with
seven P1 findings. The next revision must resolve all of the following before another acceptance
review:

- make report failure phase-authentic without adding a post-publication Store outcome that conflicts
  with ADR 0096;
- scope SQLite `O(W)` to the prepublication construction path or separately authorize and prove lazy
  query handles;
- distinguish staging-session identity from candidate identity and reserve report production before
  publication if report completeness is part of success;
- define independently derived actual and expected projections so comparison cannot be tautological;
- preserve `publication_outcome_unknown` across ambiguous commit outcomes;
- bind exact numeric resource limits from DF-0200; and
- name the exact v5 representation, physical matrix, contract IDs, and checker bindings.

The owner decision remains selected, but this ADR remains Proposed and implementation/production
activation must not treat the review reference as acceptance.
