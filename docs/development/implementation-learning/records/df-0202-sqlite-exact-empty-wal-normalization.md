---
id: DF-0202
title: Bind exact-empty WAL normalization before fresh SQLite initialization
status: investigating
kind: contract-contradiction
impact: invariant
confidence: high
implementation_disposition: blocked
scope:
  - store.sqlite-accepted-empty-normalization
  - store.sqlite-fresh-journal-transition-receipt
  - store.sqlite-wal-header-no-sidecar-recovery
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md
  - schemas/cxxlens_ng_sqlite_store_contract.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
tracking_issue: '#202'
implementation_issues:
  - '#181'
resolution_refs: []
review:
  mode: independent
  status: pending
  author: codex-agent-df0202-authority-resolution
  reviewer: null
  refs: []
created: '2026-07-23'
---

# Bind exact-empty WAL normalization before fresh SQLite initialization

## Observation

A filesystem SQLite source can be independently proved exact logical empty while its main header
is already in WAL mode and its WAL, SHM, and rollback-journal sidecars are absent. The accepted
fresh path requires the owned VFS exclusive-lock callback to seal the fresh journal-transition
receipt before persistent journal effects are enabled.

On the pinned SQLite runtime, a direct callback trace of this state showed that the only
`BEGIN IMMEDIATE` succeeded, the effect gate remained at WAL/SHM coordination, and
`armed_after_lock` remained false. The post-operation census contained the same main object plus a
zero-byte WAL and no SHM. The required exclusive-lock callback therefore did not occur and the
fresh journal-transition receipt could not be sealed at its authorized arming point.

The currently authorized recovery/checkpoint route can prove exact empty and normalized sidecar
absence, but it leaves the main header in WAL mode. A following ordinary fresh transition then
cannot reach its WAL journal-transition callback. A local diagnostic prototype that receipt-binds
WAL-to-DELETE normalization, confirms close and a sidecar-absent anchor, and only then enters
ordinary fresh initialization made an injected journal-transition remnant cold-reopen
successfully. That persistent header/journal transition is not authorized by current ADR 0097 or
the SQLite/Snapshot Store contracts.

## Working mental model

Logical exact emptiness and a normalized sidecar census do not establish the physical precondition
needed by the existing fresh journal-transition protocol. When the durable main header already
selects WAL, asking SQLite to establish WAL again can be a no-op with respect to the exclusive-lock
callback on which the pre-journal receipt and effect gate depend.

The accepted-empty recovery route and ordinary fresh initialization therefore leave an uncovered
intermediate state: exact logical empty, same main identity, no authoritative sidecars, but a WAL
header that prevents the existing fresh arming protocol from observing its required transition.
An explicit WAL-to-rollback normalization might bridge that state, but it is a persistent effect
and needs its own pre-effect proof, effect boundary, close discipline, terminal classification,
and receipt chaining. Success of the local prototype is evidence for feasibility only; it does not
authorize that transition.

## Mismatch or opportunity

The current authority requires accepted-empty recovery to run recovery/checkpoint and prove
normalized exact empty before journal setup. It separately requires fresh initialization to seal
its receipt after the underlying exclusive lock succeeds and before the first header, journal,
WAL, SHM, or file-control effect. After an exact-empty WAL recovery, however, the main header can
remain in WAL mode even though sidecars are absent, so the second requirement cannot be reached by
the authorized fresh sequence on the observed runtime.

The same contract forbids source recovery/checkpoint, sidecar-cleanup retry, or a second snapshot
after a fresh post-arm failure. It does not authorize a WAL-to-rollback header transition between
accepted-empty recovery and ordinary fresh initialization. Silently adding that write would break
the persistent-effect and receipt invariants; failing the state permanently would weaken the
existing recoverable fresh/crash qualification. SQLite v3 activation under Issue #181 is therefore
blocked on an authority-first resolution.

## Evidence

- The pinned-runtime callback trace recorded `BEGIN IMMEDIATE` success, coordination-only effect
  gating, `armed_after_lock == false`, and a same-main-plus-zero-WAL/no-SHM terminal census.
- The current recovery/checkpoint experiment re-established exact logical empty and absent
  sidecars while leaving the main header's read/write version bytes in WAL mode.
- Re-entering the ordinary fresh sequence from that state did not invoke the callback that seals
  `transaction.fresh_v3_initialization.guards.filesystem.journal_transition_atomicity.pre_journal_receipt`.
- A local WAL-to-DELETE diagnostic prototype reached a confirmed-close, sidecar-absent anchor and
  then allowed ordinary fresh initialization; the injected journal-transition remnant subsequently
  cold-reopened. The prototype exercised an effect not present in current authority and is not
  acceptance evidence.
- ADR 0097 and the integrated design require the fresh receipt to be sealed immediately before
  journal arming, and classify post-arm residue only after finalize, conditional rollback, exactly
  one close, and receipt-aware post-close observation.
