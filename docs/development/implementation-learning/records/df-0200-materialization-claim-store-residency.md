---
id: DF-0200
title: Reconcile bulk-occurrence residency with materialization claim and Store staging
status: accepted
kind: contract-contradiction
impact: invariant
confidence: high
implementation_disposition: may-proceed
scope:
  - provider.clang22-materialization-claim-streaming
  - store.materialization-transaction-staging
  - release.materialization-scale-evidence
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
  - schemas/cxxlens_ng_sqlite_store_contract.yaml
  - schemas/cxxlens_ng_release_bundle.yaml
  - docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md
tracking_issue: '#200'
implementation_issues:
  - '#181'
resolution_refs:
  - docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
  - schemas/cxxlens_ng_sqlite_store_contract.yaml
  - tools/quality/check_ng_clang22_materialization.py
  - tests/quality/test_ng_clang22_materialization.py
review:
  mode: independent
  status: complete
  author: codex-agent-df0200-authority-resolution
  reviewer: codex-agent-sqlite-fresh-terminal-third-review + codex-agent-sqlite-algebra-third-review
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/200#issuecomment-5035818740
    - https://github.com/horiyamayoh/cxxlens/issues/200#issuecomment-5035818935
created: '2026-07-20'
---

# Reconcile bulk-occurrence residency with materialization claim and Store staging

## Observation

The accepted materialization lifecycle replays, validates, seals, and destroys one task input and
output window at a time. It also requires task indexes and bulk occurrences to live in private
spools rather than resident memory. The existing claim adoption API instead accepts a span of all
sealed task results and retains their complete provider rows, base/span rows, and decoded
observations until global claim construction finishes.

Claim construction then creates all-task maps, claim occurrences, one aggregate SDK claim batch,
claim envelopes, canonicalization edges, origin associations, and every partition in resident
vectors. The pipeline copies the partition drafts into a Store transaction. The SDK writer retains
all staged partitions and, during validation, builds another candidate representation containing
partition envelopes, rows, annotations, coverage, and unresolved values.

At the accepted 4,096-task bounds, connecting these APIs to the production materializer would make
peak retained memory proportional to all task output and introduce multiple complete-output copies.
That contradicts the one-output-validation-window and private-bulk-occurrence authority.

## Working mental model

There are three distinct lifetimes:

1. One task's authenticated provider transcript, decoded rows, and materialization seal are a
   bounded validation window and should be consumed once.
2. Cross-task canonicalization, deduplication, reference checking, claim association, and partition
   formation require aggregate order and census, but their sortable occurrences can be retained in
   source-private replayable spools rather than all-task object graphs.
3. A committed memory backend necessarily owns its final deduplicated semantic snapshot. That
   irreducible backend payload is not permission to retain every pre-adoption occurrence or several
   transient copies of the full output. If it is excluded from a transient working-set claim, the
   authority and measurement must say so explicitly.

The public Store transaction remains atomic: bounded or spooled preparation must still produce one
independently validated candidate, exactly one publish attempt, and the same memory/SQLite semantic
projection. Streaming is an implementation lifetime property, not permission to publish partial
task results.

## Mismatch or opportunity

The current private claim API and the public SDK writer are both semantically correct for small
in-memory inputs, but neither is a production streaming-adoption boundary. Moving vectors can
remove individual copies; it cannot satisfy the accepted invariant while all task seals,
occurrences, and staged partitions remain resident together.

The contradiction blocks the claims-to-Store production orchestration and the detailed report
projection that depends on it. Request validation, canonical source/task.v3 streaming, Provider
Protocol 1.1 execution, occurrence measurement, and compact prepublication failures do not depend
on this decision and may proceed.

## Evidence

- `docs/design/cxxlens_next_generation_integrated_design_ja.md:2315-2336` requires canonical-order
  replay with one task/input/output window and private-spooled bulk occurrences.
- `schemas/cxxlens_ng_clang22_materialization_contract.yaml` records the same retained-memory and
  production-scale requirements, including one output-validation window.
