# ADR 0075: Query unary boolean canonical fold

- Status: accepted
- Date: 2026-07-18
- Issue: #144

## Context

The public `query::all()` factory rejected only an empty span and emitted an
`and` wrapper for one operand; `query::any()` inherited the same behavior. The
IR decoder correctly required at least two operands for an `and` or `or`
wrapper. A value successfully produced by the SDK was therefore rejected when
`where()` or a join rebound and decoded it.

## Decision

Zero operands remain invalid and return `sdk.query-empty-expression`. Exactly
one nonempty operand is returned unchanged by both `all()` and `any()`. Two or
more operands produce the existing canonical commutative `and` or `or` node.
The IR schema and decoder keep `minItems: 2`; unary boolean wrappers remain
noncanonical and fail as `sdk.query-predicate-shape` when supplied directly.

The fold preserves the operand's canonical JSON and referenced column vector
byte-for-byte. It is recursive by construction: `all({any({a,b})})` is the
same expression as `any({a,b})`, and its builder decode/encode round trip is
identical. A folded column-equality atom also retains the top-level predicate
kind required by inner and semi joins.

## Consequences

- Generic code may compose zero, one, or many inputs with an explicit boundary
  contract and no later factory/decoder disagreement.
- No redundant unary node enters logical identity or execution.
- Factory-success predicates work through `where`, `inner_join`, and
  `semi_join` whenever the folded atom is valid for that operator.
- Direct unary `and`/`or` JSON remains rejected by both schema and decoder.
