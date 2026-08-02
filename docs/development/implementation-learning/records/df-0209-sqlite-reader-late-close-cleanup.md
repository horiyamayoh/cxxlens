---
id: DF-0209
title: Resolve SQLite reader cleanup after close quarantine
status: proposed
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
tracking_issue: '#209'
implementation_issues:
  - '#181'
resolution_refs: []
review:
  mode: independent
  status: pending
  author: codex-agent-sqlite-reader-late-close-observation
  reviewer: null
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/209
created: '2026-08-02'
---

# Resolve SQLite reader cleanup after close quarantine

## Observation

At PR #193 exact head `1c9d7eebf6d4b5abadbb9d2de53daf7630cb90a7`, an exact first-reader-map
callback may return only after the same open epoch has entered terminal close quarantine. This occurs
after a same-thread or reentrant close contender, or after an other-thread close wait ends in timeout
or an unknown result.

The accepted contract requires an earlier native-started mapped attempt to finish only through its
existing exact cleanup lineage. It also requires every confirmed proactive unpublished-first cleanup
to install one `awaiting_sqlite_ack` owner. The same authority rejects an after-close acknowledgement
and gives `terminal_quarantined` open, reservation, and group states no ordinary successor. It does not
explicitly select whether a cleanup that completes after terminal close quarantine installs a later
zero-native acknowledgement or terminates directly into retained quarantine.

## Working mental model

Terminal close quarantine must not be reactivated and must not authorize a native close retry, fresh
reader admission, pointer publication, successor publication, or release of process-lifetime ambiguity
pins. The original pre-cut callback nevertheless remains a distinct already-owned drain and may need
an internal single-use subledger that is orthogonal to the terminal open/reservation/group states.

Two internally consistent outcomes remain possible under the currently accepted authority. This
proposal selects a confirmed late cleanup installing the normal one-shot acknowledgement and permitting
only the exact outer open-epoch `xShmUnmap(0)` to consume it with zero native work while retaining every
close-quarantine pin. The alternative would define a dedicated late-close cleanup terminal that records
the exact native cleanup without an acknowledgement and makes every later unmap or close a zero-call
`SQLITE_IOERR`. The proposal records the selection for review; it does not resolve the ambiguity or
authorize implementation until the normative mirrors are independently reviewed and explicitly accepted.

## Mismatch or opportunity

Snapshot-store callback/cut precedence and SQLite peer disposition require the native-started callback
to retain one exact cleanup route. The existing implementation instead falls through to generic map and
session quarantine because its close-cut predicate accepts only a live `close_admitted` wait. The normal
cleanup admission path then requires an active reservation/group and active authorities that are no
longer available after family quarantine.

Changing only the implementation would silently choose new lifecycle and acknowledgement semantics.
Because the choice affects exactly-once cleanup, terminal-state totality, lifetime-pin retention, and
replay behavior, the late mapped/cleanup/ack portion of Issue #181 is blocked until normative authority
selects one total row. Exact zero-effect, predecessor, and opaque terminal representation may be designed
in parallel but must not be committed as a purported complete matrix while the shared late-drain boundary
is unresolved.

## Evidence

- `src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp:7302-7328` stores the exact mapped
  receipt but selects cleanup only while the original close cut is still live and exact.
- `src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp:9357-9689` requires a reserved/active
  group, active authority validation, and the speculative cleanup slots for normal cleanup admission.
- `src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp:9700-9957` installs
  `awaiting_sqlite_ack` after every exact successful proactive cleanup.
- `src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp:9960-10049` rejects logical-ack
  consumption after the open is no longer open or an exact live close continuation.
- `src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp:18372-18454` terminalizes the group
  and consumes or cancels its close-wait and speculative cleanup lineage during quarantine.
- `tests/unit/sdk/sqlite_same_process_shm_mapping_registry_test.cpp:2151-2326` currently encodes all
  late outcomes as generic group/map/session quarantine and therefore proves the contract-shrinking
  behavior rather than the required exact cleanup/opaque disposition.
- `schemas/cxxlens_ng_snapshot_store_contract.yaml:1017-1019,1040,1047,1060,1068-1071` and the
  corresponding SQLite contract rows jointly expose the unresolved ordering.
- Canonical tracking issue: <https://github.com/horiyamayoh/cxxlens/issues/209>.

## Alternatives and trade-offs

1. **Proposed selection:** preserve exactly one original-callback terminal-close drain subledger, install
   the normal acknowledgement after exact successful unpublished-first cleanup, and allow only the exact
   outer open-epoch `xShmUnmap(0)` to consume it with zero native work. This preserves the general
   cleanup-to-ack rule, distinguishes that already-owned outer unwind from confirmed after-close replay,
   and releases no close-quarantine lifetime pin.
