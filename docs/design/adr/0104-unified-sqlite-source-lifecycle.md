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
`-> eager-decode -> logical-read-receipt -> connection-revoking -> connection-closed`.

From `no-effect-boundary-armed` through `logical-read-receipt`, the target family cannot be created,
deleted, truncated, resized, renamed, or written. Census uses retained-parent, no-follow typed
enumeration/open/stat and never re-resolves a host path. Cold active-WAL reading may receive authentic
`SQLITE_READONLY_CANTINIT/null` and build SQLite's private heap WAL index under `WAL_READ_LOCK(0)`.
Native `SQLITE_OK`, including null, is fail-closed unless the nested #205 callback path validates.
Decode is eager and complete while connection, locks, held objects, namespace epoch, and all use
owners remain pinned. Only then can #201 seal a logical read receipt.

The receipt binds runtime image/source ID/build options, VFS callbacks/app-data, filesystem/mount,
retained parent, main/WAL/SHM/journal object and directory-entry identities, pre-`xOpen` namespace
epoch, WAL header/salts/authoritative prefix, decoded logical state and page/census projection, and
the complete mapping/private-index provenance. Empty, unresolved, conflicting, and corrupt are
distinct terminal values.

## #205 nested mapping subprotocol

Each writer or reader `xShmMap` callback follows:

`callback-admitted -> pre-callback-sequence-cut -> attempt-pin-held -> native-started`
`-> native-outcome-captured -> pending-mapping-receipt -> identity-validated`
`-> mapping-lease-promoted -> eager-use-owner-held -> handoff-sealed`.

Before delegation there is only a callback-local attempt pin and writer/reader cohort in-flight pin.
There is no registry lease or pending mapping. Exact native `SQLITE_OK` with a non-null pointer makes
a non-authoritative pending receipt. Promotion occurs only after page, size, pointer, extend pair,
mapping generation, process instance, PID/fork generation, runtime/VFS/filesystem/file-family,
SHM object/entry/mount, namespace watch, and all current Store writer gates validate. Simultaneous
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
5. delegate at most one authenticated native unmap/close for its owner;
6. capture the exact outcome, retire generation and registry entries, then release pins/cleanup.

A native-started callback that returns after the cut remains an original-callback drain only. It
cannot publish a mapping, successor, or fresh cleanup authority. Same-thread/reentrant retirement
returns the exact outer `SQLITE_IOERR` and permanently quarantines the handle/lease. Unknown callback,
unmap, close, unload, or cleanup outcome also creates a permanent non-reusable quarantine tombstone;
there is no retry or reconstruction. Revoke always precedes cleanup and VFS unload.

## #202 independently reviewed effect profile

The effect machine is not entered by raw census. Its sole entry is:

`logical-read-receipt(exact-empty) -> receipt-revalidated -> effect-profile-capability-sealed`
`-> exclusive-normalization-owner -> pre-effect-sealed -> effect-journal-open`
`-> permitted-callback-effects -> file-and-parent-durable -> confirmed-close`
`-> post-close-census -> normalization-receipt -> ordinary-fresh-init`.

The accepted DF-0202 fixture authority and its closed `F0`, `FZ-pre`, `FZ-post`, `FP`, `FH`, `FI`,
and `FO` partitions remain exact; this ADR does not merge, rename, or broaden them. `F0/FP/FH/FZ-pre`
may reach the fixture normalizer only through their required cleanup/recovery. `FO/FI/FZ-post` may
reach only an independently validated rollback-empty fresh anchor. No path infers cold operation
history, a completed edge, or success.

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
| eager decode | locks, mapping/private-index receipts, decoded values | read receipt before complete decode |
| effect pre-seal | exact-empty logical receipt and profile capability | post-effect identity or fresh success |
| terminal/quarantine | original outcome, full owner census, cleanup evidence | values from an unentered later phase |

## Crash/effect matrix

| Interruption | Allowed state | Recovery |
| --- | --- | --- |
| before/inside #201 read or map | source unchanged | revoke, drain, unmap/close, typed fail-closed |
| callback returns after cut | source unchanged by reader | original drain only; permanent quarantine on ambiguity |
| fork/PID reuse/VFS unload/replacement/ABA | source unchanged by reader | hide/revoke before cleanup; no successor until census drains |
| before #202 effect | source unchanged | release exclusive owner; discard receipt |
| during fixture normalization | exact DF-0202 old or journaled recoverable state | follow F0/FZ-pre/FZ-post/FP/FH/FI/FO matrix; no blind retry |
| after durable normalization before fresh init | durable exact-empty rollback state | consume same receipt once or reclassify |
| during fresh init | existing atomic Store initialization states | existing recovery; no intermediate public success |
| CAS loss or phase-opaque commit | atomic old/new or unknown | preserve Store conflict/unknown semantics; lease/effect failures are not CAS conflicts |

## Counterexamples and acceptance

Reject first-map mutation, census-selected normalization, pre-delegation lease, `OK+null`, different
pointer, incomplete member/use-owner census, stale/ABA/fork/PID/VFS lease, unload before revoke,
callback retry, inferred cleanup, cross-branch fallback, fixture-to-production promotion, missing
parent fsync, non-empty normalization, sidecar ambiguity, and CAS reclassification.

The constructibility witness must cover cold active-WAL read, private-index fallback, nested native
mapping success/failure, multi-page attachment, two-live-Store CAS, all revoke/unload/fork/ABA and late
callback cuts, and every DF-0202 callback/recrash partition. #201/#205 may be accepted together only
after an exact-candidate P0/P1-zero review. #202 requires a distinct later review receipt and remains
production-inactive even if #201/#205 are accepted.

## Review history

The review of `75b233e3c3dcf1e2c636b06313e7511bbb86c54c` rejected the sibling-machine
proposal with five P1 findings. This redraft resolves their ordering directionally but remains
Proposed pending machine-checker evidence and fresh independent review.
