# ADR 0105: Direct-main review evidence and release authority

- Status: Proposed for independent review
- Date: 2026-08-21
- Owner: #173
- Contract IDs: `development.direct-main-review-governance.v1`, `development.review-receipt.v1`, `development.release-authority-composition.v1`
- Product semantics: unchanged

## Context

`AGENTS.md` makes atomic direct-to-main delivery the current repository workflow authority, while
older readiness, CI, issue, and ADR prose sometimes treats a branch or pull request as correctness
evidence. This ADR defines one evidence model without changing product/security/qualification
authority. Until acceptance, `AGENTS.md` remains the controlling workflow authority.

## Decision

The normal unit is:

`scope-resolved -> base-main-recorded -> locally-verified -> atomic-main-commit`
`-> origin-fast-forwarded -> exact-main-ci-observed -> issue-evidence-recorded`.

Branch name and PR existence are metadata, never correctness authority. Conflict class, exclusive
path ownership, dependency closure, and exact revision remain mandatory. Force push, reset/rebase of
published history, secret/permission changes, billing, and production deployment remain outside this
authority.

Evidence classes are distinct:

- `leaf`: focused contract/source tests for one bounded implementation unit;
- `integration`: exact-main dependency closure and shared-surface tests;
- `platform`: an exact toolchain/OS/configuration tuple;
- `release`: #173 aggregate decision composed from #167 GR execution and #179 terminal scope closure.

No leaf or Linux-only result is promoted to platform or release evidence. A closed implementation
issue may state `production qualification: not claimed`.

## High-risk acceptance

Normative security, identity, protocol, persistence, irreversible-effect, public-semantics, and
resource-bound changes use two non-rewriting commits:

1. commit a Proposed authority and push it to `main`;
2. independently review that exact full SHA with counterexamples;
3. record a canonical #owner-issue comment naming the exact SHA, reviewer identity/session, verdict,
   findings, verification limits, and qualification boundary;
4. commit a review receipt containing the comment URL, canonical comment-body SHA-256, reviewed
   commit/tree, candidate GitHub login, distinct reviewer GitHub login/identity/session, and explicit
   accept/reject;
5. only an `accept` receipt with no unresolved P0/P1 may accompany a follow-up Accepted commit and
   corresponding machine-authority activation.

The v2 register keeps `decision_status`, `authority_status`, `review.outcome`,
`implementation_status`, `qualification_status`, and `activation` independent. Rejection is durable
and is never rewritten to pending. Receipt v1 binds candidate commit/tree, the sorted authority
path/blob projection and canonical digest, owner issue, comment URL/body digest, distinct
author/reviewer/session, verdict and P0/P1/P2 census, connected run, and the status-only acceptance
path allowlist. The acceptance SHA is not self-embedded; the checker deterministically selects the
first descendant commit whose receipt registry contains the receipt ID, then verifies its diff.
The receipt registry is append-only: an acceptance commit may add exactly the selected receipt and
must preserve every earlier receipt byte-for-byte. Offline checking verifies Git identity, authority blobs, ancestry, allowlisted paths,
identity separation, findings, activation and preserved WIP heads. Connected CI verifies GitHub
comment bytes/author and the exact-candidate successful run. It fetches the candidate commit from
GitHub, binds the claimed candidate login to the authenticated commit author, and requires the
reviewer login to differ from both authenticated author and committer. A run name is not authority: the
connected checker resolves the immutable workflow ID and requires the active workflow path
`.github/workflows/autonomy-fast.yml`. Offline-only evidence cannot activate production support.

The authenticated comment body is the canonical JSON projection of receipt ID, decision/owner,
candidate commit/tree/Git author/candidate GitHub login, complete authority digest, author, isolated read-only Codex
reviewer provenance and UUID session, verdict, P0/P1/P2 census, and qualification boundary. The
connected checker requires byte equality with that projection; a REJECT body cannot be represented
as an accepted receipt. The comment must be authored by the reviewer GitHub login, and that login
must differ from the candidate GitHub login. The authority file set must exactly equal the decision register closure and
the acceptance path set is checker-derived, never claimant-selected. The inferred acceptance must
be the immediate direct-main child of the candidate on the ancestry path.

## Concurrency and WIP

