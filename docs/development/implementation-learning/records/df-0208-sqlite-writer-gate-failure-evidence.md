---
id: DF-0208
title: Bind SQLite writer gate failure to exact evidence
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
  - schemas/cxxlens_ng_sqlite_store_contract.schema.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.schema.yaml
tracking_issue: '#208'
implementation_issues:
  - '#181'
resolution_refs: []
review:
  mode: independent
  status: complete
  author: codex-agent-sqlite-writer-gate-failure-observation
  reviewer: codex-agent-df0208-record-reviewer
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/208#issuecomment-5103551866
created: '2026-07-28'
---

# Bind SQLite writer gate failure to exact evidence

## Observation

At exact head `96bbcd0b67954ae5240170b2b7b829b708848b9b`, the production-inert
same-process SHM registry can atomically promote the complete exact pending member set of one
registry-bound writer attachment from one positive
`sqlite_shm_verified_writer_eligibility_receipt`. Its documented rejection path deliberately
leaves every pending caller-owned and does not complete the failure boundary.

The accepted DF-0206 authority separately requires a determinate map-before-gate failure to hide
the complete attachment group before one non-removing unmap and close. The current internal
surface has no typed negative gate-failure receipt, sealed gate-attempt owner, or other exact
evidence that distinguishes a completed determinate gate failure from missing eligibility,
revocation, stale state, or an arbitrary caller assertion. A bare rejection, boolean, or selected
pending token would therefore authorize irreversible group hiding without proof that the exact
current-v3 gate failed.

## Working mental model

Positive writer eligibility and determinate gate failure are disjoint evidence outcomes of one
exact gate attempt. Absence or revocation of positive eligibility is not negative proof. A
failure outcome should be single-use and bind at least the exact family, connection, open epoch,
gate attempt, and failure classification that authorize the transition.

After validating that evidence at the registry linearization boundary, the state machine should
derive the complete attachment member set centrally, hide it atomically, and transfer one
move-only cleanup owner. Caller-provided member snapshots may be useful inputs for success
validation, but they cannot define the authoritative failure set while a same-attachment later
map may race with gate completion.

It remains uncertain whether the best representation is a closed success/failure result produced
by one gate validator or a separate verified failure receipt paired with a sealed gate-attempt
owner. That choice requires authority and independent counterexample review before mutation.

## Mismatch or opportunity

Integrated design, ADR 0013, ADR 0097, and the SQLite/Snapshot contract mirrors define exact
effects after determinate gate failure, timeout, unknown outcome, and open-epoch drift. They do
not define the typed negative evidence that is allowed to initiate the determinate failure
transition or transfer its sole cleanup authority.

The existing positive eligibility receipt cannot represent failure. The existing
`begin_writer_cleanup` overloads can derive and seal an attachment group from a post-native or
pending anchor, but possession of such an anchor does not prove that the exact Store gate
completed with a determinate failure, nor does it total-order that failure with later-map
admission at the registry boundary.

Implementing determinate failure mutation from an untyped signal would silently add authority.
Omitting the transition would leave the accepted failure contract unimplemented. The affected
failure mutation and production activation are therefore blocked. Positive-only DF-0206 group
promotion and later-map success ordering may continue independently because they consume the
already defined positive eligibility evidence and do not infer failure.

## Evidence

- `src/sdk/sqlite_same_process_shm_mapping_lease_internal.hpp:185,232-260` defines only the
  positive `sqlite_shm_verified_writer_eligibility_receipt`; its named gate validator is
  forward-declared, and a repository-wide source search at the observed head finds no validator
  definition or typed negative result.
- `src/sdk/sqlite_same_process_shm_mapping_registry_internal.hpp:562-571` explicitly states that
  group promotion rejection neither seals nor completes the failure boundary and that cleanup
  remains a later checkpoint.
- `src/sdk/sqlite_same_process_shm_mapping_registry_internal.cpp:762-822` serializes positive
  complete-group promotion under the registry mutex but exposes no corresponding determinate
  failure boundary.
- `src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp:916-1163,2636-2649` stores an
  optional positive promotion-gate receipt on an attachment and implements exact success
  publication. It has no attachment record for a verified negative gate outcome.
- `tests/unit/sdk/sqlite_same_process_shm_mapping_registry_test.cpp:1784-2243` constructs positive
  eligibility through the test peer and covers exact group success, non-consuming rejection, and
  liveness-loss cleanup. It has no typed determinate gate-failure input or failure/later-map
  total-order case.
- `schemas/cxxlens_ng_sqlite_store_contract.yaml:575-600` requires complete-group failure cleanup,
  gate/later-map total order, and terminal timeout, unknown, and open-epoch-drift behavior, while
  preserving the production activation block until exact implementation and matrix review.
- Canonical tracking issue:
  <https://github.com/horiyamayoh/cxxlens/issues/208>.

## Alternatives and trade-offs

1. Extend one gate-attempt validator to return a closed, single-use success-or-determinate-failure
   receipt. This makes the two outcomes mutually exclusive and can bind replay and ordering in
   one place, but it requires an authority amendment and independent review of the negative
   evidence fields.
2. Add a separate verified failure receipt plus a move-only gate-attempt owner. This can preserve
   the existing positive eligibility surface, but the authority must prevent a success and
   failure receipt from being produced for the same attempt and must define which owner transfers
   complete-group cleanup.
3. Treat eligibility revocation or absence as gate failure. This conflates close, drift, stale
   lookup, and a known validation result, so it cannot prove the determinate failure effect and is
   rejected.
4. Accept a boolean, prose error, rejection enum, or caller-selected pending anchor. These values
   are forgeable or incomplete and can hide the wrong group during a later-map race, so they are
   rejected.

Internal positive eligibility ordering remains separable and reversible. It may proceed under
DF-0206 as long as it does not introduce a determinate failure mutation, native callback
activation, or production claim.

## Recommendation

Define authority for the negative gate outcome before implementing it. The proposal should bind
the exact gate-attempt identity and connection/open epoch, distinguish determinate failure from
timeout, unknown, drift, and stale input, make success and failure mutually exclusive and
non-replayable, and specify the registry linearization point that centrally seals the complete
attachment group and transfers one cleanup owner.

Counterexample review should cover forged or cross-connection failure receipts, success/failure
double completion, stale open epoch, caller snapshots racing with a later map, partial cleanup,
duplicate unmap, and timeout or unknown outcome revival. Actual VFS callback binding for the
determinate gate-failure route, its native unmap/close execution, and production activation must
remain blocked until the accepted evidence contract, exact implementation, and complete matrix
receive their required independent reviews.

## Disposition

2026-07-28: Observation recorded at exact head
`96bbcd0b67954ae5240170b2b7b829b708848b9b` and tracked by Issue #208. Independent review is
complete with `P0=0 / P1=0 / P2=0`; the canonical receipt is
<https://github.com/horiyamayoh/cxxlens/issues/208#issuecomment-5103551866>. This record changes no
normative authority and activates no production behavior.

Determinate writer-gate failure mutation and any production binding are blocked until exact
negative evidence authority is accepted. Positive-only DF-0206 attachment-group promotion,
gate/later-map success ordering, and focused internal tests may continue under the existing
accepted authority without treating this record as authority.
