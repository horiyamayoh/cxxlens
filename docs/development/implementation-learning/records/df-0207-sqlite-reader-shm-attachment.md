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
- SQLite `walIndexPage()` returns an already cached `Wal.apWiData[iPage]` directly and invokes
  the VFS map path only when the cached entry is absent, so a later read session cannot rely on a
  new `xShmMap` callback for lifetime-owner admission:
  <https://www3.sqlite.org/matrix/ev/src/wal.html#L796>.
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
open epoch, writer mapping generation, observed SHM object/entry/mount receipt, and exact
map/unmap/close callback cohort. Retain one exact audit receipt for each successful callback, one
live member for each unique exact generation page tuple, and no new live member or cleanup
authority for same-page revalidation. A different-page mapping still requires a fresh active
writer-lease predelegation, while every member shares exactly one attachment handoff and cleanup
owner.

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

2026-07-30: The four SQLite/Snapshot contract/schema mirrors now carry the exact review-pending
`reader_native_attachment_amendment_proposal`,
`cxxlens.sqlite.reader-shm-native-attachment.v1`, with status
`proposed-unqualified-non-authorizing` and no acceptance receipt. It binds one checked
non-reusable attachment epoch and observed identity, callback-specific audit receipts, unique
page members, one attachment handoff/unmap owner, a shared map/unmap/close sequence cut, an exact
active eager-use-owner census, close and successor quarantine, durable tombstones, and closed
outward status rows.

Adversarial review found that an earlier draft could proactively unmap an already-live group when
a later map failed validation, invalidating pointers still owned by an active eager session. The
revised proposal permits proactive cleanup only for an unpublished first-map failure after exact
zero-member/zero-use rederivation. A later-map failure hides the complete group but defers its one
native unmap until every use owner is terminal and a later exact SQLite unmap or close consumes
the sole owner. Only confirmed proactive cleanup installs a one-shot zero-native-effect logical
acknowledgement; close atomically consumes any pending acknowledgement. Ambiguous first-map
outcomes create only an open-epoch/reservation tombstone and never fabricate a group.

This revision also keeps native CANTINIT/null and READONLY/non-null outside the amendment, maps
denied, validation-failed, ambiguous, OK/null, and unleased OK results to IOERR/null, preserves
the accepted DF-0205/DF-0206/DF-0208 subtrees, and changes no source or production route. Fresh
independent semantic and structural review of an exact committed revision remains required.
Until that acceptance, this record remains `observed` / `blocked`; no reader grouping, cleanup
mutation, VFS binding, native-OK projection, public API change, or production activation is
authorized.

Further counterexample review found that SQLite may reuse `Wal.apWiData` in a later read
transaction without another `xShmMap` callback. Owner admission therefore cannot be callback-only.
The proposal now classifies authority before entering any SQLite API, creates a session
reservation only for an exact same-process live local mapping-generation candidate, and promotes
it immediately when an active proposal group already exists. This gives a callback-free
sequential session its own owner before any cached pointer use. A first successful proposal map
promotes the reservation; no-pointer failure consumes it; predecessor results transfer it to the
existing route; ambiguous outcomes retain it in the quarantine tombstone. Session admission and
terminal, map publication, unmap, and close share one sequence cut.

That pre-mint partition is also required to preserve ordinary readers. A cross-process or
otherwise qualified predecessor read with no live local writer mapping generation creates zero
proposal identity, reservation, map attempt, or owner and remains under the existing byte
contract. The proposal never invents or imports a local generation. It separately closes cached
pointer coverage, mixed proposal-group then READONLY/non-null cleanup ownership, base
READONLY/null normalization, protocol-invalid status/pointer terminal rows, opaque no-group
successor exclusion, and a closed custody enum covering session reservations, cleanup, ack,
close/cut, waiter/reporter, and lifetime pins. These refinements remain proposal-only until the
exact committed revision receives the required independent acceptance.