Before writing and before commit, record `main` SHA and check active conflict-class/path ownership.
Unverified WIP branches, worktrees, and PRs retain exact-head provenance and are never deleted merely
because a different decision won. Reuse requires a fresh diff against accepted authority and focused
evidence; green historical CI alone is insufficient.

## CI freshness and release composition

`Autonomy fast` runs for every main SHA without cancellation and preserves exact-SHA authority,
schema, work-unit, receipt, documentation, checksum, and bounded fast-gate evidence. A successful
Fast run triggers `Autonomy heavy`; its concurrency group coalesces work, but the freshness job first
compares the candidate with current `origin/main`. A stale candidate emits only a `superseded`
report. Only the current candidate can run the full integration gate.

Heavy first authenticates the triggering workflow's immutable GitHub workflow ID against
`.github/workflows/autonomy-fast.yml`; a duplicate display name is not authority. Heavy re-fetches and reclassifies `origin/main` after the full gate and uploads a full artifact only
if the candidate is still current; a main update during execution cannot promote stale evidence.
Before producing a non-synthetic agent packet, the generator fetches `origin/main` and requires a
clean local HEAD equal to that refreshed ref. An agent packet is executable only if its own state and every transitive dependency are `ready`.
Otherwise it emits an actionable dependency-qualified stop disposition. A corrected candidate never
rewrites a prior rejecting review to pending; it remains rejected until an authenticated Accepted
receipt replaces that outcome.

Nightly evidence is release-eligible only when entered by schedule or explicit dispatch and bound to
latest main at start; legacy reusable invocations and the legacy Quality workflow remain compatibility
evidence, not inputs to the autonomy release authority. Release evaluation is non-cancelled,
dispatch-only, and requires `candidate_sha == current origin/main`. Its current implementation emits
only `not-qualified` and never GR. Future qualified evaluation must authenticate exact successful
Heavy and Nightly, #167 GR execution, and #179 terminal scope closure before #173 aggregates them.

## Failure and recovery matrix

| Failure | Repository effect | Required action |
| --- | --- | --- |
| base moved before commit | none | rebase is not implied; re-evaluate scope and fast-forward safely |
| focused verification failed | none | keep work uncommitted or make a later explicit fix; no success claim |
| push rejected/non-fast-forward | none remote | fetch and reconcile without rewrite |
| exact-main CI failed | committed history retained | owner issue plus follow-up atomic fix |
| independent review rejected | Proposed remains | record blockers; never mark Accepted |
| connected review verification unavailable | Accepted activation withheld | retain offline evidence and retry in CI |
| release gate failed | bounded units remain historically complete unless disproved | #173 owns the aggregate gap |

## Counterexamples and acceptance

Reject PR-number-as-evidence, branch-name-as-identity, review of an abbreviated or different SHA,
self-review presented as independent, accepted status with unresolved P0/P1, stale CI from another
tree, dirty-tree release evidence, Linux-to-native promotion, and reopening a bounded issue solely
because aggregate release remains blocked.

Acceptance requires negative tests for stale base, wrong tree/SHA/comment digest, reviewer equality,
missing connected verification, unknown decision state, evidence-class promotion, and release owner
drift. The role allocation composes #173 aggregate tracking with #167 GR execution and #179 terminal
production-scope closure. #173 does not fabricate either closed contract owner's result.

## Prior independent review and current candidate

The review of exact commit `ac68aa78a3aaa91e6e33e73b40e55a8da827b16e` rejected this ADR's evidence
mechanics with four P1 findings. Direct-to-main delivery remains active under `AGENTS.md`; the
rejection applies to the proposed review receipt and release-role model.

The v2 candidate addresses the four findings as follows:

- receipt v1 binds exact candidate/tree/authority/comment/reviewer/session/finding census and requires
  a successful connected run before Accepted activation;
- release roles are the explicit #173/#167/#179 composition;
- preserved WIP refs are resolved against immutable recorded heads; and
- work-unit and CI contracts own stale-base, scope, freshness, and evidence-class checks.

This revision remains Proposed until its exact commit receives a fresh independent P0/P1-zero review
and connected receipt. The direct-to-main amendment in `AGENTS.md` remains controlling meanwhile.
