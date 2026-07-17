# ADR 0076: Query relation requirement reconciliation

- Status: accepted
- Date: 2026-07-18
- Issue: #145

## Context

Query builders merged relation requirements by descriptor ID and silently kept
the first descriptor. Relation IDs contain the semantic major but not the minor,
so two operands using compatible additive minors have the same ID. First-wins
discarded version, digest, and column information and made validation depend on
operand order.

## Decision

Equal descriptors deduplicate. For unequal descriptors with one ID, the exact
higher-minor descriptor is retained only when:

- name, semantic major, semantics, owner, keys, identity, references, merge,
  and conflict projection are equal;
- every lower-minor column exists identically in the higher minor; and
- every newly added column is non-required and has an optional value type.

The higher descriptor's version, authority contract, runtime canonical form,
and bound descriptor digest are retained without synthesizing a new authority.
Every unprojected scan occurrence is expanded to that descriptor before join or
union schema comparison. Output columns are ordered by source alias and stable
column ID so the reconciled schema is operand-permutation invariant.

An unequal descriptor at the same minor, a removed or changed column, a
nonoptional addition, or any semantic/key/reference/identity policy difference
fails as `sdk.query-relation-requirement-incompatible` with the relation ID as
the stable field. There is no fallback to either operand.

## Consequences

- Compatible `v1.0 + v1.1` scans bind one exact v1.1 requirement and preserve
  the optional column for every occurrence.
- Snapshot binding uses the retained minimum minor and exact descriptor shape.
- Join, union, and static/dynamic builder paths share the same reconciliation.
- Incompatible descriptors reject identically in both operand orders.
