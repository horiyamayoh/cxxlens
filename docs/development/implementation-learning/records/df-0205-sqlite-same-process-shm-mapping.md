---
id: DF-0205
title: Reconcile same-process SQLite SHM reuse with multi-instance CAS
status: accepted
kind: contract-contradiction
impact: invariant
confidence: high
implementation_disposition: may-proceed
scope:
  - store.sqlite-same-process-shm-mapping
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
tracking_issue: '#205'
implementation_issues:
  - '#181'
resolution_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0013-ng-sqlite-physical-store.md
  - docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md
  - schemas/cxxlens_ng_sqlite_store_contract.yaml
  - schemas/cxxlens_ng_sqlite_store_contract.schema.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.schema.yaml
review:
  mode: independent
  status: complete
  author: codex-agent-sqlite-same-process-shm-observation
  reviewer: codex-agent-sqlite-authority-completion
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/205#issuecomment-5095883584
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
   `SQLITE_OK`/non-null only when the returned mapping and route-specific authenticated
   pre/post size/effect epoch match that lease. This preserves multi-instance semantics, but
   pointer reuse, stale lease, unmap, fork,
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

Alternative 1 is accepted as the exact internal implementation authority
`cxxlens.sqlite.same-process-writer-shm-mapping-lease.v1` by the independent review recorded on
Issue #205. This acceptance permits the declared internal implementation and focused tests, but
does not activate the production exception. The reviewed authority defines:

- a process-global, cross-owned-forwarding-alias registry keyed by one non-reusable process
  instance, the shared loaded-runtime/image/source-id/callback and underlying VFS/app-data
  cohort, the authenticated file family, and a non-reused mapping generation; each writer/reader
  `sqlite_api` keeps a distinct move-only runtime-lifetime pin, whose pointer equality is not a
  cohort key;
- a two-stage writer path: callback-local pre-map attempt and writer-cohort in-flight pin, then
  an exact native `SQLITE_OK`/nonnull post-map receipt and non-authoritative pending state, then
  promotion only after all current-v3 Store writer gates; no registry pending exists before
  delegation, and simultaneous first writers install or join one exact generation;
- a separate writer-map stat-only epoch whose namespace watch is armed before pre-stat:
  pre-existing SHM permits zero events, with `{0,0}` requiring exact pre/post identity and size
  equality and authenticated `{1,1}` requiring exact identity plus either preallocated-range
  zero-size-effect or the exact monotonic size extension; absent SHM permits only authenticated
  `{1,1}` with exactly one expected `IN_CREATE` and the exact direct regular post-create
  object/size. Unavailable, non-Linux, overflow, loss, extra-event, and A-B-A cases cannot mint;
- exact caller/delegated writer `extend` pairs. `{1,1}` requires authenticated RW MAIN_DB plus
  WAL write-lock/effect evidence and exact preallocated zero-effect, direct-create, or
  monotonic-extension observation;
  `{0,0}` requires a pre-existing direct SHM, exact size equality, and a zero-effect transcript;
  `{1,0}`, `{0,1}`, and other values never mint, pend, or join. Pair/effect receipts belong to
  each attempt/resulting holder rather than the generation key, so valid `{1,1}` and `{0,0}`
  holders may cross-join without pair equality. Same-generation new-page/growth atomically
  advances the page set and sealed SHM-size receipt before reader admission;
- retained parent-directory authority plus the existing main/WAL native-file-node/xOpen and SHM
  native-attachment receipts, with no duplicate target FD open/close while SQLite locks may be
  live. Native main/WAL close revokes the lease; a memory pin never makes a closed OS handle
  authoritative;
- reader in-flight-to-handoff ownership, writer and reader callback thread/reentrancy tokens,
  bounded ordered cross-thread retirement, nonblocking same-thread reentrant retirement to exact
  outer `SQLITE_IOERR` plus opaque-handle/lease quarantine, and no fabricated unmap success or
  retry;
