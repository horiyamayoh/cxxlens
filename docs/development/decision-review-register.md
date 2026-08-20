# Development decision and review register

`schemas/cxxlens_ng_development_decision_register.yaml` v2 is the versioned inventory of
repository-level decisions that must remain distinct from implementation and production
qualification. It records the selected alternative, rejected shortcuts, review state,
implementation state, qualification state, and preserved WIP provenance.

The register does not replace subsystem authority. A high-risk decision remains inactive until an
accepted ADR and machine-readable contracts resolve it and an independent reviewer binds the exact
reviewed `main` commit to a canonical GitHub issue comment. The repository owner does not provide a
separate interactive approval gate.

The six dimensions are deliberately independent: `decision_status`, `authority_status`,
`review.outcome`, `implementation_status`, `qualification_status`, and `activation`. A rejected
review remains `rejected`; a corrected candidate starts a new review and does not erase the prior
verdict. `decided` therefore never implies Accepted, implemented, qualified, or active.

Accepted authority references a receipt in
`schemas/cxxlens_ng_development_review_receipts.yaml`. The receipt binds the candidate commit and
tree, authority blobs and normalized digest, owner issue and canonical comment body, distinct
author/reviewer/session, verdict and P0/P1/P2 census, and an exact-candidate connected CI run. The
acceptance commit may change only declared status/receipt paths and must descend from the candidate.
To avoid a self-referential commit hash, it is derived as the first descendant commit whose receipt
registry contains that receipt ID.

Delivery uses atomic fast-forward commits on `main`. A pull request can remain useful review or
external-contribution evidence, but its existence, review state, or branch status is not correctness
authority. Proposed high-risk authority is committed first, independently reviewed at that exact
commit, and accepted only by a later non-rewriting `main` commit.

The offline checker rejects duplicate or orphan receipts, foreign owner issues, wrong commit/tree/blob
or normalized authority digests, self-review, accepted verdicts with P0/P1, missing exact-candidate
connected verification, activation before acceptance, qualification before implementation, and
replacement of a preserved WIP ref. Connected CI additionally authenticates GitHub comment bytes,
author identity, and the named CI run; offline-only evidence cannot activate production support.
The comment is canonical receipt JSON, so candidate, authority closure, isolated reviewer session,
verdict, finding census, and qualification boundary cannot drift independently.
