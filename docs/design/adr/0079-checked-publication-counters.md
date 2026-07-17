# ADR 0079: Checked publication counters

- Status: accepted
- Date: 2026-07-18
- Issue: #148

## Context

SQLite loading advanced the global physical generation before validating a
record. A checksum-invalid record could therefore inject `UINT64_MAX` even when
the record was later quarantined. Publication and compaction then used unchecked
addition for generation and sequence, allowing wrap to zero.

SQLite INTEGER also has a signed 64-bit storage domain, while the public counter
types are unsigned 64-bit. Exact boundary behavior requires a reversible
physical representation for the upper half of that logical domain.

## Decision

Only a committed record that has passed checksum, publication-identity,
decoded-record, collision, and semantic-graph validation contributes to the
global generation. Corrupt and every non-committed state are excluded from
counter authority.

Publication sequence and physical generation use one checked add-one helper.
Incrementing `UINT64_MAX` fails as `store.counter-overflow`; publish, the SQLite
transactional head recheck, and compaction all use this helper.

SQLite physical minor 2.6 stores values through `INT64_MAX` as nonnegative
INTEGER values and the upper unsigned half as the corresponding negative
two's-complement INTEGER. Load decodes both ranges to the exact logical u64 and
performs ordering and counter decisions only on decoded values.

## Consequences

- A corrupt maximum-generation row cannot poison later publications.
- Maximum generation or sequence never wraps to zero.
- Maximum minus one permits exactly one increment, then fails closed.
- Memory and SQLite expose identical unsigned-64 counter semantics.
- Existing nonnegative SQLite counter rows remain readable.
