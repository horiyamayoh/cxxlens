---
id: DF-0202
title: Bind exact-empty WAL normalization before fresh SQLite initialization
status: accepted
kind: contract-contradiction
impact: invariant
confidence: high
implementation_disposition: may-proceed
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
resolution_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md
  - schemas/cxxlens_ng_sqlite_store_contract.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
review:
  mode: independent
  status: complete
  author: codex-agent-df0202-authority-resolution
  reviewer: codex-agent-sqlite-independent-counterexample-review
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/202#issuecomment-5094406150
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

2026-07-28 investigation expanded the contradiction from the no-sidecar endpoint to every
successful VFS callback boundary of the observed WAL-to-DELETE transition. Cold artifacts did not
form a single generic “journal residue” class: they partitioned into exact-pre/no-sidecar,
pre-form-or-post-form/zero-WAL, pre-form/non-hot journal prefix, pre-form-or-post-form/hot journal,
post-form/invalidated journal, and post-form/no-sidecar families. Here post-form means the
byte-exact current rollback-empty state, not a claim that an unavailable prestate or operation
history was reconstructed. Directly resuming the
normalizer from a non-hot journal prefix can open a zero-WAL before completing journal handling,
creating a WAL+journal mixed artifact outside that partition. Separate family-specific cleanup or
recovery is therefore required before a clean live-receipted normalizer or rollback-empty fresh
route.

The same investigation found that the cleanup proof must not claim more than the rooted VFS
authority in integrated-design §17.6. A retained authenticated parent and immediate current-leaf
regular/identity observation authorize one known-path unlink, but do not guarantee deletion of the
previously held exact object across a final-check-to-unlink rebind. Every delete also requires a
separate successful parent-directory sync before handoff; SQLite `xDelete(syncDir=1)` alone is not
a durability receipt.

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

Receiptless cold bytes can identify a finite physical family, but cannot reconstruct an operation
receipt or history. In this receiptless partition, pre/post mean the byte-exact current
complete-valid WAL-header 2/2 or rollback-header 1/1 empty main form; the post label alone does not
mean that an unknown preimage was reconstructed. Exact-post/no-sidecar, invalidated-journal/post, and
zero-WAL/post can become a new independently validated rollback-empty fresh anchor; none proves
that an earlier normalizer completed. Exact-pre/no-sidecar, non-hot-prefix/pre, hot-journal/pre-or-post
after exact recovery, and zero-WAL/pre can reach a new normalizer only after the current invocation
seals a fresh source receipt. A completed normalization edge exists only when one live receipt
chain binds pre-effect state, bounded effects, confirmed close, and exact poststate.

The pager journal nonce/checksum and large-sector record grammar are useful incomplete-write
guards, not provenance or success authority. The record set depends on decoded page size, effective
sector size, database page count, locking-page exclusion, SQLite build, VFS, and device
characteristics; the observed one-page/S=512 trace cannot be generalized by assumption.

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

The pending proposal must also distinguish authority acceptance from production activation.
Independent acceptance may authorize implementation and effects on explicitly disposable
qualification fixtures, while canonical/user-source effects remain blocked until repository-tracked
evidence covers the full family, parameter, crash, rebind, parent-sync, and recrash matrix.

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

### 2026-07-28 temporary receiptless probes

All paths and hashes in this subsection are temporary investigation evidence. They are neither
repository-tracked qualification artifacts nor authority, and they do not activate any source
effect.

- The default completed-callback process-termination matrix used decoded page size `P=4096`,
  effective journal sector `S=512`, one database page, pre-main SHA-256
  `e3ba06536f7dbba337dee3c1c5f01b43660ce276abb54c5cee2d5defc5b970aa`, and post-main
  SHA-256 `bd70f69256dee6875161b88a66f56baaf057e8f064a01108d11428b2d7a7b071`.
  Boundaries 1–6 retained exact-pre plus a non-hot journal prefix; 7–11 retained a valid
  one-record hot journal with exact-pre or exact-post main; 12–16 retained exact-post plus a
  journal whose first 28 bytes were zero; boundary 17 retained exact-post with no journal.
- The same trace observed `SQLITE_FCNTL_SYNC` 21 and `SQLITE_FCNTL_COMMIT_PHASETWO` 22
  delegate and return exact `SQLITE_NOTFOUND` 12. This is profile evidence, not a portable SQLite
  guarantee.
