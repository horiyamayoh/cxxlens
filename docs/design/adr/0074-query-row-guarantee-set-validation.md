# ADR 0074: Query row guarantee set validation

- Status: accepted
- Date: 2026-07-18
- Issue: #143

## Context

`annotated_row::canonical_form()` canonicalized `contributor_guarantees` by
sorting and deduplicating a local copy. The result schema already declared this
array `uniqueItems: true`, while a cursor-owned public row retained its original
vector. Without a direct public-value invariant, duplicate entries could be
observed by one surface and silently collapse in canonical identity.

The bound `contributor_edges` projection check already rejected many such
states indirectly, but the guarantee set's own contract, stable error field,
and independence from another projection were not explicit.

## Decision

`annotated_row::validate()` validates every `claim_guarantee` and then requires
the vector to be strictly ordered by the exact canonical structured guarantee
JSON key. Equal adjacent keys and noncanonical insertion order are rejected as
`sdk.query-row-invalid` with field `contributor_guarantees` before projection
parity is considered.

Runtime publication continues to canonicalize contributor edges first and
derive the guarantee projection from them. Consequently cursor views, detached
owned copies, canonical row JSON, memory/SQLite comparison, and the result
schema observe one identical nonempty set. `canonical_form()` remains
order-independent for diagnostic use, but its normalization cannot make a
duplicate vector a second valid public state.

## Consequences

- `{g}` and `{g,g}` cannot coexist as distinct valid public rows with one
  canonical identity.
- Distinct guarantee insertion orders have the same canonical form, while only
  the canonical public ordering validates.
- Every operator-produced row is checked through the same public validator in
  the runtime acceptance matrix.
- The schema's `uniqueItems: true` and the C++ public boundary now express the
  same cardinality contract directly.
