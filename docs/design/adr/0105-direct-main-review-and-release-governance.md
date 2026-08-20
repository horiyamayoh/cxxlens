# ADR 0105: Direct-main review evidence and release authority

- Status: Proposed for independent review
- Date: 2026-08-21
- Owner: #173
- Contract ID: `development.direct-main-review-governance.v1`
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
- `release`: full/stress/platform/scope-closure evaluation owned only by #173.

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
   commit/tree, author identity, distinct reviewer identity/session, and explicit accept/reject;
5. only an `accept` receipt with no unresolved P0/P1 may accompany a follow-up Accepted commit and
   corresponding machine-authority activation.

The offline checker verifies receipt shape, body digest, reviewed commit object/tree and ancestry,
semantic equality between Proposed and Accepted content, reviewer/author distinction, and exact
activation. A connected CI check verifies the GitHub URL, comment author, and body bytes. If network
verification is unavailable, acceptance remains locally review-complete but remotely unverified and
cannot activate production support.

## Concurrency and WIP

Before writing and before commit, record `main` SHA and check active conflict-class/path ownership.
Unverified WIP branches, worktrees, and PRs retain exact-head provenance and are never deleted merely
because a different decision won. Reuse requires a fresh diff against accepted authority and focused
evidence; green historical CI alone is insufficient.

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
drift. #173 remains the sole distribution 1.0 release authority.