- `src/llvm/clang22/materialization_claims.hpp::construct_materialization_claims` requires a span of
  all `sealed_materialization_result` values.
- `src/llvm/clang22/materialization_seal.hpp::sealed_materialization_result` owns the provider seal,
  base/span rows, and decoded observation rows for one task.
- `src/llvm/clang22/materialization_claims.cpp::construct_materialization_claims` retains all-task
  maps, occurrences, aggregate claim batch inputs, and partition accumulators before returning.
- `sealed_materialization_claims` retains the final batch, envelopes, edges, associations, and
  partition drafts simultaneously.
- `src/llvm/clang22/materialization_pipeline.cpp::make_materialization_store_transaction` copies
  each partition draft into another transaction vector.
- `sdk::snapshot_writer::stage()` accepts one partition at a time but stores all partitions; its
  `validate()` constructs partition envelopes, rows, and annotations for the complete candidate.
- Minimal scale witness: 4,096 independently valid task seals at the per-task output limit require
  an all-task result span before the first Store stage, even though no semantic rule requires those
  decoded task result objects to coexist.

## Alternatives and trade-offs

1. Add a source-private `begin / consume_task / finalize` claim accumulator. Consume and destroy one
   task seal at a time, write canonical claim/report occurrences to bounded private spools, and use
   external canonical sort/dedup/reference/conflict/closure validation to produce a move-only
   replayable partition source. Adapt Store internals without adding a public C++ surface so
   precommit representations do not duplicate the complete output. This preserves accepted scale
   and transaction semantics, but is the largest implementation change.
2. Add an explicit aggregate provider-output/claim-byte limit and keep a finite in-memory batch.
   This is mechanically smaller but changes request/runtime capability, requires new budget and
   schema bindings, and still needs a proved amplification bound.
3. Weaken the retained-memory contract to allow all-task output, claim, and Store candidate graphs
   to remain resident. This matches the current code but abandons the accepted production-scale
   guarantee and is not recommended.
4. Move rather than copy existing vectors. This reduces amplification but leaves the all-task
   residency contradiction and is therefore insufficient by itself.

## Recommendation

Prefer alternative 1. Keep the new accumulator and replayable partition source private to the
installed materializer/Store implementation; do not broaden the public C++ API. Define exact spool
limits, canonical occurrence codec, external ordering/deduplication, global hard-reference and
conflict validation, failure cleanup, and fail-closed replay receipts before implementation.

Explicitly distinguish the final deduplicated memory-backend snapshot payload from transient
working memory if the former is an allowed resident lower bound. Measure both the final payload and
peak transient amplification. Require 4,096 tasks, 512 MiB aggregate source, limit-adjacent provider
output, arbitrary spool failures, deterministic permutation evidence, and memory/SQLite semantic
parity before production qualification.

## Accepted authority resolution (implementation pending)

The following v2.1 lifetime amendment is accepted authority and unblocks implementation. It does
not claim the incremental production path, required evidence, or release qualification complete.
The checker remains fail closed against the exact accepted resolution and must not be weakened by
retargeting a fixed digest around a semantic difference.

The Snapshot Store checker validates its self-contained hardcoded ingress object and does not load
the materialization contract. The materialization checker alone validates the downstream exact
resolution ID `cxxlens.df-0200.incremental-claim-store.v1` and codec/completeness/counter/SQLite
decision bindings against that Store object; any missing or drifting downstream binding rejects
the proposal.

### D1 — independent claim-batch oracle

Keep the resident public sdk::claim_batch::commit control flow as an independent bounded reference
during qualification. The production incremental path may share only canonical codecs, identity
functions, and field validators with that reference; shared commit control flow or verdict logic is
forbidden. Freeze pre-refactor canonical claim-batch v2 bytes and verdicts as
cxxlens.df-0200.claim-batch-differential-corpus.v1 and compare both the current public API and the
production path against it. The corpus closes added versus existing claims, new-to-existing
hard/soft references, conflicts and differentials, existing-to-existing non-reclassification,
one-shot versus split batches, input permutations, exact duplicates, metadata-distinct
same-content occurrences, and complete byte/verdict/census projections. Regenerating the corpus
without independent review and a new frozen digest is forbidden.