2. Preserve a terminal-close drain subledger but define exact successful cleanup as a dedicated terminal
   quarantine row with no acknowledgement. This keeps all later calls uniformly rejected but creates an
   exception to the mandatory acknowledgement rule and must define how the outer SQLite unwind is
   represented without reconstructing an owner.
3. Treat the late mapped result as generic ambiguity and skip cleanup. This strands a determinate native
   mapping, violates the native-started peer disposition, and is rejected.
4. Reactivate the terminal group/open or reuse active-authority validation. This violates the accepted
   terminal graphs and fresh-admission quarantine boundary and is rejected.

## Recommendation

Independently review and, only after acceptance, add the proposed narrow precedence-ordered authority row
for an original first-map callback whose native start precedes an exact close cut but whose exact mapped
terminal arrives after that close enters terminal quarantine. The proposed row keeps open/reservation/group
terminal, retains candidate/predelegate/runtime/open and family pins, prohibits
pointer/predecessor/successor publication, preserves exactly one original callback cleanup owner, installs
one acknowledgement after exact successful unpublished-first cleanup, and permits only the exact outer
open-epoch `xShmUnmap(0)` to consume it once with zero native effect. Confirmed after-close replay remains
a zero-native `SQLITE_IOERR` and cannot enter the carve-out. Zero-effect, predecessor, opaque,
non-OK/throw/unknown, duplicate, abandonment, and terminal-commit-failure peers remain fail closed.

The exact outer unwind must present a pre-cut issuer-sealed noncopyable one-shot owner retained in the
original SQLite call/session context; the registry and close cut retain only its nonauthorizing validation
seal/control until exact entry moves that owner under the registry mutex. Eligibility also requires a closed
close-terminal provenance receipt selecting only same-thread/reentrant quarantine or bounded-other-thread
timeout/unknown, with zero native `xClose` and zero prior ack. A wrong presenter preserves the valid owner
and ack, while exact-owner abandonment, unknown, destruction, or terminal-commit failure transitions the
ack to terminal quarantine with every pin retained and no native retry or reconstruction.

The exact contract amendment must receive independent semantic and structural review before the late
mapped implementation or tests are accepted.

## Disposition

2026-08-02: Observation recorded at exact head
`1c9d7eebf6d4b5abadbb9d2de53daf7630cb90a7` and tracked by Issue #209. The late mapped cleanup and
logical-ack implementation is blocked. This record is non-normative and authorizes no behavior.

2026-08-02: Status advanced to `proposed` with the narrow option-1 selection mirrored as
`cxxlens.sqlite.reader-late-close-cleanup.v1`. The proposal preserves exactly one original callback drain
through pre-cleanup terminal close quarantine, installs one ack only after exact successful unpublished-first
cleanup, permits only the exact outer same-open-epoch `xShmUnmap(0)` to consume it with zero native effect,
and rejects confirmed after-close replay. Here, confirmed after-close replay means an unmap after a close
terminal either consumed an ack that was already present before close or completed native `xClose`; it
explicitly excludes the pre-cleanup terminal close-quarantine row. Candidate, predelegate, runtime/VFS,
open, family, generation, native-file, and ambiguity pins remain retained; no close, group, predecessor,
successor, fresh-admission,
unregister, unload, or wakeup authority revives. `implementation_disposition` remains `blocked`,
`resolution_refs` remains empty, and neither this record nor the proposal subtree authorizes runtime or test
implementation before exact-commit independent review and explicit normative acceptance.

2026-08-02: Semantic review of exact `3a35af9` rejected the proposal with P1=3 because the outer-unwind
issuer/custody, close-terminal provenance, and ack indeterminate-terminal rows were under-specified:
<https://github.com/horiyamayoh/cxxlens/issues/209#issuecomment-5154806081>. The revised proposal closes
those three axes with a caller-context move-only presentation owner plus registry-only validation seal,
closed provenance enum/receipt/tuple/sequence, wrong-owner preservation, and exact-owner indeterminate
terminalization. This rejected review is evidence, not an acceptance receipt.

2026-08-02: Structural review of exact `3a35af9` was GO with P0=0, P1=0, P2=1 and requested an independent
Snapshot schema digest pin:
<https://github.com/horiyamayoh/cxxlens/issues/209#issuecomment-5154807970>. The pin and fail-closed test were
added. Review metadata remains `pending` because the materially revised exact proposal commit has not yet
received fresh independent semantic and structural review. Status remains `proposed`, disposition remains
`blocked`, and the revised subtree still authorizes no runtime or test implementation.
