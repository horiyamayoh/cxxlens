# ADR 0103: Bounded Store candidate, adoption, and report construction

- Status: Accepted
- Date: 2026-08-21
- Contract IDs: `store.incremental-candidate.v1`, `materialization.bounded-report.v1`
- Support disposition: bounded candidate, independent validation, and two-phase report contract

## Context and scope split

The production materializer must preserve ADR 0033 independent tamper detection without retaining
all tasks, a second complete candidate graph, or a complete public-report DOM. Moving current
vectors is insufficient. This ADR defines prepublication candidate/adoption and ADR 0096 two-phase
report construction. It does not claim that a reopened SQLite Store/query handle has `O(W)`
residency; cursor lifetime, page residency, and query semantics remain independently specified and
tested Store behavior.

## Two coupled but distinct machines

The backend-owned candidate machine is:

`idle -> staging-session-open -> appending -> input-sealed -> candidate-identity-sealed`
`-> independently-validating -> validation-sealed -> (not-attempted | publication-attempt)`
`-> publication-terminal`.

`staging_session_id` is random/private cleanup identity available at open. `candidate_id` is a
semantic identity available only after the external request/journal census and complete input seal.
They are never equal or substituted. `publication-terminal` is exactly one of `not-attempted`,
`rejected-stale`, `rejected-store-failure`, `publication-outcome-unknown`, `committed-unverified`, or
`committed-verified`. Publication ends the Store candidate. Reporting is not another Store outcome.

The report machine is the accepted ADR 0096 bounded two-phase lifecycle:

`publication-independent-projection -> projection-validated -> maximum-tail-reserved`
`-> publication-attempt -> exact-outcome-captured -> outcome-tail-finalized`
`-> full-schema-validated -> bottom-up-cross-binding-validated -> stdout-published`.

Before publication, it may build only publication-independent report sections and must reserve the
checked maximum tail for every applicable detailed outcome, final JSON framing, exact SDK records,
receipts, and maximum bounded diagnostics. It cannot claim publication outcome, invocation
publication record, physical generation, or reopen status. If projection/reservation cannot produce
a schema-valid zero-effect compact response, the process exits 2 with zero authoritative stdout.

After the single publication attempt, failure to capture/finalize/validate/write the complete
response is exit 2 with zero authoritative response and no compact downgrade. A committed Store is
not rolled back; the Store recovery record is the only authority. Partial stdout is non-authoritative.
A safely constructible detailed response may preserve `committed_unverified`. A phase-opaque
writer/commit error always preserves `publication_outcome_unknown`, even if later inspection sees no
candidate. When the complete detailed failure is safely finalized, both
`publication_outcome_unknown` and `committed_unverified` exit 1; only an unsafe finalization or
transport failure exits 2 with no authoritative response. They never exit 0. Only an exact
expected-head mismatch is `store.publication-conflict`.

## Additive candidate port

The move-only port operations are `begin_staging_session(metadata, budgets, expected_head)`,
`append_task(sealed_task&&)`, `seal_input(external_census)`, `validate_independently()`,
`publish_once()`, and `abort()`. Existing bulk APIs remain source/ABI compatible convenience
adapters but are forbidden on this production bounded path. Destruction of a nonterminal session
performs bounded abort and records cleanup failure without replacing the original verdict.

## Independent actual and expected projections

The actual projection is produced only by replaying backend staging rows in canonical physical key
order and decoding their exact framed bytes. Cursor order must already equal the physical key
derived from the closed record grammar and semantic key; a backend-supplied ordinal is never
ordering authority, and the validator does not sort a cursor into compliance. The expected projection is produced before the first
event encoder call from the immutable sealed task result plus the independently retained selected
request and execution-journal receipts. It enumerates semantic keys, full claim projections,
detached rows, annotations, coverage, unresolved records, partition censuses/completeness, closure
bindings, and global identities bottom-up. It never reads staging rows, segment/run manifests, or an
encoder-emitted digest.

Both projections emit the same closed record grammar into separate namespaces and separate cursors.
Comparison is one full record at a time, byte-exact, including counts and ordered full projections.
The paths may share canonical codecs, identity functions, and field validators, but not traversal,
verdict logic, projection construction, or mutable state. Hashes may reject early but cannot replace
the dual byte comparison. Checksum-recomputed tamper, whole-partition omission, reordered input,
duplicate/conflicting claims, detached rows, or coverage/unresolved/provenance/guarantee drift is
`store.corrupt`.

## Exact bounds