- A larger `S=65536`, `P=4096`, 66-page fixture produced `R=16` ordered records for pages
  1 through 16. Its pre-main SHA-256 was
  `678f61863e69ffe142f4ab0d2fb220d7732f5047a945a97f8b38942906299536` and post-main
  SHA-256 was `edf14063e8fe437b074840e39ee96d5c5b9b086abde7947298cc182145b2e75a`.
  Page 1 became the deterministic postimage and pages 2–16 remained byte-exact preimages.
  The temporary source `/tmp/cxxlens_sqlite_durability_probe.cpp` had SHA-256
  `37cbef3a86edbad5c8315fd01f518ff98765da2a95873e6143ae75ac99cad8de`; one locally built
  `/tmp/cxxlens_sqlite_durability_probe_large` had SHA-256
  `25d005861a6eae541baa1a58cdf83a6a98653181a15618ceaeb037ba7cb380f5`.
  The large probe inherited one-record ordinal labels, so it is not a complete large-sector
  interruption matrix and its rebuild-specific binary hash is not a durable identity.
- The temporary hot-recovery barrier source
  `/tmp/cxxlens_sqlite_recovery_trace_probe.cpp` had SHA-256
  `ec462a0dae7ca9f55405eb51fe94375acf1ee07428cc60d61f06df9b4b8b6b97`, and one binary had
  SHA-256 `24e00488761bafb6e5fc63abb8152904474c2591f835929397399883d240ebc0`.
  Exact-pre, exact-post, and `S=65536/P=4096/R=16` hot inputs replayed exact preimages,
  deleted the journal, then stopped the next WAL open before underlying delegation with
  `SQLITE_IOERR` 10 and confirmed close to exact-pre/no-sidecar. Trace SHA-256 values were
  `2d9d3d343b370fbc6a0c2745a9ca2727c7dae68f1399313339d57074494eb5fb`
  (small pre),
  `a3eb4b4d498eccbcd4642a3536e5d7d41d2c4a3d568a7648a40375bcd7ea39f0`
  (small post), and
  `987f2bc83f6fe606423d25f50048fa9e28c0a3474b3c7d27f3542f2600c3c5d0`
  (large post).
- The prototype did not insert or fault the required authenticated parent-directory fsync and did
  not cover the final-check-to-unlink leaf-rebind race. It denied WAL through a diagnostic
  prototype latch rather than a production exact-state controller. These are explicit
  qualification blockers.
- These probes terminate only after successful callback returns. They do not cover power loss,
  torn/partial sectors, kernel or device cache behavior, termination inside callbacks, or
  unqualified filesystem reorder.

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
   would preserve the existing contract, but it must reproduce in both Cxxlens static/shared
   runners while binding the SQLite image each runner actually loads, and prove the exact source
   file-family effects. The current trace does not establish such a route.
3. Fail every WAL-header exact-empty remnant closed. This avoids a new write boundary, but it loses
   the existing recoverable fresh/crash qualification. It is rejected unless higher authority
   explicitly accepts that semantic reduction; an implementation may not select it merely to
   avoid the normalization design.

Only investigation and authority work may proceed for this scope. No alternative currently
authorizes SQLite v3 activation or the persistent WAL-to-rollback transition.

## Recommendation

Materialize Alternative 1 as a pending authority-first amendment, without treating this record,
temporary paths, hashes, probe output, SQLite result prose, or pager checksum as authority. The
proposal should do all of the following:

- Define the disjoint `F0/FZ/FP/FH/FI/FO` receiptless family partition, with raw no-effect
  classification before generic journal rejection and explicit `FZ-pre`/`FZ-post` precedence.
- Resolve that raw census only relative to a retained authenticated parent capability with
  no-follow typed enumeration/open/stat and no host-path re-resolution.
- Route `F0/FP/FH/FZ-pre` through their required cleanup/recovery into a newly sealed live
  normalizer receipt. Route `FO/FI/FZ-post` only to an independently validated rollback-empty
  fresh anchor. Never infer a cold operation history, completed edge, or success.
- Bind the normalizer's original accepted-empty recovery receipt, same main identity and entry,
  continuous namespace epoch, exclusive lock, exact pre-main bytes, VFS/runtime/device/build
  profile, sidecar census, deterministic post-main bytes, exact allowed effects, confirmed close,
  and post-close census in one immutable receipt chain.
- Limit cleanup to authenticated-known-path unlink relative to a retained parent capability after
  immediate current-leaf regular/identity observation. Do not promise exact-object deletion across
  the final-check-to-unlink race; a rebind there is post-effect durability/authority opaque with
  no retry, handoff, or second snapshot.
