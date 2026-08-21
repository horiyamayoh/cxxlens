# ADR 0104: SQLite zero-effect read, nested mapping lease, and isolated normalization effect

- Status: Proposed for independent review
- Date: 2026-08-21
- Owners: #201 and #205; separate effect-profile owner #202
- Contract IDs: `store.sqlite-read-mapping-lifecycle.v1`, `store.sqlite-exact-empty-normalization-effect.v1`
- Production activation: fail-closed
- Production qualification: not claimed

## Context and ownership

The prior sibling-branch model was not constructible. #201 owns the zero-source-mutation active-WAL
read connection. #205 is an authenticated same-process mapping subprotocol nested inside that live
connection; it is not a sibling source outcome. #202 is a later persistent-effect profile that may
start only from an already sealed logical exact-empty read receipt. A raw physical census never
authorizes it.

No receipt substitutes for another. #201 cannot authorize normalization, #205 cannot prove logical
database validity or CAS, and #202 cannot mint or prolong a mapping lease. Production canonical or
user-source normalization remains disabled until the separate #202 effect profile has an exact
candidate P0/P1-zero review and authenticated acceptance receipt.

## #201 outer zero-effect machine

The no-effect boundary begins before target `xOpen`, not after census:

`unresolved -> runtime-vfs-filesystem-sealed -> retained-parent-held`
`-> no-effect-boundary-armed -> typed-family-census -> active-read-connection-open`
`-> wal-lock-and-prefix-held -> mapping-subprotocol-or-private-index`
`-> eager-decode -> decoded-read-candidate-sealed -> connection-revoking`
`-> outer-custody-join-pending -> outer-custody-join-sealed -> connection-closed`
`-> zero-effect-callback-receipt-sealed -> logical-read-receipt`.

From `no-effect-boundary-armed` through `logical-read-receipt`, including every map, unmap, and
close callback, the target family cannot be created,
deleted, truncated, resized, renamed, or written. Census uses retained-parent, no-follow typed
enumeration/open/stat and never re-resolves a host path. Cold active-WAL reading may receive authentic
`SQLITE_READONLY_CANTINIT/null` and build SQLite's private heap WAL index under `WAL_READ_LOCK(0)`.
Native `SQLITE_OK`, including null, is fail-closed unless the nested #205 callback path validates.
Decode is eager and complete while connection, locks, held objects, namespace epoch, and all use
owners remain pinned. Decode first seals a non-public candidate. The connection then revokes and
drains all custody, calls authenticated `xShmUnmap(deleteFlag=0)` and `xClose`, and seals an exact
zero-create/write/truncate/extend/delete/resize callback-effect transcript. Only after connection
closure, zero live callbacks/leases/use owners, and that zero-effect receipt can #201 seal the
logical read receipt.

The receipt binds runtime image/source ID/build options, VFS callbacks/app-data, filesystem/mount,
retained parent, main/WAL/SHM/journal object and directory-entry identities, pre-`xOpen` namespace
epoch, WAL header/salts/authoritative prefix, decoded logical state and page/census projection, and
the complete mapping/private-index provenance. Empty, unresolved, conflicting, and corrupt are
distinct terminal values.

## #205 nested mapping subprotocol

Writer and reader callbacks have different authentic authority and different post-native products.
A writer callback follows:

`callback-admitted -> pre-callback-sequence-cut -> attempt-pin-held -> native-started`
`-> native-outcome-captured -> pending-mapping-receipt -> identity-validated`
`-> mapping-lease-promoted -> eager-use-owner-held -> handoff-sealed`.

A reader callback follows:

`reader-session-reserved -> writer-lease-page-support-pin-held -> native-started`
`-> native-outcome-captured -> reader-attachment-candidate -> identity-and-effect-validated`
`-> reader-attachment-group-promoted -> eager-session-owner-admitted -> reader-handoff-sealed`.

A reader never enters `mapping-lease-promoted`, mints writer/page authority, or admits another page
transitively. Its product is one reader attachment group/handoff plus the exact session owner.

