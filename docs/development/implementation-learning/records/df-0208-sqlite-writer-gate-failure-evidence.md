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

2026-07-28: The four SQLite/Snapshot contract/schema mirrors now carry the exact review-pending
proposal `cxxlens.sqlite.writer-gate-outcome-evidence.v1` as
`writer_gate_outcome_evidence_amendment_proposal` with status
`proposed-unqualified-non-authorizing`. It binds one move-only gate-attempt owner, the closed
success/determinate-failure/terminal-indeterminate outcome union, the registry cut and later-map
order, native-state-specific drain/cleanup, composite unmap-then-close ownership, and the DF-0207
reader carve-out. The proposal changes no accepted DF-0206 field, adds no acceptance receipt, and
does not authorize implementation or production binding.

Fresh independent semantic/structural review of the exact proposal commit is still pending. The
completed review recorded above applies only to the accuracy and materiality of this observation;
it is not acceptance of the proposal and must not be cited as proposal authority.

2026-07-28: Independent semantic review of the prior review-pending proposal draft returned
`P0=0 / P1=7 / P2=0` and `NO-GO`. It found that the draft did not close: the six-stage exact
value/effect/canonical evidence grammar; immutable issuer-sealed outcome versus registry-owned cut
execution; one shared checked non-reusable admission/cut sequence domain and exhaustion; mandatory
exactly-one drain for known-mapped indeterminate state; attachment-wide reduction of mixed no-map
and mapped members; empty-indeterminate exact-close-owner totality; and preservation of the
accepted DF-0206 live cleanup-only lifetime during a same-attempt lifecycle contradiction.

A separate DF-0206 predecessor-semantics audit added four blocking checks: registry execution must
not reseal the issuer outcome; the final complete group and one-to-one coverage must be rederived
after blocker resolution; gate-before-map must bind an expected attachment reservation separately
from observed present state; and pre-acceptance owner drop or post-cut continuation abandonment
must not discharge existing DF-0206 member cleanup obligations. The revised four mirrors now make
those eleven axes explicit. They also close the reservation lifecycle
`reserved -> consumed_to_present | revoked`, failure/terminal first-nonpass evidence locus, cut
transition graph and cleanup-lineage boundary, close-only versus mapped unmap-then-close owner
union, and the live-positive carve-out from generic cut quarantine.

This revision remains `proposed-unqualified-non-authorizing`, carries no acceptance receipt, and
does not resolve this record's `blocked` implementation disposition. A fresh independent review
must evaluate the exact committed mirrors and positive/negative mutation matrix before any gate
outcome mutation, native cleanup route, or production binding is authorized.

2026-07-28: Frozen structural review of that revision returned `GO`, while independent semantic
review returned `NO-GO`. The semantic review found that the proposal still did not define a
closed stage-result algebra with an exact stage/failure bijection and outcome-consistency
equations; an exclusive `claimed_inflight` reservation state and move-only callback claim owner;
native-state-only member classification separated from attachment cleanup and connection-close
authority censuses; coverage accounting separated from effect readiness; or a total, disjoint
native dispatch matrix selected from the current cut-execution state. It also found the late
resolution lineage under-specified: its exact expected-reservation binding, tagged observed
attachment state, valid pre-native cancellation boundary, permanent-unresolved zero-call outcome,
and exactly-one late mapped cleanup owner were not closed. Direct positive assertions for the
empty-positive first exact map and preservation of pre-acceptance member ownership were also
missing.

The second revision makes those axes explicit. It closes every stage result to
`passed | typed_determinate_failure | terminal_indeterminate`, binds each ordered stage
bijectively to one typed failure, and rejects unknown results, wrong-stage failures, and
result/prefix/outcome/locus disagreement. The expected reservation now has the five-state
`reserved | claimed_inflight | consumed_to_present | revoked | quarantined` graph. Exactly one
callback claim owner is moved under the registry mutex before native delegation; same-thread,
other-thread, timeout, revocation, exact no-map, exact map, mismatch, and uncertain-result races
are individually closed without allowing a contender to steal or reconstruct the original owner.

