---
cxxlens_document_status: archived
cxxlens_authority: non-normative
cxxlens_replacement: docs/design/cxxlens_next_generation_integrated_design_ja.md
cxxlens_removal_issue: "#72"
---
# Public API Contract Freeze (#54)

> [!CAUTION]
> Issue #57 によりこの freeze と Phase C authorization は失効した。以下は旧124 API の
> migration provenance であり、新規 API 実装を許可しない。現行 authority は
> `cxxlens_next_generation_integrated_design_ja.md` と
> `../../schemas/cxxlens_ng_authority_transition.yaml` である。

## Authority and atomic transition

`schemas/cxxlens_public_api_contract_freeze.yaml` is the deterministic Phase B freeze authority. It authorizes Phase C only when all 22 packages, 124 APIs, and 124 atomic units are covered exactly once and every package contract changes from `candidate` to `frozen` under issue `#54` in one catalog revision. Candidate manifests remain immutable provenance, so the fingerprints validated by issues `#52` and `#53` do not change during the transition.

Contract state, implementation state, and execution readiness remain independent. The freeze retains 47 `conformant` and 77 `unimplemented` implementation states; it does not claim that planned implementations already exist. The 77 implementation units become contract-ready, while the generated readiness gate may still fail closed on provider, dependency, fixture, or CI evidence that belongs to Phase C execution.

## Commit binding

A Git commit cannot contain its own commit ID. The checked-in authority therefore uses `git-head-at-execution`. `cxxlens-public-api-contract-freeze` validates every input digest against that authority and emits the same schema with `exact-git-commit`, `commit_sha`, `tree_sha`, `evidence_commit_sha`, and the commit-bound Phase B integration report. The command rejects a dirty worktree, a stale integration report, or differing evidence and source commits.

Any change to a catalog, header, schema or registry, documentation/example, task packet, ownership ledger, readiness artifact, Phase A evidence, high-risk result, or integration acceptance input invalidates the input fingerprint and revokes Phase C authorization until this gate is regenerated and independently validated.

## Phase C authorization

The authorized mode is `serial-single-writer`. One atomic unit is implemented and validated at a time; dependency requests and shared-component stewardship remain authoritative. This freeze does not permit silent fallback, AST-pointer transfer, unordered serialization, macro-expansion edits, mutation without plan/validator/dry-run/transaction, or loss of unresolved/evidence/coverage/guarantee information.
