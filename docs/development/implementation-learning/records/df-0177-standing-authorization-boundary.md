---
id: DF-0177
title: Bind delegated goal execution to a risk-tiered approval boundary
status: accepted
kind: missing-assumption
impact: contract
confidence: high
implementation_disposition: may-proceed
scope:
  - agent-goal.standing-authorization
  - agent-goal.protected-main-workflow
  - api-development-readiness.authorization-binding
authority_refs:
  - AGENTS.md
  - docs/development/agent-api-development-goal.md
  - docs/design/adr/0093-implementation-learning-design-feedback.md
  - schemas/cxxlens_ng_api_development_readiness.yaml
tracking_issue: '#177'
implementation_issues:
  - '#176'
resolution_refs:
  - docs/design/adr/0094-risk-tiered-goal-authorization.md
  - AGENTS.md
  - docs/development/agent-api-development-goal.md
  - tools/quality/check_ng_api_development_readiness.py
  - tools/quality/verify_checksums.py
  - tests/quality/test_ng_api_development_readiness.py
review:
  mode: independent
  status: complete
  author: codex-agent-root
  reviewer: codex-agent-authorization-adversarial-reviewer
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/177#issuecomment-5014900762
    - https://github.com/horiyamayoh/cxxlens/issues/177#issuecomment-5014944654
created: '2026-07-19'
---

# Bind delegated goal execution to a risk-tiered approval boundary

## Observation

The durable API-development goal delegates repository implementation, CI repair, issue updates,
commit, push, and CI monitoring, but it does not bind that delegation to an explicit approval
policy. During PR #175, a generic skill requirement for explicit approval was therefore treated
as a new conversational gate even though the active `/goal` had already delegated the same-issue
CI repair and GitHub workflow.

The same goal document instructs the agent to push each completed issue to `main`. That language
does not encode the repository's current protected-main workflow: unit branch, pull request,
exact-head required checks, review resolution, merge, and exact merged-main qualification.

## Working mental model

An explicit `/goal` can carry standing authorization for a bounded class of reversible,
repository-scoped actions. The authorization should attach to the named execution contract and
policy ID, not to every conversation with the repository. A generic skill-level approval gate can
then be satisfied by that prior authorization when the action is enumerated by the policy, while
the skill's diagnosis and focused-plan reporting still run.

Standing authorization cannot grant capabilities that the platform or system withholds, and it
should not cover destructive, externally consequential, or authority-undecidable decisions. Those
operations remain target/effect-specific fresh user-approval gates after disclosure. Mutable
GitHub operations must be limited to the canonical repository's active issue/PR; that normal
workflow is distinct from contacting a customer or third party outside the active unit. The user
must also be able to revoke or narrow the delegation at any time.

## Mismatch or opportunity

The current contract has only a semantic-decision question rule. It does not distinguish a
non-blocking notification from a fresh approval gate, does not state when delegated authorization
activates, and does not reconcile repository delegation with generic skill language. This creates
unnecessary stops for ordinary CI repair while leaving the direct-main sentence broader than the
actual branch-protection workflow.

A repository-scoped policy can make the boundary both more autonomous and more restrictive:
routine unit work proceeds without repeated approval, but fresh-approval and platform carve-outs
become explicit and fail-closed.

## Evidence

- `docs/development/agent-api-development-goal.md` delegates issue-scoped implementation,
  commit, push, issue state, and CI monitoring to the coordinating agent.
- The same document currently says to push each completed issue to `main` and only later monitor
  the final main SHA.
- Implementation issue #174 / PR #175 required a local design-feedback fixture repair within the
  same contract; all exact-head checks and the merged-main run subsequently passed without changing
  accepted public semantics.
- The merged-main qualification for PR #175 is
  `https://github.com/horiyamayoh/cxxlens/actions/runs/29677931873` at
  `c6596b25aed5e2ee3b5afb3241b57f062cd49067`.
- Issue #176 records the approved implementation scope and negative scenarios.

## Alternatives and trade-offs

1. Require a new conversational approval whenever a skill uses the word "approval". This is
   simple, but it discards explicit goal delegation and repeatedly blocks reversible work.
2. Treat all goal actions as pre-approved. This maximizes autonomy but is too broad for destructive
   history changes, production effects, permissions, billing, third-party communication, and
   platform permission prompts.
3. Adopt a risk-tiered standing authorization bound to an explicit goal contract. This preserves
   autonomy for enumerated unit work and uses fresh approval for high-impact boundaries, at the
   cost of a policy contract and regression checker.
4. Keep the direct-main sentence and rely on branch protection to reject it. This delegates safety
   to an external failure and cannot prove that the durable workflow itself is correct.

## Recommendation

Adopt option 3 as `CXXLENS_AGENT_AUTHORIZATION_V1`. Bind it exactly once in `AGENTS.md` and the
goal document, encode activation and ordinary-conversation non-activation, revocation,
active-unit-scoped notification, target/effect-specific fresh approval, external blocker, skill
compatibility, platform, and protected-main rules, and make API-development readiness reject drift.

## Disposition

2026-07-19: Proposed from issue #176. The execution-contract change remains blocked pending an
independent adversarial review and authority-first resolution through ADR 0094.

2026-07-19: Independent review identified two authorization ambiguities in the first proposal:
mutable issue/PR updates were not explicitly limited to the active unit, and repository review
responses overlapped an unqualified third-party-contact gate. The proposal now limits mutable
GitHub workflow to the canonical repository's active issue/PR, distinguishes external contact,
requires disclosed target/effect-specific fresh approval, and adds external-blocker and ordinary-
conversation non-activation verification.

2026-07-19: Independent adversarial review completed with no remaining blockers. ADR 0094 was
accepted as the authority-first resolution. Implementation issue #176 may proceed to align the
repository execution contracts and readiness checker with that decision.

2026-07-19: The repository contracts now bind the policy exactly once, replace the direct-main
sentence with the protected-main unit workflow, and distinguish notification from fresh approval.
The readiness checker accepts the complete policy and rejects missing binding, standing scope,
ordinary-request non-activation, platform carve-out, fresh approval, external blocker,
protected-main binding, short-goal-example binding, and legacy direct-main workflow. Targeted
readiness tests passed 28/28 and design-feedback tests passed 21/21.

2026-07-19: The design checksum inventory now includes ADR 0094, preventing the accepted decision
from falling outside the package-integrity check.

2026-07-19: Final independent implementation review found no blockers and declared the unit ready
to commit. The final contract-label run passed 18/18 in addition to the targeted suites and
freshness checks.

2026-07-19: PR review found that policy-ID substring counting could accept a suffixed token and
that two prose sentences were checked outside the marker contract. The checker now uses full-token
matching, moves direct-main prohibition and fresh-approval non-reuse into stable markers, and has
dedicated negative tests for all three regressions.
