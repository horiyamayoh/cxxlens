---
id: DF-0209
title: Resolve SQLite reader cleanup after close quarantine
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

Two internally consistent outcomes remain possible. A confirmed late cleanup could install the normal
one-shot acknowledgement and permit only the exact outer open-epoch `xShmUnmap(0)` to consume it with
zero native work while retaining every close-quarantine pin. Alternatively, a dedicated late-close
cleanup terminal could record the exact native cleanup and omit the acknowledgement, making every later
unmap or close a zero-call `SQLITE_IOERR`. Existing authority does not distinguish these outcomes.

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

1. Preserve a terminal-close drain subledger, install the normal acknowledgement after exact successful
   cleanup, and allow only the exact outer open-epoch unmap to consume it with zero native work. This
   preserves the general cleanup-to-ack rule but needs explicit authority that the unmap is not an
   after-close replay and that it releases no close-quarantine lifetime pin.
2. Preserve a terminal-close drain subledger but define exact successful cleanup as a dedicated terminal
   quarantine row with no acknowledgement. This keeps all later calls uniformly rejected but creates an
   exception to the mandatory acknowledgement rule and must define how the outer SQLite unwind is
   represented without reconstructing an owner.
3. Treat the late mapped result as generic ambiguity and skip cleanup. This strands a determinate native
   mapping, violates the native-started peer disposition, and is rejected.
4. Reactivate the terminal group/open or reuse active-authority validation. This violates the accepted
   terminal graphs and fresh-admission quarantine boundary and is rejected.

## Recommendation

Add one narrow, precedence-ordered authority row for an original first-map callback whose native start
precedes an exact close cut but whose exact mapped terminal arrives after that close enters terminal
quarantine. The row must keep open/reservation/group terminal, retain candidate/predelegate/runtime/open
and family pins, prohibit pointer/predecessor/successor publication, authorize exactly one cleanup owner,
and select the exact acknowledgement fate. It must also define zero-effect, predecessor, opaque,
non-OK/throw/unknown, duplicate, abandonment, and terminal-commit-failure peers so unrelated quarantine
cannot enter the carve-out.

The exact contract amendment must receive independent semantic and structural review before the late
mapped implementation or tests are accepted.

## Disposition

2026-08-02: Observation recorded at exact head
`1c9d7eebf6d4b5abadbb9d2de53daf7630cb90a7` and tracked by Issue #209. The late mapped cleanup and
logical-ack implementation is blocked. This record is non-normative and authorizes no behavior.
