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

Publication identity maps the logical unsigned-64 sequence to the canonical signed
integer without an implementation-defined C++ conversion: values through
`INT64_MAX` are unchanged and the upper half uses the exact two's-complement signed
value. This preserves the accepted identity bytes while making the codec portable.

For SQLite, physical generation is allocated inside the same `BEGIN IMMEDIATE`
transaction that inserts the publication. The writer revalidates all persisted
records against the authority-record conditions, derives the global maximum only
from intact committed records, and applies checked add-one before encoding the new
payload. A process-local generation loaded earlier is not allocation authority;
therefore long-lived writers publishing different series cannot reuse the same
generation.

The same allocator governs compaction. Compaction first orders the exact committed
authority replacement set by prior `(publication sequence, physical generation,
publication ID)`, then allocates a checked contiguous range with one distinct new
generation per publication. This preserves `open(snapshot_id)` resolver order and
prevents equal-sequence publications from becoming ambiguous. Memory applies the
same distinct checked assignment. SQLite obtains the replacement set from the
in-transaction database census, writes every replacement in one transaction, and
updates process-local maps/tokens only after commit; a stale process-local map is
never compaction authority. Overflow or any record failure rolls back the entire
replacement set and preserves every prior generation.

Within the publication transaction, the decoded committed authority census also
derives each series head. The durable head ID and sequence must exactly equal that
authority record before expected-parent CAS. Missing head metadata with existing
history, a same-parent sequence mismatch, or a duplicate publication ID is
corruption; `store.publication-conflict` is reserved for a genuine parent advance.

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
- Concurrent SQLite writers across different series receive distinct monotonic
  physical generations.
- Compaction preserves resolver order with distinct generations in memory and
  SQLite, and SQLite replacement is all-or-nothing across the full authority set.
- A stale Store instance cannot omit externally committed records during
  compaction or publish atop a missing/corrupt durable head.