- `schemas/cxxlens_ng_sqlite_store_contract.yaml` authorizes accepted-empty original recovery as
  recovery/checkpoint followed by proof of exact empty and normalized sidecars before journal
  setup, while its fresh post-arm rule forbids recovery/checkpoint, cleanup retry, or a second
  snapshot.
- `schemas/cxxlens_ng_snapshot_store_contract.yaml` mirrors the distinct fresh-initialization
  receipt and its before-journal-arming seal but defines no receipt for the observed intermediate
  WAL-header/no-sidecar state.
- The canonical observation and required review boundary are tracked by
  <https://github.com/horiyamayoh/cxxlens/issues/202>.

## Alternatives and trade-offs

1. Authorize an exact-empty-only normalization transaction. Before any normalization effect, bind
   the stable source receipt, same main object and directory entry, exclusive lease,
   recovery/checkpoint result, exact-empty projection, and sidecar census. Permit only the
   WAL-to-rollback main-header and WAL/SHM/journal normalization effects; forbid schema, metadata,
   payload, publication, head, counter, and process-state authority writes. Require confirmed
   close, a normalized post-close census, and a composite receipt carried into the ordinary fresh
   receipt. This preserves recoverability but adds a new persistent-effect phase that needs
   authority, schema mirrors, checker negatives, runtime fault injection, and independent review.
2. Produce an implementation-backed recovery/checkpoint route that reaches a rollback-mode main
   header and sidecar-absent exact empty without any new persistent transition authority. This
   would preserve the existing contract, but it must reproduce on the pinned static and shared
   runtimes and prove the exact source file-family effects. The current trace does not establish
   such a route.
3. Fail every WAL-header exact-empty remnant closed. This avoids a new write boundary, but it loses
   the existing recoverable fresh/crash qualification. It is rejected unless higher authority
   explicitly accepts that semantic reduction; an implementation may not select it merely to
   avoid the normalization design.

Only investigation and authority work may proceed for this scope. No alternative currently
authorizes SQLite v3 activation or the persistent WAL-to-rollback transition.

## Recommendation

Materialize Alternative 1 as an authority-first proposal, without treating this record or the
working prototype as authority. The proposal should define one exact-empty-only normalization
phase and an immutable composite receipt that binds the original accepted-empty source receipt,
same main identity and directory entry, exclusive lease, recovery/checkpoint result, pre-effect
exact-empty projection and sidecar census, exact permitted physical effects, confirmed close, and
post-close rollback-header/sidecar-absent census. Successful normalization must remain an
intermediate state, never public success, and must feed the original receipt chain into the one
ordinary fresh `BEGIN IMMEDIATE` initialization.

Before accepting that proposal, independently falsify all of the following:

- Exact empty, same identity, and sidecar census are proved before any normalization effect.
- Replacement, nonempty authority, mixed layout, journal presence, busy checkpoint, or receipt
  drift causes no normalization write.
- Only main-header and WAL/SHM/journal normalization effects are possible; schema, metadata,
  publication, chunk, head, counter, and process-state writes remain absent.
- Cleanup is statement finalization, at most one rollback when applicable, exactly one close, and
  then total post-close classification.
- Close non-OK, observation failure, residue, invalid/mixed state, and replacement map to the
  operation-specific typed result, with no retry or second snapshot.
- Successful normalization is not exposed as public success; the original receipt chain remains
  bound to the subsequent fresh receipt and single `BEGIN IMMEDIATE` initialization.
- Faults before and after checkpoint, normalization, close, and the fresh journal transition all
  preserve the required cold-reopen outcome and exact source file-family effect bounds.
- The full matrix passes for both static and shared pinned SQLite runtimes, including the direct
  callback trace that originally falsified the current sequence.

Alternative 2 falsifies the need for this proposal only if it supplies the same implementation,
trace, fault, cold-reopen, and exact file-effect evidence without relying on an unauthorized
persistent transition. Any normative patch must cover the integrated design, ADR 0097,
SQLite/Snapshot Store contracts and schema mirrors, exact checker expectations, negative tests,
and design checksums, then receive a fresh independent review on Issue #202 before implementation
resumes.

## Disposition

2026-07-23: Investigation opened from Issue #181 and is tracked by Issue #202 after a direct
pinned-runtime callback trace showed that accepted-empty WAL recovery can leave a WAL-mode main
with no sidecars while the ordinary fresh path cannot seal its journal-transition receipt. The
local WAL-to-DELETE prototype is retained only as diagnostic evidence of a possible bridge. No
authority, public C++ API, logical Store semantic, production implementation, or qualification is
changed by this record. SQLite v3 activation and the affected exact-empty recovery/fresh path
remain blocked pending an authority-first proposal and independent review; reviewer identity and
review references are intentionally unset.
