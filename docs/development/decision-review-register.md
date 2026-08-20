# Development decision and review register

`schemas/cxxlens_ng_development_decision_register.yaml` is the versioned inventory of
repository-level decisions that must remain distinct from implementation and production
qualification. It records the selected alternative, rejected shortcuts, review state,
implementation state, qualification state, and preserved WIP provenance.

The register does not replace subsystem authority. A high-risk decision remains inactive until an
accepted ADR and machine-readable contracts resolve it and an independent reviewer binds the exact
reviewed `main` commit to a canonical GitHub issue comment. The repository owner does not provide a
separate interactive approval gate.

Delivery uses atomic fast-forward commits on `main`. A pull request can remain useful review or
external-contribution evidence, but its existence, review state, or branch status is not correctness
authority. Proposed high-risk authority is committed first, independently reviewed at that exact
commit, and accepted only by a later non-rewriting `main` commit.

The checker rejects duplicate decisions, unknown repository paths, high-risk self-review,
acceptance without a canonical review comment, qualification before implementation, and mutation or
deletion of preserved WIP provenance.
