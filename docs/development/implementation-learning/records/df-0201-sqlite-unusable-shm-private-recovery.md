---
id: DF-0201
title: Qualify nonmutating SQLite WAL shared-memory recovery
status: accepted
kind: contract-contradiction
impact: invariant
confidence: high
implementation_disposition: may-proceed
scope:
  - store.sqlite-forwarding-vfs-shm-map-contract
  - store.sqlite-active-wal-nonmutating-shm-routing
  - store.sqlite-migration-crash-recovery
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md
  - schemas/cxxlens_ng_sqlite_store_contract.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
tracking_issue: '#201'
implementation_issues:
  - '#181'
resolution_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md
  - schemas/cxxlens_ng_sqlite_store_contract.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
review:
  mode: independent
  status: complete
  author: codex-agent-df0201-authority-resolution
  reviewer: codex-agent-sqlite-nonmutating-shm-capability + codex-agent-sqlite-shm-epoch-falsification
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/201#issuecomment-5040167571
created: '2026-07-22'
---

# Qualify nonmutating SQLite WAL shared-memory recovery

## Observation

The default forwarding VFS denies source mutation during an existing-v2 read. In particular, it
changes SQLite's `xShmMap(..., extend=1, ...)` request into a delegated `extend=0` observation.
The underlying VFS may then legally return `SQLITE_OK` with a null mapping. Passing that result
back as success lets SQLite dereference a null WAL-index page. Returning exact primary
`SQLITE_READONLY` is also insufficient: SQLite's WAL-index allocation path converts that exact
code to success. Returning extended `SQLITE_READONLY_CANTINIT` keeps the output null and makes the
open fail safely.

After that fail-safe was applied, the process-crash migration fixture no longer crashed, but its
cold predecessor-v2 reopen failed with `attempt to write a readonly database`. The source had a
present, readable, regular WAL/SHM pair, yet that SHM state was not usable by a cold read-only
connection without initializing or extending the WAL index.

## Working mental model

WAL and SHM presence, readability, object kind, and stable identity are necessary source
classification evidence. They do not prove that a new process can initialize a usable SQLite
WAL-index mapping without writing SHM. SHM coordination bytes are explicitly non-authoritative;
the committed WAL prefix and independently decoded logical Store state are the authority.

A source-mutation-free reader therefore needs to choose its route before the first potentially
effectful source `xShmMap`. The preferred route is a qualified VFS capability that opens/maps the
source SHM without initializing, truncating, extending, or resizing it. When page 0 is unreliable,
that capability returns authentic `SQLITE_READONLY_CANTINIT` on the same SQLite connection and
allows SQLite to construct its own private heap WAL index while holding `WAL_READ_LOCK(0)`.

An external private workspace is not a safe after-failure fallback. The first delegated
`xShmMap(extend=0)` may already have changed source SHM, and post-close identity/digest endpoint
checks do not establish a coherent main/WAL epoch across a live writer, WAL reset, or A-to-B-to-A
change. External recovery would require a separately specified typed epoch lease acquired before
the active open and retained until capture completes.

## Mismatch or opportunity

ADR-0097 and the SQLite Store contract currently classify every readable WAL+SHM pair as the
active-WAL route. That route requires a source connection, one explicit read transaction, an
observed SHM read-lock slot, and complete eager decode. The same authority already defines a
private recovery route for main+WAL with no SHM, but does not define what happens when SHM exists
and is stable yet cannot initialize a cold read-only WAL index.

Treating the active route's `SQLITE_READONLY_CANTINIT` as a generic open failure would be safe but
would violate the required subprocess-crash qualification: a valid committed predecessor state
must cold reopen to the exact logical v2 state or to a fully validated authorized v3 descendant.
Treating `SQLITE_OK` plus null as success, allowing source SHM extension, or weakening the crash
oracle would violate safety and zero-mutation invariants.

The gap is earlier than the originally observed `extend=1` request. SQLite's Unix VFS may perform
DMS initialization and truncate source SHM while servicing the first `extend=0` map when no other
process holds the DMS lock. Suppressing only a later extension request therefore does not prove
the existing active route source-mutation-free.

## Evidence

- `src/sdk/sqlite_default_forwarding_vfs.cpp::forwarding_shm_map` suppresses SHM extension while
  observing the existing source and binds SHM identities.
- `src/sdk/store.cpp` separates `sqlite_active_wal_read_anchor` from
  `sqlite_wal_source_capture`; only the latter owns a private recovery workspace.
- `tests/unit/sdk/store_test.cpp` contains active-WAL, WAL-only, and subprocess migration-crash
  routes. The minimized failure is a cold direct `read_v2_publications()` after the crash fixture,
  independent of an earlier Store handle's close lifecycle.
- SQLite documents `SQLITE_READONLY_CANTINIT` as the result used when shared memory cannot be
  initialized and permits a private in-memory WAL-index fallback:
  <https://sqlite.org/rescode.html#readonly_cantinit>.
- SQLite's `walIndexPageRealloc()` accepts a null page-0 mapping on `SQLITE_OK` and converts exact
  `SQLITE_READONLY` from `xShmMap` to success; an extended readonly code is therefore required for
  an unusable null mapping:
  <https://www3.sqlite.org/matrix/ev/src/wal.html>.