The revision also limits terminal member classification to native state, derives one
attachment-wide cleanup-authority census and one orthogonal connection-close census, and forbids
member receipts from minting either owner. Complete coverage no longer implies effect readiness:
any unresolved member forces `cut_execution_indeterminate` and installs one unfired late lineage.
That lineage binds the exact expected reservation, retains
`absent | present_exact | unresolved`, accepts cancellation only from the same callback
owner/control epoch before native callback start with proof that a future effect is impossible,
waits for every bound callback to become terminal or exactly cancelled, and otherwise keeps
permanent zero-call quarantine. A late exact mapping joins the existing sole owner rather than
minting another.

Finally, the revision defines a precedence-ordered, total, disjoint dispatch matrix:
live-positive carve-out, unresolved fence, current cut-indeterminate operational rows, then sealed
outcome rows. It distinguishes positive eligibility/promotion, determinate close-only or
unmap-then-close, terminal close/drain/zero-call, and operational indeterminate close/drain/zero-
call cells across normalized native census and exact/absent/ambiguous authority. Every applicable
no-map row atomically revokes a still-reserved expected attachment, while positive mapped
completion requires and retains both exact attachment and close owners. The quality contract now
includes direct positives for the empty-positive first exact claim/map and pre-acceptance owner
retention, plus mutations for the new closed axes.

This second revision is still `proposed-unqualified-non-authorizing`, has no acceptance receipt,
and does not authorize implementation, native cleanup, VFS binding, public projection, or
production activation. Fresh independent structural and semantic review must evaluate the exact
committed mirrors and mutation matrix; neither the prior observation review nor either rejected
proposal review can be cited as acceptance authority.

2026-07-28: A read-only closure audit of the in-progress second revision identified two remaining
ambiguities before qualification. First, a completed cut or an already consumed operational
lineage could appear to re-enter the 21 effect rows despite exactly-once dispatch. The mirrors now
partition `unfired` pending dispatch from `consumed_tombstone` or completed re-entry; the latter
selects no effect row, performs zero native or authority effect, and rejects replay. Second, an
exact same-owner/control-epoch cancellation before native callback start normalized to no-map
without explicitly consuming the post-cut claim reservation. It now atomically transitions the
exact `claimed_inflight` reservation owner to `revoked` under the registry mutex. These
clarifications remain part of the same non-authorizing second revision and require fresh exact-
commit review with the rest of the proposal.

The same audit then tightened four dependent boundaries. Pending dispatch now begins only from
`effect_ready` after final rederivation, exact coverage, zero unresolved, and the required
authority cell, or from a zero-unresolved cut-indeterminate terminal with an unfired operational
lineage; `cut_open` and `resolving_cut_universe` cannot select a row. Dispatch consumption is the
one-way `unfired -> consumed_tombstone` graph, and every row, including a zero-call quarantine
row, consumes it atomically with the decision. Late resolution owns no independent native or
close permit: after the last exact resolution it submits once to the matching existing
cut-indeterminate operational row and shares that consumption. Finally, tagged `absent` requires
an exact expected-reservation/callback-cohort terminal no-map receipt or valid same-owner
pre-start cancellation receipt; it cannot be inferred from an unknown identity. These additions
also remain non-authorizing pending fresh exact-commit review.

Further counterexample review closed dependent P1s before qualification. The dispatch domain is
now the three-way `waiting_no_effect | decision_unfired |
terminal_or_consumed_reentry` partition. A complete, exactly covered, zero-unresolved,
closed-census resolving cut enters `effect_ready` with no effect or consumption; only
`effect_ready + unfired` selects one of fifteen sealed rows, while an equivalently complete
cut-indeterminate terminal selects one of six operational rows. Valid sealed rows complete;
invalid sealed rows perform their named safe operational action and enter cut-indeterminate with
the same token, without selecting a second row. Coverage-integrity failure is outside all 21 rows
and consumes one zero-effect quarantine tombstone. Live-positive handling first removes the
accepted live group and all of its owners from both target and census, rederives the residual, and
then waits, applies the coverage fence, or consumes one residual row without fallthrough.