Before writer delegation there is only a callback-local attempt pin and writer cohort in-flight pin.
There is no new registry lease or pending mapping. A reader callback, however, must first acquire a
fresh, page/range-specific pre-delegation pin from an already-live authenticated local writer mapping
lease and its sealed page-support receipt. Absence, retirement, ambiguity, or cross-process ownership
rejects the reader before native entry. Exact native `SQLITE_OK` with a non-null pointer makes
a non-authoritative pending receipt. Promotion occurs only after page, size, pointer, extend pair,
mapping generation, process instance, PID/fork generation, runtime/VFS/filesystem/file-family,
SHM object/entry/mount, namespace watch, the exact zero-effect callback receipt, and all current Store
writer gates validate. Simultaneous
first writers may install or join one exact generation only through CAS on the complete key.

The production exception can translate exactly one promoted native `SQLITE_OK/non-null` to
`SQLITE_READONLY` with the identical pointer. `OK/null`, different pointer, unknown extend, missing or
stale receipt, cross-process/fork, or a generation/page not in the sealed member set is rejected and
never exposes a mapping. A reader attachment can contain multiple independently validated page map
receipts, one member per unique generation/page tuple, but owns exactly one attachment handoff and
one cleanup owner.

## Lifetime, ABA, callback, and cleanup order

Every open/attempt/pending/lease/attachment/use-owner/cleanup owner carries a move-only runtime/VFS
image lifetime pin plus process-instance and fork generation. Pointer/path/PID equality is never
identity. The writer-map stat epoch is armed before pre-stat. Watch loss/overflow, A-to-B-to-A,
object/directory replacement, WAL salt/reset, resize, close, unload, unregister/re-register, fork, or
PID reuse blocks new admissions and revokes the full generation.

The only teardown order is:

1. atomically hide the generation and seal a pre-callback sequence cut;
2. revoke new mapping and use-owner admission;
3. retain lifetime pins while all already admitted callbacks and eager-use owners drain;
4. seal the complete member/use-owner census;
5. delegate at most one authenticated native `xShmUnmap(deleteFlag=0)` and capture its outcome;
6. only after confirmed `xShmUnmap == SQLITE_OK`, consume the distinct close owner and call `xClose`;
7. after confirmed close, seal the callback-effect transcript, retire generation and registry
   entries, then release pins/cleanup. Non-OK, throw, timeout, or indeterminate unmap installs a
   terminal opaque quarantine and performs zero close, retry, or reconstructed cleanup.

A native-started callback that returns after the cut remains an original-callback drain only. It
cannot publish a mapping, successor, or fresh cleanup authority. Same-thread/reentrant retirement
returns the exact outer `SQLITE_IOERR` and permanently quarantines the handle/lease. Unknown callback,
unmap, close, unload, or cleanup outcome also creates a permanent non-reusable quarantine tombstone;
there is no retry or reconstruction. Revoke always precedes cleanup and VFS unload. Every callback
receipt proves zero initialize/create, write, truncate, extend, delete, and resize effect; identity
continuity alone is never such proof.

Reader attachment retirement repeats the same cut/census discipline independently: hide generation,
seal its pre-callback cut, revoke admission, drain callbacks and use owners, seal the complete member
census, perform one authenticated `xShmUnmap(0)`, consume its distinct close owner, seal close outcome,
callback-effect transcript, and cleanup acknowledgement, then release the page-support pin and seal
only `reader-retired`. It never retires or makes a claim about an independently live writer attachment.
The outer connection separately joins all reader terminals with all writer terminals owned by that
outer connection. It enters `outer-custody-join-pending`, compares the exact outer-owned writer set
with the retired-writer set and the exact outer-owned reader set with the retired-reader set, and
only then seals `outer-custody-join-sealed`. Unrelated live writers are explicitly outside that
census; a missing writer terminal or reader terminal blocks the join, and reader retirement alone
cannot satisfy it. Enrollment is an atomically sealed set of `(custody kind, instance ID,
outer-generation digest)` records. The join requires an authenticated terminal receipt for every
enrolled instance. A terminal receipt is a nonserializable capability minted only by the lifecycle
issuer when that exact enrolled instance reaches `retired` or permanent `quarantined`; a digest over
public enrollment fields is not authentication. The join rejects omitted additional instances and fabricated or duplicate receipts, and
does not reduce the census to one row per custody kind. VFS unload has an executable causal path
`request -> revoke -> join-pending -> join-sealed -> unload-permitted -> unloaded`; no declarative
terminal label can bypass that path.