- SQLite's Unix VFS may initialize and truncate SHM during its first shared-memory map. A wrapper
  cannot make that earlier effect safe by changing only a later `extend=1` request:
  <https://sqlite.org/src/artifact/410185df49>.
- The accepted zero-mutation oracle forbids source main/WAL/journal byte changes and SHM
  create/delete/resize outside explicit compact. The migration crash matrix independently
  requires exact logical v2 or validated v3 recovery.

## Alternatives and trade-offs

1. Add a qualified source-SHM read capability that forbids DMS initialization/truncation/resize
   from the first map, returns `SQLITE_READONLY_CANTINIT` for unreliable page 0, and retains the
   same connection for SQLite's heap WAL-index recovery under `WAL_READ_LOCK(0)`. This best
   preserves the existing active snapshot model, but requires a portable typed capability and
   exact real-VFS qualification rather than an undocumented URI parameter.
2. Acquire a typed coherent source-epoch lease before any SQLite source map, then copy held main
   and the bounded authoritative WAL prefix to a private workspace while retaining the lease.
   This can support external private recovery but must close live-writer/checkpoint/reset,
   replacement, A-to-B-to-A, watch-overflow, and lock-loss cases. Endpoint rechecks alone are
   insufficient.
3. Always route every readable WAL+SHM source through an unleased private copy. This is rejected:
   it can combine incoherent main/WAL epochs in the presence of a live writer or WAL reset.
4. Return a typed open failure whenever source SHM is unusable. This is memory-safe and simple but
   fails the accepted crash-recovery requirement for a recoverable committed state.
5. Allow SHM extension on the source or return success with a null mapping. Both are rejected: the
   former violates source zero mutation and the latter is memory-unsafe.
6. Weaken the subprocess-crash expected state. This is rejected because it shrinks accepted
   authority to match an implementation gap.

## Recommendation

Prefer alternative 1, conditionally bound as `sqlite-source-shm-readonly-unix-uri-v1`. After the
bound nonmutating census has identified readable WAL+SHM, but before the first underlying SQLite
source `xOpen`, map, or authority read, require a behavioral qualification of the exact runtime,
loader-origin Unix default VFS (or typed exact equivalent), and filesystem profile using a scratch
WAL/SHM fixture that never accesses the target source. Name or URI spelling alone is insufficient.
Resolve `sqlite3_sourceid`, `sqlite3_uri_parameter`, and `sqlite3_uri_key` from the same pinned
runtime handle only after that active-WAL census; quiescent exact-v2 diagnostic reads retain the
base-symbol path.
Open only the strict application-generated URI
`file:<uppercase-percent-encoded-canonical-absolute-path>?mode=ro&cache=private&readonly_shm=1`
with `READONLY|URI|PRIVATECACHE|FULLMUTEX`; prohibit CREATE, `vfs`, `immutable`, user query, and
unknown parameters. The owned main `xOpen` callback must validate the exact URI receipt before it
delegates the underlying source open.

Resolve every qualification producer/cold/active locator through a retained directory descriptor
and a candidate-only exact `xFullPathname` arm; host-path re-resolution is not qualification
evidence. The held target main, WAL, and SHM objects must each expose the same typed filesystem
profile as the retained parent and scratch fixture. Normalize every unavailable or failed
qualification to `{store.backend-unavailable, sqlite, source-shm-readonly-qualification}` without
exposing an internal stage.

Keep the logical target URI canonical, but project native main/WAL/SHM resolution through one
receipt-bound retained parent descriptor. Require each target main/WAL/SHM leaf to be a direct
regular directory entry; reject symlinks and other indirection before any native callback. Hold a namespace-only watch from before target
`xFullPathname`/`xOpen` through the end of eager decode, excluding content modification and
attribute events that a legitimate writer may produce. Check the watch and an fd-relative exact
census before and after native map delegation and before accepting the decoded state. A watch
event, loss, queue overflow, or identity drift must release any native mapping with a non-removing
unmap and fail closed. Endpoint equality alone is insufficient for leaf or ancestor A-to-B-to-A
replacement.
After the epoch starts, do not re-resolve the logical host path even for census or identity
receipts; use only the retained parent descriptor and held-object receipts.

The qualified wrapper must delegate both the first and every later native `extend=0` map. A caller
`extend=1` is delegated as `extend=0` on first and later calls; the extension request is never
passed through.
`SQLITE_READONLY_CANTINIT` plus null and exact `SQLITE_READONLY` plus a non-null mapping are the
only accepted readonly-SHM outcomes; `READONLY` plus null normalizes to CANTINIT. Any native
`SQLITE_OK` in this qualified profile, regardless of mapping, is a backend protocol violation and
fails closed rather than being translated. A writer attachment may produce the authentic
CANTINIT/null to READONLY/non-null transition. READONLY-family state must never become a permanent
delegation-suppression latch and resets only after a successful delegated unmap. A generic
non-profile caller's `extend=0, SQLITE_OK` semantics remain separate and legal.