The immutable artifact is
`schemas/cxxlens_ng_df_0200_claim_batch_differential_corpus.tsv`, validated by the adjacent
schema and frozen at raw SHA-256
`f05513d05b0b57788b6f94d9c1a477c88d589b64dd8232d88a5c6c6022a84836`. Its ten cases contain
nine success tuples and one structured error tuple, 17 added and 16 existing claim inputs, and
88,607 bytes in the case/input/expected/verdict census projection. That projection is fixed at
`semantic-v2:sha256:741e9c5d7682f11a574b5218de1b48b009907782441bc3c9ac1488f264d42ba3`.
The dedicated C++ qualification driver executes the current public `sdk::claim_batch::commit` for
all ten cases and compares the complete artifact bytes. Its source-private `--emit` mode is not an
artifact update authority. The production incremental comparison remains an accepted-activation
step, but the current public reference is executable now.

### D2 — move-only task consumption and full-byte event codec

Use the private begin / consume_task(sealed_materialization_result&&) / finalize() && lifecycle,
accept only the exact next canonical task, and destroy each seal before returning. The final source
uses cxxlens.df-0200.partition-event-stream.v1. Its byte order is big-endian and key/payload values
use cxxlens-canonical-tuple-v1.

Each spool stream has an exact 86-byte CXLPEV01 header. A frame is event-kind u8, key-length u64be,
payload-length u64be, exact key bytes, exact payload bytes, and a raw 32-byte SHA-256 checksum over
the domain and all preceding frame bytes. The exact kind codes 1 through 7 are partition-begin,
claim-occurrence, detached-row, claim-annotation, coverage, unresolved, and partition-end. The
contract closes every key and payload projection. The exact 112-byte CXLPEEND trailer binds spool
index, next global u128 event ordinal, actual count/bytes, frame digest, and stream-prefix digest.
Unknown kinds, missing or duplicate boundaries, reordered/interleaved events, ordinal gaps,
noncanonical values, truncation, checksum/digest mismatch, and trailing bytes reject the candidate.

Canonical tuple tags, every event field type and cardinality, collection ordering/deduplication,
and key/payload field order are closed. Stream-sequence, frame, both trailer digests, and every
task/partition/global event, claim, row, coverage, and unresolved aggregate use dedicated domains.
Every semantic aggregate binds its explicit u64 count and ordered, length-framed full canonical
projections. Exact duplicate claim occurrences collapse under the independently applied claim law,
metadata-distinct same-content occurrences remain distinct, and duplicate final event projections
reject.

### D3 — external completeness and private Store ingress

Stream header and trailer claims are cross-checks, never completeness authority. The authorities are
the selected-schema validated request and the sealed execution journal/task receipts external to
the event stream. They bind request/task order and budget, source and provider stdout receipts,
decoded provider frame count, and exact task/partition/event/claim/row/coverage/unresolved
censuses and digests. Segment/run/merge manifests and byte/record/seal receipts are mandatory.
The Store independently compares all of them before constructing a candidate; a whole partition
drop is rejected even when the edited stream remains internally self-consistent.

The pre-encoder receipt oracle enumerates the exact event identity/full-projection multiset from
the immutable sealed task result before the first event encoder call. It shares only codecs,
identity functions, and field validators with the encoder. The selected request produces exactly
one pre-dispatch entry binding per canonical task; each task receipt cross-checks its own task ID
and ordinal. A task receipt binds `successful-seal`, the Provider Runtime raw stdout/frame/sealed
transcript authorities, and task partition/event/claim/row/coverage/unresolved count-plus-full-
projection digests. The task result is destroyed only after both this receipt and the event stream
are sealed. Task receipts bind the pre-existing selected-request entry, never the final execution
journal; the cycle-free final journal digest is created afterward over canonical task order and the
ordered task receipt seals.