`W` is the exact checked sum of a 64 MiB source/output-validation window, an 8 MiB sort arena, two
32 KiB comparator cursors, a 1 MiB backend cursor, 64 KiB codec/hash state, a 1 MiB record buffer,
and 4 KiB counter state: `77,729,792` resident bytes. No omitted allocator, backend cursor, codec, or
counter state may be treated as zero-cost. The maximum reserved report tail is exactly
`28,321,546` bytes, derived independently from the accepted ADR 0096 maxima: 10,420,985 global
projection bytes, 8,463,179 task-metadata bytes, 8 MiB exact SDK records, 1 MiB diagnostics, and 198
framing bytes. It is not inferred from the source window.
Each spooled record is charged before I/O as one kind byte, two u64 lengths, key bytes, payload
bytes, and a 32-byte checksum: exactly `49 + key_bytes + payload_bytes` logical bytes. The framed
record length, current segment length, spool length, and aggregate length are checked in unsigned
128-bit arithmetic before narrowing or append. Segment rollover, merge metadata, cursor storage,
and all 18 merge descriptors are covered by the fixed resident/descriptor census above rather than
treated as zero-cost spool overhead.
The exact DF-0200 limits are:

- 4,096 tasks and a 512 MiB aggregate scale witness;
- canonical v5 collection counts are unsigned 64-bit with checked unsigned-128 aggregate arithmetic;
- exact `CXLPEV01` stream header 86 bytes and exact `CXLPEEND` trailer 112 bytes;
- sort arena 8 MiB; records larger than it are streamed singleton runs;
- exactly two 32 KiB comparator cursors, total comparison budget 64 KiB;
- merge fan-in 16 inputs plus one output and one metadata descriptor, exactly 18 FDs;
- maximum authoritative report bytes 1 GiB;
- materializer source/output validation window 64 MiB, as fixed by the machine contract.
- all 18 merge descriptors have an explicit acquisition census and are released on every terminal;
  checked u64 counters and u128 aggregate arithmetic reject overflow before allocation or I/O.

Record length, segment/spool capacity, offsets, counters, report reservation, and additions are
checked before allocation/I/O. SQLite candidate/adoption/report peak retention is `O(W)`, independent
of task count and admitted output. Memory is one immutable final payload `F` plus `O(W)` and may never
hold a second complete `F`. Private spool/disk quota is an explicit bound, not resident memory.

## Representation and compatibility

Production writes use exact logical `cxxlens.ng-snapshot-payload.v5` with u64 collections and exact
decode/re-encode byte identity. SQLite writes use exact physical
`cxxlens.sqlite-semantic-store.v3` version `3.0.0`, chunk profile
`cxxlens.sqlite-payload-chunks.v1`, maximum chunk bytes `8,388,608`, the closed publication/chunk/head
matrix in ADR 0097, and its schema/user-object census. v1-v4 payloads remain read-compatible; they are
never production write output or silently upgraded. The direct bindings are
`schemas/cxxlens_ng_snapshot_store_contract.yaml`, `schemas/cxxlens_ng_sqlite_store_contract.yaml`,
`schemas/cxxlens_ng_clang22_materialization_contract.yaml`, and their existing fail-closed checkers.

## Phase-authentic fields

| Phase | Available | Forbidden |
| --- | --- | --- |
| staging open | staging session, budgets, expected head | candidate/snapshot identity |
| input sealed | external census, stream/task receipts, candidate identity | validation or publication verdict |
| validating | actual/expected cursor and bounded mismatch witness | publication receipt |
| report reservation | publication-independent projection and maximum tail capacity | outcome/generation/reopen claims |
| publication attempt | expected head and attempt receipt | zero-effect claim after ambiguous I/O |
| terminal capture | exact SDK return/error or outcome-unknown | fabricated prior-head preservation |
| full response sealed | complete validated bytes | partial stdout authority |
| aborted | original failure and cleanup outcome | later-phase identities |

## Crash/effect matrix

| Event | Durable/public effect | Required outcome |
| --- | --- | --- |
| append/validation/reservation/disk-full/cancel before attempt | none | abort private staging; typed original plus cleanup receipt |
| crash before publication attempt | none | recovery removes private staging |
| exact CAS loss | none | `store.publication-conflict` |
| phase-opaque commit I/O or crash | old or new head | `publication_outcome_unknown`; no blind retry |
| returned handle then reopen/verification failure | committed snapshot | safe detailed `committed_unverified`, otherwise exit 2/no response |
| post-attempt report allocation/validation/stdout failure | Store-defined old/new state | exit 2/no authoritative response; Store recovery record only |
| cleanup failure | no success promotion | preserve original failure and cleanup evidence |

## Counterexamples and acceptance

Reject a second full graph, common actual/expected traversal, digest-only validation, unbounded
SQLite TEMP/page cache, a complete report DOM, candidate identity before sealing, publication before
tail reservation, compact downgrade after attempt, loss of `publication_outcome_unknown`, partial
stdout authority, non-v5 writes, legacy one-BLOB SQLite writes, and implicit lazy-read claims.

The machine checker and direct positive, negative, fault, determinism, and resource-bound tests
cover the six-outcome policy, symbolic state ordering, separate backend-row/immutable-input
projection builders, byte-exact comparison including omission, duplication, reorder, and
checksum-recomputed tamper, ambiguous publication authority, and checked u64/u128
window/spool/descriptor arithmetic. Scale tests cover 4,096 tasks and 512 MiB, memory/reopened-SQLite
parity, every publication and report crash edge, and rejection of a production bulk-API path.
SQLite lazy-read residency is a separate Store behavior and does not change this contract's
supported product surface.
