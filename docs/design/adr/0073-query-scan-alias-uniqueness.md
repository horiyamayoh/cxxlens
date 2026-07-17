# ADR 0073: Query scan alias uniqueness

- Status: accepted
- Date: 2026-07-18
- Issue: #142

## Context

Logical Query IR uses `(source_alias, stable column ID)` as its query-local
column occurrence identity. The typed builder normally disambiguates a
self-join, but public IR can be assembled directly from decoded node argument
JSON. Two reachable scans with the same alias therefore produced identical map
keys. Shape construction and runtime row combination retained only one side,
and a join predicate could compare that retained cell with itself.

## Decision

Every reachable `query.scan.v1` node in one Logical Query IR must have a unique
alias. `logical_query_ir::validate()` decodes every scan argument, establishes
the rooted graph closure, and rejects the second occurrence deterministically with
`sdk.query-duplicate-scan-alias`, identifying the duplicate alias as the error
field. This validation precedes any authoritative canonical identity use and is the
mandatory precondition used by the reference engine before any execution.

The alias remains semantic and participates in the logical digest. We do not
introduce a second hidden occurrence identifier: column references, typed node
shapes, intermediate rows, predicates, and terminal projection continue to use
the same explicit `(source_alias, stable column ID)` identity. A self-join must
therefore use distinct aliases such as `left` and `right`.

When `builder::union_with()` combines branches that use the same alias for the
same descriptor, it reuses the identical scan DAG node instead of manufacturing
a second occurrence. A conflicting descriptor for that alias is rejected. This
preserves the existing union surface while keeping the reachable scan-node
invariant literal.

The quality reference validator enforces the same invariant for nested JSON IR.
Regression tests cover direct validation, decoded scan arguments, stable error
identity, pre-execution rejection, successful distinct-alias self-join, and the
actual left/right value pairs.

## Consequences

- Direct and builder-produced IR share one fail-closed occurrence invariant.
- Duplicate aliases cannot collapse right-side type, value, or provenance.
- Existing valid IR is unchanged; invalid duplicate-alias IR now fails before
  a result or physical plan can be produced.
- Users who scan the same relation more than once must explicitly choose unique
  aliases and qualify both sides.