Store replay compares raw provider stdout bytes/digest/frame census, receipt partition/event full
projection digests, task receipt seals, and dedicated global digests. Editing a stream together
with its partition-end/trailer still fails the fixed pre-encoder receipt. Editing stream, task
receipt, and task seal together still fails the immutable execution-journal receipt-set digest (or
the fixed selected-request entry cross-check).

The private bridge preserves one unpublished candidate, one publish attempt, and zero partial
visibility. Only semantic-version components have a u32 ceiling. Canonical v5 collection counts
are u64. The current one-million and ten-million decoder caps are legacy implementation guards to
remove through bounded streaming, not normative ceilings. A count greater than u64 maximum must
produce the exact private ingress SDK tuple operation partition_stage, code
store.counter-overflow, field materialization-v5-collection-count, empty detail. Its public mapping
is store-stage / materialization.store-failure with the draft discarded and no publication attempt.

### D4 — exact framing, rollover, and accounting

A framed record includes kind, both lengths, key bytes, payload bytes, and checksum. A record never
crosses a segment or spool. Before append, checked-u128 arithmetic validates framed length,
segment capacity, spool capacity, and aggregate census. Segment intervals are half-open; a
non-final end canonicalizes to the next segment at offset zero, while final EOF is the pair whose
segment index equals segment count and whose offset is zero. Empty non-final segments are
forbidden. After segment rollover, a record that does not fit
starts the next spool at the record boundary. Aggregate censuses remain checked u128 across every
spool, run, and merge.

The sort arena is 8 MiB. A larger framed record becomes one streamed singleton run. Exactly two
32 KiB comparator cursors share the total 64 KiB comparison budget. A merge uses 16 input, one
output, and one metadata descriptor, exactly 18 FDs. Overflow tests use checked operands rather
than trying to construct u128 maximum plus one. Known record/spool/collection/report limit failures
precede any I/O; actual private-spool ENOSPC is phase-authentic only after the append is proved
in-range. The report limit remains 1 GiB.

### D5 — phase-authentic failures and existing publication semantics

Only an actual private prepublication spool port I/O, hash, or ENOSPC failure is
materialization.spool-failure. The 2026-07-22 accepted activation atomically adds the three
private-spool phase bindings, their request-bound reverse closures, the partition_stage
counter-overflow tuple, and the updated full report-schema canonical JSON digest. Request 2.1.0
shape remains unchanged; report 2.1.0 activates only the private spool-failure closure, the exact
13/19-file occurrence inventory, and the 4,096-item sandbox-requirement bound.

SQLite writer_publish ENOSPC or SQLITE_TOOBIG retains the existing exact
writer_publish / store.sqlite-failure / database / opaque mapping and
publication_outcome_unknown semantics. If publish returns a commit handle and later verification
fails, the existing committed_unverified detailed response remains authoritative when it can be
constructed safely. Exit 2 with zero authoritative stdout is limited to a spool/allocation/report
construction or transport failure that makes any response unsafe, or to a successful-receipt or
checked-arithmetic contradiction. A post-publish failure is not by itself a no-response condition.

### D6 — compatibility and accepted SQLite capacity decision

Request 2.1.0, installed public C++ headers/signatures/callable inventory, claim/Store identity, and
logical canonical v5 remain unchanged. The public catalog changes only by the additive non-callable
SQLite-v3 behavior entry carrying `store.migration-required`; it adds no callable or CLI. Report
2.1.0 now carries the accepted private spool-failure, occurrence-inventory, and sandbox-bound
activation. The full parsed report schema canonical JSON digest is
sha256:f321e25f72bf8c6312dfe1e36fe6b6573239db697c2cfabd60e2c0546f9ee98b.
Private receipts and counters remain outside the public report/API.

