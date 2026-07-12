# Immutable fact store operations

The M1 fact store persists reducer output as a versioned canonical binary snapshot. JSON remains an
export projection and is not a persistence input. `fact_store_port` is the only backend boundary:
the in-memory and SQLite implementations publish the same detached, canonically ordered values and
balanced coverage.

Provisioning supplies an explicit database path and the expected workspace, fact schema, semantics,
adapter, and extractor versions. Issue #21 owns the workspace cache-location policy; the store does
not infer a path or silently select a neighboring cache. SQLite is loaded through the runtime dynamic
library port, so no SQLite or Clang type enters a stable public header.

Each write follows `created -> staged -> validated -> committed`. A transaction holds the single
writer lease from begin through commit or rollback. Existing readers retain their immutable snapshot;
new readers see the replacement only after the SQLite transaction commits and the current generation
is atomically published. Any failed phase rolls back and leaves the prior generation readable.

Opening a valid database with a different workspace/schema/semantics/adapter/extractor tuple reports
`rebuild_required`; incompatible rows are never returned. `rebuild()` replaces the valid incompatible
cache with an empty compatible generation. Invalid binary payloads, failed SQLite integrity checks,
and truncated databases report `facts.store-corrupt` and are preserved for diagnosis. Corrupt stores
are not rewritten in place: provisioning must quarantine or remove them through the filesystem port
before creating a fresh cache.

`compact()` runs SQLite `VACUUM` only without an active writer. Query order is always
`(fact_kind, stable_key, fact_id)` and never depends on insertion order, `rowid`, index layout, job
count, checkout root, database path, or `VACUUM`.

The SQLite schema keeps the canonical snapshot blob authoritative and maintains deterministic
secondary indexes for fact kind/stable key, semantic owner, source file/range, edge forward/reverse
lookup, and authoritative coverage units. Index rows are rebuilt from the validated snapshot inside
the same transaction and cannot publish independently.