- Require a separate successful retained-parent fsync after every deletion and before every
  handoff, including hot recovery and the normalizer's coordination WAL deletion and terminal
  rollback-journal deletion. Also require
  journal namespace sync after the first journal sync and before valid header/main writes.
- Treat the pager checksum and derived large-sector record grammar only as supporting
  incomplete-write evidence, and bind all `S/P/page-count/record-set`, locking-page,
  `xDeviceCharacteristics`, SQLite build, VFS, and filesystem assumptions. Prove mechanically
  that the admitted bounds make the pending-byte locking page exceed every derived record-set
  bound, and use a synthetic injected-locking-page rejection instead of requiring an impossible
  crossing execution vector.
- Preserve successful normalization as an internal intermediate edge, never Store/public success,
  and carry its live receipt into exactly one ordinary fresh initialization.

Before accepting that proposal, independent review must falsify replacement, nonempty authority,
mixed/extra residue, identity/byte drift, watch loss, same-process lock release through another fd,
rebind-at-unlink, parent-sync failure, close failure, skipped/reordered gate sequence, unqualified
device/build profile, and every permitted callback-boundary interruption. Review must also confirm
that static/shared Cxxlens runners bind the SQLite DSO they actually load rather than inferring two
SQLite runtime forms from runner labels.

Acceptance of the exact proposal may authorize classifier/port/gate/barrier/fault-harness and
fixture-scoped cleanup/recovery/normalizer implementation, with source effects on explicitly
disposable qualification fixtures only. Canonical/user
source and production activation must remain blocked until repository-tracked harness and
toolchain evidence, the full parameterized callback/recrash/idempotence matrix, canonical report
digest, and an independent counterexample review are complete and the draft profile is explicitly
replaced by an accepted profile. A fixture must be reachable only through an internal
qualification entrypoint and a harness-minted nonforgeable capability created only after the
isolated disposable root exists. It must bind retained-root identity/lifetime, exact fixture
locator, run ID, selected runtime/VFS/device/build profile, and allowed family/effect/fault
schedule; it must not be derivable from a path, environment, public flag, report field, or
self-asserted boolean, and a public/user locator must never be accepted as fixture authority.

Alternative 2 falsifies the need for this proposal only if it supplies the same implementation,
trace, fault, cold-reopen, and exact file-effect evidence without relying on an unauthorized
persistent transition. Any normative patch must cover the integrated design, ADR 0097,
SQLite/Snapshot Store contracts and schema mirrors, exact checker expectations, negative tests,
and design checksums, then receive a fresh independent review on Issue #202 before implementation
of qualification-only classifier/port/gate/fault-harness and fixture-scoped cleanup/recovery/
normalizer work resumes. Production activation
remains separately blocked by the full qualification gate.

## Disposition

2026-07-23: Investigation opened from Issue #181 and is tracked by Issue #202 after a direct
pinned-runtime callback trace showed that accepted-empty WAL recovery can leave a WAL-mode main
with no sidecars while the ordinary fresh path cannot seal its journal-transition receipt. The
local WAL-to-DELETE prototype is retained only as diagnostic evidence of a possible bridge. No
authority, public C++ API, logical Store semantic, production implementation, or qualification is
changed by this record. SQLite v3 activation and the affected exact-empty recovery/fresh path
remain blocked pending an authority-first proposal and independent review; reviewer identity and
review references are intentionally unset.

2026-07-28: Independent zero-base review accepted exact proposal commit
`b6cbb86347e02c4b374d7991a1f78d2535789ced` for the disposable qualification implementation
layer only. The accepted amendment adds the finite receiptless family model, live-versus-cold
receipt boundary, known-path cleanup semantics, parent-sync requirements, process-callback-only
crash scope, and two-layer authorization described above. The implementation disposition is
`may-proceed` only for classifier/ports/gates/barrier/harness and fixture-scoped cleanup/recovery/
normalizer behind the harness-minted nonforgeable retained-root capability and internal
qualification-only entrypoint.

Canonical/user-source effects, production activation, public Store success, and accepted-profile
replacement remain blocked. The temporary probes establish feasibility and counterexamples only;
they are not repository-tracked qualification and omit at least authenticated parent-fsync fault
injection and rebind-at-unlink coverage. Production may proceed only after the repository-tracked
harness/build/toolchain and full parameterized callback/rebind/parent-sync/recrash matrix produce a
canonical report digest and a distinct later independent review accepts the exact implementation
and production profile.