- exact-file-family successor-generation exclusion while any page handoff from the prior mapping
  generation remains, including W1/G1 handoff versus W2 same-pointer and controlled-VFS
  different-page rejection, and the exact existing
  `store.sqlite-failure / database / preserve-sqlite-runtime-diagnostic`, retryable-false
  projection;
- a production-only qualified route that may translate one leased native `SQLITE_OK`/nonnull to
  exact `SQLITE_READONLY` with the identical pointer. Qualification scratch/map-sequence
  validation stays leaseless and all native OK results remain terminal there.

The accepted authority is bound in integrated design, ADR 0013 and ADR 0097, the SQLite/Snapshot
contracts and schema mirrors, checker expectations, source-negative tests, design checksums, and
traceability. Because this is an invariant and compatibility change, the exact implementation and
complete counterexample matrix require a distinct independent review before production activation.

## Disposition

2026-07-28: Observation recorded from Issue #181 after exact-head CI and a clean local
reproduction showed that SQLite's same-process Unix SHM mapping reuse makes the blanket
`any_native_ok` rule incompatible with accepted multi-instance CAS semantics. Production
multi-instance Store activation and the affected materialization race remain blocked. No public
semantic, contract, source behavior, or qualification profile is changed by this record.

2026-07-28: The exact proposal was drafted into integrated design, ADR 0013/0097, and identical
SQLite/Snapshot contract and schema-mirror objects. Exact checker digests and mutation negatives
bind its current-rejection rule, post-acceptance projection, pending order, writer namespace
epoch, extend matrix, gate ordering, reader handoff, successor exclusion, target-FD discipline,
and fresh-review boundary. This DF remains `observed`, `implementation_disposition: blocked`,
`resolution_refs: []`, and independent review `pending`. Until that separate review accepts the
proposal, every native `SQLITE_OK` in the qualified readonly-SHM profile remains a terminal
protocol violation and no production or implementation exception is authorized.

2026-07-28: Independent semantic counterexample review of exact proposal commit
`e54d73e2767f06c85ef98de67f003944a962d7de` returned three P1 blockers: its unconditional
pre-existing-SHM size equality contradicted authenticated growth, holder-to-holder extend-pair
equality rejected valid `{1,1}` / `{0,0}` joins, and successor exclusion covered one page rather
than the complete native mapping generation. The revised four-mirror proposal digest
`sha256:7018a853e3053beb5b93cd3713c49eb0350c7f32471822fb9f0f968196100a8f`
closes those three contradictions, adds an old-rule negative for each affected field, and binds
the two mixed-pair join directions plus different-page successor rejection into the
machine-readable qualification matrix. The blocking review is recorded on Issue #205; fresh
independent review of the revised exact commit is still required. This record therefore remains
`observed`, blocked, and non-authorizing.

2026-07-28: Independent semantic and structural review accepted exact proposal commit
`6cb705c256c9576f74b50a2dca8fc4e8f72d06bb` with P0/P1/P2 `0/0/0`; the accepted proposal digest
is `sha256:7018a853e3053beb5b93cd3713c49eb0350c7f32471822fb9f0f968196100a8f`.
DF-0205 is therefore `accepted` with `implementation_disposition: may-proceed`, and the declared
internal registry, writer pending/promotion, reader pin/handoff, callback gates, and focused tests
may be implemented. The current source continues to reject every native `SQLITE_OK` in the
qualified readonly-SHM profile. Production activation remains blocked until the exact
implementation and complete positive/negative counterexample matrix receive their distinct
independent review.

2026-08-17: Exact main `15087378588f18fd69f7c90849785fba4c1b96f0` adds a test-only fail-closed
checkpoint to the leaseless readonly-SHM map-sequence validator. The existing accepted
`CANTINIT/null` and `READONLY/non-null` controls remain positive cases, while both native
`SQLITE_OK/null` and native `SQLITE_OK/non-null` are now explicit negative cases. No source
activation, projection exception, public surface, or qualification claim changed; #205 remains
open and production-blocked.
