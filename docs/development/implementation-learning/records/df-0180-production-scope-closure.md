---
id: DF-0180
title: Close the distribution 1.0 production surface exactly once
status: accepted
kind: contract-contradiction
impact: invariant
confidence: high
implementation_disposition: may-proceed
scope:
  - production.scope-closure
  - release.qualification-rollout
  - readiness.cross-authority-binding
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0088-foundation-declared-github-issue-scope.md
  - docs/design/adr/0091-distribution-1-production-qualification.md
  - docs/design/adr/0092-exact-public-callable-inventory.md
  - schemas/cxxlens_ng_release_bundle.yaml
  - schemas/cxxlens_ng_release_qualification.yaml
  - schemas/cxxlens_ng_api_development_readiness.yaml
  - schemas/cxxlens_ng_acceptance_manifest.yaml
tracking_issue: '#180'
implementation_issues:
  - '#179'
resolution_refs:
  - docs/design/adr/0095-production-scope-closure.md
  - docs/design/adr/0094-risk-tiered-goal-authorization.md
  - docs/development/agent-api-development-goal.md
review:
  mode: independent
  status: complete
  author: codex-agent-root
  reviewer: codex-agent-authorization-adversarial-reviewer
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/180#issuecomment-5015143471
    - https://github.com/horiyamayoh/cxxlens/issues/180#issuecomment-5015155832
created: '2026-07-19'
---

# Close the distribution 1.0 production surface exactly once

## Observation

The existing production authorities validate their own domains, but no release-rooted contract proves that every distribution-1.0 surface is present exactly once and classified by release disposition and evidence state. The current #173 headline counts therefore cannot distinguish complete scope closure from an omitted provider, package, qualification, security, compatibility, or explicit-exclusion surface.

Known audits also show that a green GR checker can currently emit production tuples without the actual-source installed Clang worker-to-snapshot path, while NG1 provider hardening, production incremental coordination, and exact-main Nightly evidence remain incomplete.

## Working mental model

Production scope is a typed graph rooted in the Release Bundle. Source authorities own node semantics and IDs; a closure contract owns only typed references, release disposition, cross-links, and evidence/remediation mapping. Exact-once applies within typed leaf namespaces. Aggregate gates and inherited callable rows are derived rather than separately hand-owned.

Inventory and qualification are different. A normal report may prove a complete classification while exposing tracked gaps. Final qualification requires all production-required nodes to have independent exact-SHA evidence and no unresolved blocking feedback.

## Mismatch or opportunity

Independent green domain checkers do not prove a cross-authority partition. A flat copied list would become a second semantic authority, while prose counts in #173 are neither durable nor fail-closed. Immediate strict activation also creates a rollout cycle: honest gaps would make #179 main red, but permissive legacy GR would continue issuing false production claims.

The release workflow therefore needs an acyclic, truthful intermediate evaluation state that produces no production tuples while gaps remain, without weakening the existing strict GR report.

## Evidence

- Release Bundle defines NG0/NG1, NG2/NG3 exclusions, R0-R4 blockers, R5-R7 non-blockers, public target/package matrices, and compatibility axes.
- Acceptance Manifest contains ten gate entries, not only G0-G5/GR.
- Public API Catalog admits 23 unique headers; 21 of them declare the 536 stable callable rows in the exact callable inventory. The two umbrella-only headers declare no callable of their own.
- Relation Registry contains 18 descriptors; only 11 generated relation headers are catalog-admitted, with DF-0174 blocking the remaining static-admission classification.
- Provider Protocol defines 13 profile features, 23 messages, and two execution surfaces; current NG1 implementation/evidence is incomplete.
- ADR 0091 requires an installed actual-source Clang-to-store path that current GR evidence does not traverse.
- Latest Nightly run 29656128809 failed both shallow-history callable binding and ASan/UBSan provider-runtime completion.

## Alternatives and trade-offs

1. Keep prose/counts in #173. This is cheap but cannot fail on drift, omission, or duplicate ownership.
2. Add more independent per-domain checkers. This improves local validation but still cannot prove the release-rooted partition.
3. Copy all semantic rows into one flat manifest. This appears complete but creates a second authority and collapses typed layers.
4. Add a typed closure graph, grouped assignments, derived inheritance, exact report, and two-stage release evaluation. This adds governance code but preserves source ownership and prevents false production tuples during rollout.

## Recommendation

Adopt option 4 through ADR 0095. Keep known production fixes in separate units. Make normal mode accept only explicit, fully mapped gaps; make final mode require all required/evidence nodes qualified, zero blocking feedback, exact qualified evaluation, and the existing strict GR report.

## Disposition

2026-07-19: Proposed from implementation issue #179 and production audit #173. The contract/invariant change remains blocked pending exact-text independent adversarial review and accepted authority-first resolution.

2026-07-19: First exact-text review requested five amendments: distinguish the 23 admitted versus 21 callable-declaring headers; make unresolved authority part of the exhaustive state matrix; freeze exact v1 namespace IDs; separate intermediate release evaluation from ADR 0094/final GR semantics; and derive blocking-DF applicability independently of assignments. ADR 0095 and this evidence record were amended accordingly; acceptance remains blocked pending re-review.

2026-07-19: Independent exact-text re-review confirmed that all five amendments are resolved and found no remaining blocker. ADR 0095 is accepted as the authority-first resolution, so the implementation disposition is `may-proceed`. Implementation issue #179 may now implement the manifest, validators, reports, and CI binding without absorbing the queued production fixes.

2026-07-19: Three independent implementation reviews then challenged release/evaluation byte
binding, typed evidence and aggregate ownership, and the workflow/report tail. The tail review
demonstrated concrete marker-comment, condition/mode reversal, missing argv, dependency-pair,
schema omission, artifact order/provenance, runner/default/environment, and missing-push-trigger
bypasses. The implementation now parses the workflow structure, fixes the exact tail job and step
shape/order/action mapping/argv, rejects unrecognized mutation steps, and requires exactly 30
domain rows, 14 source digests, and 13 evidence censuses in the standalone report schema. Every
demonstrated counterexample is a permanent negative test.

2026-07-19: Final independent re-review approved all three scopes with no remaining changes
requested. The consolidated evidence is recorded in
[the DF-0180 implementation review](https://github.com/horiyamayoh/cxxlens/issues/180#issuecomment-5015468036).
The final reviewer ran
82 combined tail tests and nine focused adversarial regressions; the scope, release, readiness,
checksum-independent contract checks, and diff hygiene passed.
