# Development decision and review register

`schemas/cxxlens_ng_development_decision_register.yaml` is the versioned inventory of
repository-level decisions that must remain distinct from implementation and production
qualification. It records the selected alternative, rejected shortcuts, review state,
implementation state, qualification state, and preserved WIP provenance.

The register does not replace subsystem authority. A high-risk decision remains inactive until an
accepted ADR and machine-readable contracts resolve it and an independent reviewer binds the exact
reviewed `main` commit to a canonical GitHub issue comment. The repository owner does not provide a
separate interactive approval gate.

`decision_status: decided` means that the owner has selected a direction and rejected alternatives;
it does not mean that a Proposed ADR is Accepted. `review.status: required` remains set after a
rejecting review while P0/P1 findings are unresolved. In that state, `review.refs` records the
completed counterexample review, and activation remains blocked until a corrected exact commit is
reviewed and accepted.

Delivery uses atomic fast-forward commits on `main`. A pull request can remain useful review or
external-contribution evidence, but its existence, review state, or branch status is not correctness
authority. Proposed high-risk authority is committed first, independently reviewed at that exact
commit, and accepted only by a later non-rewriting `main` commit.

The current offline checker rejects duplicate decisions, unknown repository paths, high-risk
self-review presented as complete, completion before review, and qualification before implementation.
It validates the shape of preserved WIP provenance but does not yet make prior heads immutable or
authenticate GitHub comment bytes. Those two enforcement gaps are blocking findings against Proposed
ADR 0105 and must be closed before its receipt mechanism can be Accepted.
