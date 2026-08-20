# ADR 0104: Unified SQLite source capability, normalization, and mapping lease lifecycle

- Status: Proposed for independent review
- Date: 2026-08-21
- Owners: #201, #202, #205
- Contract ID: `store.sqlite-source-lifecycle.v1`
- Production activation: fail-closed
- Production qualification: not claimed

## Context

#201, #202, and #205 describe different phases of one file-family lifecycle. Reviewing them in
isolation permits contradictory ownership of the first source map, an exact-empty persistent effect,
and a live same-process mapping. This ADR fixes their ordering and proof composition while retaining
ADR 0013/0097 Store semantics.

## Decision and ownership

- #201 owns a zero-source-mutation active-WAL/SHM read capability acquired before the first target
  `xOpen`/`xShmMap` and retained through eager decode.
- #202 owns the only new persistent effect: normalization of an independently proved exact-empty,
  sidecar-absent WAL-header remnant to rollback-journal/DELETE, followed by ordinary fresh init.
- #205 owns a one-reader, non-transitive, authenticated same-process writer-mapping lease and the
  narrow `SQLITE_OK/non-null -> SQLITE_READONLY/same-pointer` outward projection.

No receipt substitutes for another: #201 cannot authorize normalization; #202 cannot mint a mapping
lease; #205 cannot prove logical database validity or bypass CAS.

## Integrated state machine

The dispatcher first seals a target-independent runtime/VFS/filesystem capability and a retained
parent namespace handle, then performs an effect-free file-family census.

`unresolved -> capability-sealed -> namespace-held -> census-sealed` branches exactly once:

1. `active-wal-shm -> read-epoch-held -> first-map-qualified -> eager-decode -> read-receipt` (#201)
2. `exact-empty-wal-remnant -> normalization-lease-held -> pre-effect-sealed -> normalizing`
   `-> durable-post-effect-sealed -> fresh-init -> initialized-receipt` (#202)
3. `same-process-live-writer -> lease-acquired -> native-map-observed -> readonly-projected`
   `-> reader-unmapped -> lease-released` (#205)
4. other accepted ordinary states enter the existing no-new-effect Store path.

Mixed state, missing capability, identity drift, unknown result shape, or receipt incompleteness
transitions to a typed terminal before effect. No branch falls through to another after it has
observed or attempted an effect.

## Identity, epoch, and ABA rules

The composite authority binds pinned SQLite source ID and loaded image, underlying VFS callbacks and
app-data, mount/filesystem profile, retained root/parent, main/WAL/SHM/journal held-object and
directory-entry identities, WAL header/salts and authoritative prefix, namespace epoch, mapping
page/size/pointer, writer generation, process-instance generation, PID, and fork generation as each
becomes phase-authentic.

The epoch starts before target resolution or map delegation. Namespace watch overflow/loss, any
held-object or entry replacement, WAL reset/salt change, VFS unregister/re-register, runtime unload,
PID/fork change, mapping resize/unmap, or writer close revokes or quarantines affected capabilities
before pointer reuse. Monotone generation plus epoch identity rejects A-to-B-to-A and pointer ABA;
path equality, raw pointer equality, PID alone, and post-close endpoint equality are insufficient.

## Branch-specific effects

### #201 active read

The exact capability forbids create/delete/truncate/resize/write from the first target map. It may
return authentic `SQLITE_READONLY_CANTINIT/null` so the same SQLite connection constructs a private
heap WAL index under `WAL_READ_LOCK(0)`, or exact `SQLITE_READONLY/non-null` when qualified. Any
native `SQLITE_OK`, including null, is fail-closed unless the #205 lease branch was selected before
delegation. Decode is eager and complete while locks, objects, namespace epoch, and receipts remain
held. Cold-process success never depends on a #205 same-process lease.

### #202 exact-empty normalization

Eligibility requires logical and physical exact-empty, WAL-mode main header, all sidecars absent,
stable held identities, exclusive non-forgeable normalization lease, and no reader/writer ambiguity.
The allowlist contains only the minimal main-header/file-family changes required for WAL-to-DELETE
normalization and durability. Schema, Store metadata, payload, partition, chunk, counter, head, and
publication writes are forbidden. Pre-effect receipt, effect journal, file/parent durability barrier,
post-close census, and fresh-init receipt form one non-reusable chain. Any failure prevents fresh init.

### #205 same-process mapping lease

The writer registry mints a provisional lease before native delegation and promotes it only after
the exact mapping and identities are observed. A reader acquires an in-flight one-shot handoff before
its delegation. Only exact native `SQLITE_OK` with the same non-null pointer, page, size, generation,
epoch, runtime, VFS, filesystem, file-family identities, PID, and process instance can project to
exact `SQLITE_READONLY` with that pointer. Null, different pointer, absent/stale/revoked lease, unknown
extend, or cross-process/fork use fails closed and does not expose a mapping.

## Ordering and CAS

Revocation/quarantine begins no later than unmap, writer close, rollback/reset, resize, unload,
replacement, or fork detection; new acquisition is blocked, in-flight users drain, native unmap or
close completes, then registry retirement and cleanup occur. Publication validation precedes
expected-head CAS in one transaction. Only an expected-head mismatch is
`store.publication-conflict`; lease/capability/normalization/I/O failures never become CAS conflicts.

## Phase-authentic fields

| Phase | Available | Forbidden |
| --- | --- | --- |
| capability | runtime/VFS/filesystem qualification | target file identity |
| census | held parent and observed file-family identities | mapping pointer or WAL decode |
| read epoch | locks, WAL header/salt/prefix, namespace epoch | logical success before eager decode |
| normalization pre-effect | exact-empty proof, exclusive lease, allowlist | post-effect identity |
| normalization post-effect | effect journal, sync/close/census receipts | Store/publication success |
| mapping lease | writer/process generation and exact mapping identity | database validity or CAS success |
| terminal | original failure and bounded cleanup evidence | values from an unentered later phase |

## Crash/effect matrix

| Interruption | Allowed durable state | Recovery |
| --- | --- | --- |
| before branch selection | source unchanged | discard receipts |
| active read/map/decode | source unchanged | release locks/mapping; fail closed |
| before normalization effect | source unchanged | release exclusive lease |
| during normalization | old or journaled recoverable exact-empty physical state | replay/rollback journal, re-prove eligibility |
| after normalization sync, before fresh init | durable exact-empty DELETE state | consume same receipt chain or reclassify |
| during fresh init | ordinary atomic Store initialization states | existing recovery; no intermediate public success |
| lease revoke with readers | source unchanged by readers | quarantine, drain, unmap, retire generation |
| CAS loss/crash | atomic old or new head only | rollback/reopen and independently validate |

## Counterexamples and acceptance

Reject first-map mutation, `OK+null`, unleased or different-pointer projection, lease transfer,
stale/revoked/ABA lease, fork/PID reuse, runtime/VFS reload, alias/mount/object/directory replacement,
WAL reset, watch loss, non-empty normalization, sidecar-present normalization, implicit normal-open
effect, post-close-only proof, CAS weakening, cleanup-as-success, and cross-branch fallback.

Acceptance requires one exact-main independent review of this combined machine and its minimal
witnesses: cold active-WAL read, exact-empty normalization/recrash at every barrier, two live Stores
with CAS win/loss, and lease revoke/unload/fork/ABA. Source activation remains fail-closed until
contract/schema/source/checker agree. Platform and release qualification remain #173-owned.