Fork is not an ordinary drain. `pthread_atfork` prepare seals admission and records the complete
custody census; the parent handler revalidates process/fork generation before resuming. The child
handler atomically moves all inherited connection, callback, mapping, cleanup, and lifetime custody
to `child-inherited-custody-quarantine`. That child terminal permits no SQLite entry, native unmap,
close, retry, cleanup, or authority reconstruction; vanished parent-thread owners are never awaited.
Only child process exit/exec may discard the private inherited address-space copy.

## #202 independently reviewed effect profile

The effect machine is not entered by raw census or a decoded candidate. Its sole entry is:

`logical-read-receipt(exact-empty, connection-closed, zero-live-custody, zero-effect-receipt)`
`-> receipt-revalidated -> effect-profile-capability-sealed`
`-> exclusive-normalization-owner -> pre-effect-sealed -> effect-journal-open`
`-> permitted-callback-effects -> file-and-parent-durable -> confirmed-close`
`-> post-close-census -> normalization-receipt -> ordinary-fresh-init`.

The accepted DF-0202 fixture authority remains an executable closed partition, not a label census:

- `F0 -> live-receipt -> fixture-normalizer`.
- `FP/FH -> authenticated cleanup-or-recovery -> independently-revalidated F0 -> new-live-receipt`.
- `FZ-pre -> retain-and-revalidate-the-exact-size-zero-coordination-WAL`
  `-> fixture-normalizer-with-same-coordination-WAL`
  `-> authenticated-coordination-WAL-delete -> retained-parent fsync`
  `-> journal-created-and-parent-fsynced -> valid-journal-and-main-write`
  `-> terminal-journal-delete-and-parent-fsynced`
  `-> confirmed-close -> post-close-census -> normalization-receipt`.
  The coordination WAL is retained through the same normalizer coordination object; deletion is
  completed and its parent durability sealed before journal creation or main write. A crash or indeterminate effect enters
  `recoverable-interruption`, is cold-reclassified through the seven-family classifier, and cannot
  continue or mint the original FZ-pre receipt.
- `FI/FZ-post/FO -> independently-validated rollback-empty-fresh-anchor` only; none is a completed
  normalization edge and none may mint or reuse a live receipt.

Each family has `pre-effect`, `effect-admitted`, `recoverable-interruption`, `recrash-classified`, and
its listed terminal route. A recrash re-enters the same seven-family classifier from durable bytes;
it cannot resume from in-memory phase, merge families, or cross-route to success.

`normalization-receipt` is a live uninterrupted terminal only. After any recoverable interruption,
the original family has no success edge: durable bytes are reclassified and only the newly selected
family authority applies. In particular, post-form plus zero WAL becomes `FZ-post`, never an `F0` or
`FZ-pre` success continuation. Family-specific cleanup and parent-sync steps dominate every handoff.

Eligibility binds the original accepted-empty recovery receipt, same main object/entry, continuous
namespace epoch, exclusive lock, exact pre-main bytes, VFS/runtime/device/build profile, sidecar
census, decoded page size and logical page count, deterministic post-main bytes, exact effect
allowlist, confirmed close, and post-close census. Cleanup is authenticated-known-path unlink through
the retained parent after immediate leaf type/identity observation. A final-check-to-unlink rebind is
effect/durability opaque: no retry, handoff, or second snapshot. Every deletion requires a separate
retained-parent fsync before handoff; journal namespace sync occurs after first journal sync and
before valid header/main writes.