Keep the same connection through SQLite's private heap WAL-index recovery and bind its
`WAL_READ_LOCK(0)`, held main/WAL/SHM identities, WAL header/salt, and complete eager decode to the
receipt. Failure to establish the qualified capability, loss of the lock, identity drift, or any
non-CANTINIT SQLite error fails closed. Do not close and reopen into a digest-only fallback.

Alternative 2 remains available only if an independent authority amendment specifies a continuous
pre-map epoch lease. It must copy from already-held objects, omit source SHM bytes, create
coordination files only in its private directory, and accept only exact v2 or an authorized,
fully validated current-v3 descendant. The current WAL-only route remains SHM-absent-only.

Before acceptance, independently review the exact ADR-0097, SQLite/Snapshot Store contract,
schema-mirror, checker, and negative-test diff. Falsification must cover qualification before the
underlying target-source open, first/later-map SHM bytes/size, CANTINIT/null, READONLY/non-null,
READONLY/null normalization, native OK as a protocol violation, CANTINIT-to-READONLY writer attach,
continued delegation, successful-unmap-only reset, and generic non-profile `extend=0` OK semantics.
It must also cover URI receipt mismatch, invalid/torn WAL, live-writer checkpoint/reset races,
main/WAL/SHM replacement and A-to-B-to-A, lock/watch loss, and subprocess-crash cold reopen with
exact v2/v3 projection and zero source mutation. Checker negatives must reject qualification or
receipt removal, post-close endpoint-only fallback, different-connection or arbitrary-error
fallback, and weakening of the same-connection `WAL_READ_LOCK(0)` eager-decode receipt.

## Disposition

2026-07-22: Investigation opened from Issue #181 and tracked by Issue #201 after the forwarding
VFS crash was reduced to a successful null SHM mapping and the safe CANTINIT response exposed the
active-WAL routing gap. The forwarding VFS may proceed with the memory-safety fail-safe and its
focused fake/real VFS tests. SQLite migration/release activation and any active-WAL private
recovery implementation remain blocked until the routing authority and independent review are
complete.

2026-07-22: Independent design review rejected the initial close-after-CANTINIT private-workspace
proposal. The first Unix-VFS `xShmMap(extend=0)` may initialize and truncate source SHM before the
wrapper observes a later denied extension, and post-close endpoint equality cannot prove a
coherent epoch against a live writer. The preferred investigation path is now a pre-map qualified
nonmutating SHM capability plus SQLite's same-connection heap WAL-index recovery. DF-0201 remains
`investigating / blocked`; no routing alternative is accepted yet.

2026-07-22: `codex-agent-sqlite-nonmutating-shm-capability` and
`codex-agent-sqlite-shm-epoch-falsification` independently accepted only the conditional Option A
shape recorded above and rejected raw/native OK translation, name-only `readonly_shm` trust, and
post-close capture. The first authority patch binds the capability ID, strict internal URI,
loader/runtime/VFS/filesystem behavioral qualification, callback receipt, native map transition
protocol, same-connection lock-zero eager decode, and no-fallback rule. Review remains `pending`
and implementation remains `blocked` until a fresh independent reviewer accepts this exact patch;
Issue #201 stays open as the canonical review thread.

2026-07-22: A fresh independent review of the exact integrated-design, ADR, SQLite/Snapshot
contract, schema-mirror, checker, and negative-test diff returned ACCEPT. It confirmed that the
authority does not admit generic dynamically loaded SQLite: the exact runtime/VFS/filesystem
profile must pass target-independent behavioral qualification before the target source `xOpen`,
and every unavailable or mismatched capability fails closed. The review also independently
mutated the permanent latch, native-OK translation, READONLY transition, generic non-profile OK,
different-connection fallback, and arbitrary-error fallback rules and observed fail-closed
checker rejection. DF-0201 is therefore `accepted / may-proceed`; release activation remains
blocked on the implementation and real-VFS qualification evidence required by the accepted
contract.

2026-07-22: Implementation falsification found that endpoint-only SHM identity checks did not
close native-callback or ancestor A-to-B-to-A replacement, and that a parent-only filesystem
profile could misqualify bind-mounted target objects. The accepted Option A contract was
strengthened before implementation acceptance with retained-fd scratch and target resolution,
per-object filesystem-profile equality, a target namespace epoch watch, pre/post native-map
census checks, and one stable qualification-failure tuple. These changes narrow the accepted
capability and do not authorize a new fallback or public C++ surface. Fresh independent review of
the implementation and this exact strengthening remains required before release activation.

2026-07-22: A second implementation falsification showed that a retained parent descriptor does
not constrain a leaf symlink's target ancestry. Option A therefore accepts only direct regular
main/WAL/SHM entries. A stable symlink or symlink-target A-to-B-to-A must fail before native access;
this is a fail-closed narrowing of the same capability, not a new fallback or public surface.

2026-07-22: The same review found logical-path identity rechecks in the forwarding VFS and Store
after the retained epoch began. Those checks could touch a transient replacement namespace even
when native SQLite remained anchored. The contract now forbids all such host-path re-resolution
after epoch start and binds census to the retained parent and held-object receipts.
