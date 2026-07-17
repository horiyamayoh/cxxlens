# ADR 0077: Persisted publication identity binding

- Status: accepted
- Date: 2026-07-18
- Issue: #146

## Context

Publication creation derived `publication_id` from series, snapshot, sequence,
and parent. Persisted payload decoding trusted the stored ID, however, so a
payload and SQLite primary key changed together could detach that ID from its
identity fields while retaining a valid physical checksum.

## Decision

Every publication record must satisfy:

```text
publication_id = H(series_id, snapshot_id, sequence, parent_publication)
```

A shared validator recomputes this canonical identity and rejects a mismatch as
`store.corrupt`. Memory and SQLite publication, persistence, decoding/loading,
read exposure, and compaction paths use that validator. SQLite load quarantines
an invalid record as corrupt instead of exposing its decoded payload.

`physical_generation`, state, and the corruption marker remain operational
fields outside publication identity. Copy-on-write compaction validates the
record both before and after changing `physical_generation`; the publication ID
must therefore remain unchanged.

## Consequences

- Synchronizing a tampered payload ID with its SQLite primary key and checksum
  no longer bypasses validation.
- Mutating any identity input without deriving the matching ID is fail closed.
- Valid compaction and reopen preserve the publication ID while advancing only
  the physical generation.