Schema, Store metadata, payload, partition, chunk, counter, head, and publication writes are never
normalization effects. Successful normalization is an internal one-shot receipt consumed by ordinary
fresh initialization, never Store/public success. Fixture-only capability cannot satisfy production.

## Phase-authentic fields

| Phase | Available | Forbidden |
| --- | --- | --- |
| pre-`xOpen` boundary | runtime/VFS/filesystem/retained parent and namespace epoch | target pointer or logical database claim |
| typed census | held file-family identities and physical observations | exact-empty logical receipt or effect eligibility |
| callback attempt | callback cut, attempt/in-flight pin, requested page/size/extend | registry lease |
| native outcome | exact return and pointer | promoted mapping or database validity |
| promoted lease | full mapping/process/runtime/file-family identity and generation | logical read success or CAS |
| eager decode | locks, mapping/private-index receipts, decoded values | public read receipt or effect entry |
| closed read candidate | decoded candidate, closed connection, zero live custody, exact zero-effect callback receipt | normalization before all four predicates |
| effect pre-seal | exact-empty logical receipt and profile capability | post-effect identity or fresh success |
| terminal/quarantine | original outcome, full owner census, cleanup evidence | values from an unentered later phase |

## Crash/effect matrix

| Interruption | Allowed state | Recovery |
| --- | --- | --- |
| before/inside #201 read or map | source unchanged | revoke, drain, unmap/close, typed fail-closed |
| callback returns after cut | source unchanged by reader | original drain only; permanent quarantine on ambiguity |
| fork/PID reuse/VFS unload/replacement/ABA | source unchanged by reader | hide/revoke before cleanup; no successor until census drains |
| before #202 effect | source unchanged and #201 connection closed | release exclusive owner; discard receipt |
| during fixture normalization | exact DF-0202 old or journaled recoverable state | follow F0/FZ-pre/FZ-post/FP/FH/FI/FO matrix; no blind retry |
| after durable normalization before fresh init | durable exact-empty rollback state | discard original receipt; cold-reclassify only; no original success continuation |
| during fresh init | existing atomic Store initialization states | existing recovery; no intermediate public success |
| CAS loss or phase-opaque commit | atomic old/new or unknown | preserve Store conflict/unknown semantics; lease/effect failures are not CAS conflicts |

## Counterexamples and acceptance

Reject first-map mutation, census-selected normalization, writer pre-delegation lease, reader native
entry without a fresh authenticated writer-lease pin, `OK+null`, different pointer, incomplete
member/use-owner census, stale/ABA/fork/PID/VFS lease, unload before revoke, callback effect without
an exact zero-effect receipt, nonzero unmap delete flag, callback retry, ordinary-drain fork handling,
`#202` entry before connection-close/zero-custody, inferred cleanup, cross-branch fallback,
fixture-to-production promotion, missing
parent fsync, non-empty normalization, sidecar ambiguity, and CAS reclassification.

The constructibility witness must cover cold active-WAL read, private-index fallback, nested native
mapping success/failure, multi-page attachment, two-live-Store CAS, all revoke/unload/fork/ABA and late
callback cuts, and every executable DF-0202 callback/recrash partition. Future production activation
additionally requires the repository-tracked harness, exact loaded SQLite DSO identity,
callback/recrash, large-sector, rebind and parent-sync matrices, canonical report digest, a distinct
exact-implementation review, and an explicit Accepted profile replacing the prohibition. Review
receipt alone is insufficient. #201/#205 may be accepted together only
after an exact-candidate P0/P1-zero review. #202 requires a distinct later review receipt and remains
production-inactive even if #201/#205 are accepted.

## Review history

The review of `75b233e3c3dcf1e2c636b06313e7511bbb86c54c` rejected the sibling-machine
proposal with five P1 findings. This redraft resolves their ordering directionally but remains
Proposed pending machine-checker evidence and fresh independent review.
