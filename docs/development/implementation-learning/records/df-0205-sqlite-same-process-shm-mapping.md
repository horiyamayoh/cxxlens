---
id: DF-0205
title: Reconcile same-process SQLite SHM reuse with multi-instance CAS
status: observed
kind: contract-contradiction
impact: invariant
confidence: high
implementation_disposition: blocked
scope:
  - store.sqlite-same-process-shm-mapping
  - store.sqlite-multi-instance-cas
  - provider.clang22-materialization-store-race
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0013-ng-sqlite-physical-store.md
  - docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md
  - schemas/cxxlens_ng_sqlite_store_contract.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
tracking_issue: '#205'
implementation_issues:
  - '#181'
resolution_refs: []
review:
  mode: independent
  status: pending
  author: codex-agent-sqlite-same-process-shm-observation
  reviewer: null
  refs: []
created: '2026-07-28'
---

# Reconcile same-process SQLite SHM reuse with multi-instance CAS

## Observation

At exact branch head `16f9ad45da6654d63bdef84ed2ed7c423680c724`, opening two
simultaneously live SQLite Store instances on the same current-v3 database fails before the
durable head CAS is reached. The first Store opens successfully. The second Store's active-WAL
readonly probe delegates `xShmMap` with `extend=0` to the pinned SQLite Unix VFS, receives
`SQLITE_OK` with a non-null mapping, applies the contract's `any_native_ok` terminal rule, and
returns `store.sqlite-failure / database / disk I/O error`.

The same state causes the materialization Store race fixture to fail. Its prepared transaction
retains one Store while the competing transaction opens the same database. The competitor cannot
open and therefore cannot publish.

The existing active-WAL fixture does not reproduce this state. On Linux it closes its SQLite
holder and retains only an OFD read lock on SHM, so the process has no pre-existing SQLite writable
SHM mapping when the readonly probe starts.

## Working mental model

SQLite's Unix SHM implementation shares an inode-bound mapping within one loaded SQLite runtime.
When a writer connection in the process already owns that mapping, a later readonly connection can
receive `SQLITE_OK` and the existing non-null mapping even though the later call delegated
`extend=0` and did not create or resize SHM.

The blanket `any_native_ok` rule correctly rejects an unqualified backend that grants a new
writable mapping, but it also rejects this same-runtime reuse. Native status and a non-null pointer
alone do not distinguish those cases. A safe exception would need authority that predates the
reader delegation and proves that the returned mapping is the already-live mapping for the exact
same runtime, VFS, filesystem object family, and namespace epoch.

## Mismatch or opportunity

Integrated-design Store semantics and ADR 0013 require publication CAS to remain atomic across
Store instances, SQLite connections, and processes. A losing writer after another Store commits
must roll back and return `store.publication-conflict`.

ADR 0097 and the SQLite contract separately require every native `SQLITE_OK` in the qualified
readonly-SHM profile to be a terminal backend protocol violation. On the observed pinned Unix VFS,
those requirements cannot both hold for two live same-process Store instances. Silently allowing
all `SQLITE_OK` results would weaken the nonmutation invariant; silently dropping multi-instance
support would violate public compatibility and durable CAS semantics.

Production multi-instance Store activation and the affected materialization race path are blocked
until authority resolves the contradiction. The DF-0202 disposable qualification-only capability
and other work that cannot activate this path may proceed independently.

## Evidence

- Clean exact-head build: `unit.sdk-store` deterministically failed at
  `independent SQLite CAS stores unavailable`.
- A diagnostic-only error projection showed that the first Store was valid and the second returned
  `store.sqlite-failure / database / disk I/O error`.
- A diagnostic-only callback trace reached the qualified main handle's `xShmMap`, observed native
  `SQLITE_OK` plus a non-null mapping, and then reached the existing protocol-violation terminal
  latch. Closing the first Store before opening the second removed the failure.
- GitHub Actions run `30290229624`, job `build-test (OFF)` reproduces both the multi-instance Store
  failure and the materialization competitor failure from the committed tree.
- Integrated design §11 states that a later writer from another Store instance must lose the
  durable head CAS without updating process state.
- ADR 0013 requires WAL, `synchronous=FULL`, and database head CAS to be atomic across multiple
  connections and processes, and names the multi-instance CAS test as verification.
- ADR 0097 and `schemas/cxxlens_ng_sqlite_store_contract.yaml` require qualified readonly-SHM
  native `SQLITE_OK` to fail closed, while preserving only CANTINIT/null and READONLY/non-null.
- The canonical observation is tracked by
  <https://github.com/horiyamayoh/cxxlens/issues/205>.

Diagnostic edits and temporary build paths are reproduction aids only. They are not authority,
qualification artifacts, or a production implementation.

## Alternatives and trade-offs

1. Define a nonforgeable process-local writer-mapping lease. Mint it before the reader delegation
   only from an owned writer mapping, bind exact runtime/VFS/image/callback and main/WAL/SHM
   object-entry identities, page/size/pointer and retained lifetime, and accept native
   `SQLITE_OK`/non-null only when the returned mapping and pre/post no-resize epoch match that
   lease. This preserves multi-instance semantics, but pointer reuse, stale lease, unmap, fork,
   VFS unregister, replacement, and ABA counterexamples must fail closed.
2. Share one Store implementation or SQLite connection between same-process opens. This can make
   the current unit test pass by serialization, but it does not satisfy the explicit
   multiple-connection authority and would hide rather than exercise durable CAS.
3. Perform active-WAL classification in an isolated helper process or independent SQLite loader
   namespace. This avoids same-runtime SHM reuse but adds process/transport/runtime authority and
   portability costs not present in the current Store contract.
4. Permit every qualified native `SQLITE_OK`, or remove same-process multi-instance support. Both
   weaken an accepted invariant or public compatibility and are rejected without an explicit
   higher-authority decision.

## Recommendation

Develop Alternative 1 as an exact authority proposal, but do not treat this record as permission
to implement the exception. The proposal must define:

- the lease mint, registry, lookup, transfer prohibition, PID and lifetime model;
- exact runtime, underlying VFS implementation/app-data/image/callback, filesystem profile,
  retained parent, main/WAL/SHM object and directory-entry bindings;
- page number, page size, returned pointer identity, pre-existing mapping proof, and no
  initialize/create/truncate/extend/delete/resize pre/post receipt;
- unmap and last-writer release ordering, stale and pointer-reuse rejection, fork behavior,
  VFS unregister/runtime unload behavior, identity replacement and namespace-watch loss;
- the original fail-closed rule when any field is absent, mismatched, ambiguous, or cannot be
  rechecked;
- bounded positive tests for two Store instances and a materialization winner/loser race, plus
  negative tests for every listed counterexample and cross-process qualification.

Any accepted change must update integrated design, ADR 0013 and ADR 0097, the SQLite/Snapshot
contracts and schema mirrors, checker expectations, source-negative tests, design checksums, and
traceability before production implementation. Because this is an invariant and compatibility
change, an independent counterexample review is required on Issue #205.

## Disposition

2026-07-28: Observation recorded from Issue #181 after exact-head CI and a clean local
reproduction showed that SQLite's same-process Unix SHM mapping reuse makes the blanket
`any_native_ok` rule incompatible with accepted multi-instance CAS semantics. Production
multi-instance Store activation and the affected materialization race remain blocked. No public
semantic, contract, source behavior, or qualification profile is changed by this record.
