# SDK projection evidence ownership

Contract: `cxxlens.sdk.projection-evidence-ownership/1`.

The independent consumer is Monet's snapshot verifier. It projects Source,
Exceptional Exits and Exceptional Routes from the same immutable query results
and retains all original evidence. The existing projections copy each retained
annotated row. The added opt-in ownership mode lets these projections retain the
exact immutable query row instead, eliminating that physical copy.

The default remains detached evidence. `finite_population_evidence::row` remains
the ordinary mutable detached row, including two-argument brace construction.
Consumers choosing `projection_evidence_ownership::shared_immutable` read every
evidence row through `original_row() const`. Its private strong owner survives
projection copies, moves, destruction of the input and resetting `source_queries`.
The detached `row` field is not the shared-mode original. Raw span inputs always
produce detached evidence because the library cannot retain the caller's owner.

Only the SDK's private query-result access creates a shared evidence row. It checks
the exact row address within the immutable result before making an aliasing
strong owner. An arbitrary public shared pointer or a caller's mutable row cannot
establish this ownership or generic validation. Every projection still performs
its existing complete type, state, domain, World, FK, closure, row and evidence
admission. This mode changes no proved, disproved, unknown, partial or conflicting
result and does not infer availability. Coverage, unresolved reasons, conflicts,
guarantees, provenance and all raw row side channels remain available.

Work charges follow the actual copy or strong-owner construction performed.
Row/evidence/text limits, canonical ordering and current cancellation checkpoints
remain in force. Owner-view storage, handle slots and temporary growth are charged
before allocation. Existing immutable-query backing reservations stay live until
all referring projections and handles are destroyed; a handle's small size is
not a bound for its complete backing owner. Projection usage covers additional
projection allocations and retains the existing conservative selected-row
reservation in both modes. It is not a complete bound for caller-owned query
data; the caller retains its input backing reservation while any query or evidence
handle survives. The shared mode does not newly copy or allocate that already
retained backing. Raw spans and default projections keep their complete existing
copy charges and ownership.

The authority is the integrated design's complete input and bounded-resource
invariants and the author SDK public API catalog. The implementation scope is
the common evidence/limit contract, private query-result owner access, the three
projectors, their ordinary controls and Monet's opted-in readers. The mode is
supported for these three projectors; other projections keep detached evidence.
There is no wire or relation change. Full ordinary admission precedes retaining
a row. Failure and cancellation do not return a partially admitted projection.
The completion path is full row/DTO parity, independent lifetime and raw fallback
controls, real bounded-work/storage/cancellation controls, then Monet's verifier.