Terminal stage evidence is also phase-total. Passed and determinate-failure stages retain full
observed-value, typed-receipt, and observed-effect bundles. An at-stage terminal uses canonical
tags for `not_observed | exact_present`, `not_issued | exact_present`, and
`not_executed | started_outcome_unresolved | exact_present`: before-value is wholly negative,
after-value has exact value and receipt but no executed effect, after-effect-start has exact
value/receipt and the exact start-permit-consumption/partial-transcript payload with unresolved
outcome, and after-effect has all exact payloads. Negative tags carry no payload; present and
started tags require their exact payload; every tag and payload is length-framed into the evidence.

Late resolution now has disjoint installation kinds. A precut unresolved cut shares the unfired
cut-indeterminate dispatch and can submit only after complete rederivation, exact coverage, zero
unresolved, and closed censuses. A postcut uncertain claim exists after a completed positive cut
and therefore owns a distinct claim-derived decision lineage; the original cut census, close
owner, dispatch token, and completed terminal are immutable non-authorizing context. Exact no-map
or valid cancellation consumes that lineage with zero effect. Exact mapped resolution may
transform the same owner into one new sealed accepted DF-0206
`post_native_failure_without_live_member` cleanup instance only with complete DF-0205
`pending_install` and `post_map_seal` receipts, a complete attachment-member census, and zero
existing live cleanup instances. Any existing instance or missing/ambiguous authority forbids a
join and yields zero-call quarantine. The first exact bound postcut receipt after the original
completed terminal is valid; only cross-bound, duplicate, or own-tombstone-late receipts are
rejected. DF-0208 row authority and the postcut claim-derived DF-0206 transfer form a closed,
disjoint source union and never authorize postcut close or cut reuse.

2026-07-29: The exact review-pending proposal was revised to close the remaining callback-origin,
cut-admission, mapping-integrity, custody, and durable-release products. Callback claim acquisition
now atomically forms one `atomically_dual_bound` native/member prestart pair; a claim-only or partial
peer formation is unreachable as an ordinary origin and selects only the zero-effect invalid-partial
guard or defensive sink. Normal reservation-bearing work selects exactly one of
`claim_and_form_dual | start_existing_dual`; a cut-frozen fresh or already formed dual origin selects
exactly one of the four origin-specific start/terminal continuation steps and cannot claim, form, or
cross origins while frozen.

Mapped terminal publication now binds the mapping-identity subtype and its exact actual or opaque
observed lifetime pins in the same commit. Fresh unsafe custody has only
`live_origin_tombstone | retired_identity_tombstone`; its sole registry-mutex retirement transition
changes only state, pins, and the exact retirement receipt. Fence-first retirement preserves any
installed typed reason, offending-member tag, quarantine reference, and terminal-route binding
byte-identically. Retirement-first allows the later fence to bind that complete metadata exactly
once while treating preexisting components byte-identically and never reconstructing a pin. Dual
nonpromotion uses one composite pin-custody cell with a closed cleanup/fence race, ownerless versus
independent DF-0206 retirement, and an exact zero-live-pin terminal proof.

Every cut terminal also installs or replaces one checked-generation durable latest record before
slot release or waiter wakeup. Later dispatch consumption may update only that exact generation, and
release is forbidden until every fresh or dual custody and postcut continuation has transferred
completely. The counterexample matrix directly covers atomic dual formation, invalid partial
formation, normal and frozen substeps, fresh/dual mapping fences, both fresh retirement orders,
composite pin lifecycle races, and durable record/release ordering.

The frozen canonical SQLite YAML has file SHA-256
`8ef6fd952df321cada2f083d9368d28108d67fb95c7cc2101d243319a7fafc46`,
DF-0208 semantic digest
`sha256:30fd6d29224e9707bbdf0f72b585d75c726f361e97ce6ed730dd608f985fa728`,
and enclosing lease digest
`sha256:79f31929806955fceeb373739b5f67b8395525bb77d57f8886f6f0c559bcd89f`.
Two independent frozen read-only semantic/product audits each returned
`P0=0 / P1=0 / P2=0` and `GO`. Those results qualify this exact non-authorizing proposal revision for
mirror, document, and quality-contract synchronization only. They do not accept the proposal, add an
acceptance or resolution receipt, authorize implementation or production activation, or change this
record's blocked implementation disposition.