A confirmed High blocker requires a physical-format revision: SQLite v2 stores one payload BLOB and the qualified
runtime MAX_LENGTH is 1,000,000,000, so it cannot satisfy required limit-adjacent passed
memory/reopened-SQLite parity. On 2026-07-21 the user selected alternative A, and two distinct
independent falsification reviews subsequently accepted its authority. The exact
`sqlite_capacity_decision` therefore has status `accepted` and `selected_alternative: A`.
Implementation may proceed, while required evidence and release qualification remain pending. The
evaluated alternatives are:

- A: authorize SQLite physical v3 segmented/chunk-table storage and deterministic migration while
  preserving logical canonical-v5 bytes.
- B: authorize a successor request/budget and a single cross-backend canonical-payload cap used by
  both memory and SQLite qualification.

Alternative A is selected; alternative B is not. The accepted binding is SQLite physical v3
bounded chunk-table storage, exact v2.6 read-only direct-open, and deterministic explicit
`snapshot_store::compact()` migration. It adds no public callable or CLI, performs no implicit
migration, and preserves logical canonical-v5 bytes and identities. Weakening parity is forbidden.
ADR 0097, the Store contracts, validators, and registered release-axis exception are accepted
authority. Production acceptance remains pending implementation and required qualification
evidence. D3 and D6 do not claim that unchanged SQLite v2 resolves the capacity requirement.

### Required falsification

- Frozen pre-refactor corpus and current public API byte/verdict differentials, including every D1
  added/existing, reference, conflict, non-reclassification, split, permutation, duplicate, and
  metadata-distinct case.
- Full codec unknown/missing/reordered/truncated/checksum negatives, external census/digest drift,
  segment/run/merge and byte/record/seal receipt drift, and whole-partition drop.
- Record/segment/spool rollover, u64 collection and checked-u128 overflow, singleton-run,
  comparator, fan-in/FD, and 1 GiB report boundaries.
- Exact partition_stage counter-overflow tuple; every private-spool failure; SQLite writer_publish
  outcome-unknown and returned-handle committed-unverified preservation.
- 4,096-task one-live-seal evidence, single-owner memory payload evidence, and
  memory/SQLite/static/shared parity. The selected SQLite A authority must prove v3 cold reopen,
  side-effect-free v2.6 read, explicit-only migration, crash/outcome-unknown recovery, and rejection
  of every unregistered cross-major transition.

## Disposition

2026-07-20: Investigation opened from Issue #181 and tracked by Issue #200 after the runtime gap
audit compared the accepted one-window/private-spool lifecycle with the existing all-task claim and
Store staging APIs. Claims-to-Store production orchestration, detailed report construction from its
candidate, and scale qualification are blocked. Unrelated request/provider streaming foundation may
continue. No authority or implementation decision has been accepted by this record.

2026-07-21: D1-D6 were bound as a proposed, fail-closed v2.1 authority amendment for independent
review, then tightened after independent review findings. The current request-bound failure matrix
and report schema remain unchanged until one accepted activation change adds the private-spool
bindings, exact Store counter tuple, reverse closures, and updated schema digest. The user selected
SQLite capacity alternative A. Its proposed authority is now bound by ADR-0097, the SQLite and
Snapshot Store contracts, and the single registered release-axis migration; alternative B is not
selected and parity weakening remains forbidden. At that checkpoint status remained
`investigating` pending the final independent authority reviews.

2026-07-22: Two distinct independent falsification reviews accepted the fresh/open/terminal and
compressed descendant-algebra authority, with canonical evidence recorded on Issue #200. ADR 0097,
the SQLite/Snapshot/Materialization exact bindings, and Option A are accepted. This record is now
`accepted` / `may-proceed`; implementation, production-path differential evidence, scale evidence,
and release qualification remain pending.

2026-07-22: The accepted D5/D6 report-schema activation was applied atomically after the private
spool phase bindings, reverse closures, occurrence inventory, sandbox bound, and Store overflow
mapping became executable. Request 2.1.0 remains unchanged; report schema digest
`sha256:f321e25f72bf8c6312dfe1e36fe6b6573239db697c2cfabd60e2c0546f9ee98b` is now the exact binding.
This activation does not qualify SQLite v3 or production release evidence.
