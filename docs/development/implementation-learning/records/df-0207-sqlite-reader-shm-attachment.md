---
id: DF-0207
title: Bind SQLite reader SHM handoffs to native attachments
status: observed
kind: contract-contradiction
impact: invariant
confidence: high
implementation_disposition: blocked
scope:
  - store.sqlite-same-process-shm-mapping
  - store.sqlite-reader-native-attachment-lifetime
  - store.sqlite-multi-instance-cas
  - provider.clang22-materialization-store-race
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0013-ng-sqlite-physical-store.md
  - docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md
  - schemas/cxxlens_ng_sqlite_store_contract.yaml
  - schemas/cxxlens_ng_sqlite_store_contract.schema.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.schema.yaml
tracking_issue: '#207'
implementation_issues:
  - '#181'
resolution_refs: []
review:
  mode: independent
  status: pending
  author: codex-agent-sqlite-reader-attachment-observation
  reviewer: null
  refs: []
created: '2026-07-28'
---

# Bind SQLite reader SHM handoffs to native attachments

## Observation

At exact PR head `bf30978eb34d5f94bbadfd675c8ce2b50fb2f899`, the production-inert
same-process SHM coordinator creates one `sqlite_shm_reader_handoff` and one
`sqlite_shm_reader_unmap_obligation` for every successfully promoted reader `xShmMap` callback.
The accepted reader authority requires a different-page mapping on the same reader connection to
acquire a new in-flight admission and permits it to promote after exact validation.

SQLite exposes one `xShmUnmap(sqlite3_file*, deleteFlag)` for the open-file SHM connection. The
qualified Unix VFS implementation removes that connection object and clears the file handle
SHM pointer in one callback. Thus one reader `sqlite3_file` that maps page 0 and page 1 produces
two handoffs in the current model but has only one real native unmap outcome. Applying the outcome
to one handoff leaves false live authority; applying or replaying it for both fabricates a second
native effect.

## Working mental model

A reader native attachment belongs to one exact reader file handle, alias, connection, open epoch,
and callback cohort. It may accumulate multiple independently validated page-map receipts. The
externally owned handoff and teardown obligation should represent that attachment group while
retaining exact per-map page, range, pointer, generation, and callback evidence. One native unmap
must atomically retire the complete member set.

Confidence is high in the cardinality mismatch. Exact later-map join, rejected-map cleanup,
map/unmap ordering, close, and remap rules remain proposal work and are not authority yet.

## Mismatch or opportunity

`reader_lifetime.different_reader_or_mapping` requires a new admission for a different page, while
`reader_lifetime.handoff` describes promotion to a reader native-attachment handoff. Neither the
authority nor the current coordinator groups multiple page-map receipts belonging to one reader
attachment into a single unmap effect.

DF-0206 intentionally groups writer holders only. Its exact review grants no reader grouping
authority, so treating the writer amendment as a transitive reader decision would be a silent
contract expansion.

## Evidence

- `src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp` appends one `handoff_record` and
  increments `generation_record::handoff_count` in `promote_reader`.
- `begin_reader_unmap` and `complete_reader_unmap` consume and erase exactly one handoff token per
  supplied callback outcome.
- `src/sdk/sqlite_same_process_shm_mapping_lease_internal.hpp` exposes one move-only
  `sqlite_shm_reader_handoff` and one `sqlite_shm_reader_unmap_obligation` per promotion.
- `tests/unit/sdk/sqlite_same_process_shm_mapping_lease_test.cpp` covers distinct reader
  connections but has no same-reader page-0 plus page-1, one-native-unmap case.
- The SQLite Unix VFS describes `xShmUnmap` as closing a connection to shared memory and clears
  `pDbFd->pShm` in that one call:
  <https://sqlite.org/src/artifact/410185df49?ln=5231-5281>.
- Issue #207 records the reproducible observation and keeps this record non-normative.

## Alternatives and trade-offs

1. Add an exact reader native-attachment group and make one move-only handoff own all validated
   member receipts plus one unmap obligation. This matches SQLite lifecycle and can share checked
   identity machinery with the writer group while keeping writer and reader authority distinct.
2. Keep per-map records internally but expose one attachment-level owner that seals and retires
   the complete member set on one callback. This preserves audit detail but must total-order later
   maps, rejected-map cleanup, unmap, and close.
3. Forbid a different-page map on one reader connection. This contradicts existing authority and
   assumes SQLite callback behavior that the VFS contract does not grant.
4. Reuse one callback result for multiple independent handoffs or delegate repeated unmaps. This
   fabricates or replays native effects and is rejected.

DF-0206 writer-only internal implementation may proceed after its own durable acceptance. Reader
grouping, reader VFS binding, native-OK exception activation, and production qualification remain
blocked by this record.

## Recommendation

Define a checked, non-reusable reader attachment identity bound to process/runtime/VFS/file family,
reader alias and lifetime, connection, authenticated main `native_file_node` / `xOpen` receipt,
open epoch, and exact map/unmap callback cohort. Preserve one authority member for each successful
map callback, while same-page reuse and different-page mapping on that exact attachment create no
additional native cleanup obligation.

Atomically hide and seal the complete exact member set before one unmap callback. Apply its exact
outcome once to the whole attachment, update generation handoff count and successor exclusion as
one state transition, and retain retired members as audit history only. Define total order for
later map versus unmap, post-native later-map failure, non-OK/unknown outcome, close without prior
unmap, close after unmap, and fresh non-reusable remap epoch.

Require positive cases for same-reader page 0 plus page 1 with one unmap, same-page reuse, two
reader attachments with two unmaps, and a multi-page reader group outliving writer retirement.
Reject cross-attachment aggregation, incomplete or duplicate member sets, outcome replay,
per-page unmap, second-map post-native failure that leaves the first member live, map/unmap races,
unknown/non-OK cleanup, close double cleanup, and stale attachment-epoch remap.

## Disposition

2026-07-28: Observation recorded during the read-only implementation-architecture pass following
exact DF-0206 proposal review at
`bf30978eb34d5f94bbadfd675c8ce2b50fb2f899`. Issue #207 and this record block reader attachment
grouping and production activation. They do not invalidate or broaden the independently reviewed
DF-0206 writer-only proposal. No reader implementation or normative authority changed.
