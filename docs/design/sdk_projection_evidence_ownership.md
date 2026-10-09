# SDK projection evidence ownership

Contract: `cxxlens.sdk.projection-evidence-ownership/1`.

The independent consumer is Monet's snapshot verifier. It projects Source,
Exceptional Exits, Exceptional Routes and ABI Surfaces from the same immutable query results
and retains all original evidence. The existing projections copy each retained
annotated row. The added opt-in ownership mode lets these projections retain the
exact immutable query row instead, eliminating that physical copy.

The default remains detached evidence. `finite_population_evidence::row` remains
the ordinary mutable detached row, including two-argument brace construction.
`abi_surface_evidence` provides the same detached default and immutable accessor;
ABI callers select the mode through `abi_surface_limits::evidence_ownership`.
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
the common evidence/limit contract, private query-result owner access, the four
projectors, their ordinary controls and Monet's opted-in readers. The mode is
supported for these four projectors; other projections keep detached evidence.
There is no wire or relation change. Full ordinary admission precedes retaining
a row. Failure and cancellation do not return a partially admitted projection.
The completion path is full row/DTO parity, independent lifetime and raw fallback
controls, real bounded-work/storage/cancellation controls, then Monet's verifier.

ABI ordering retains the complete unsigned canonical row order. In shared mode,
the complete escaped first `claim_contributors` array decides order between
different arrays. A singleton printable ASCII claim can expose that complete
array through a checked byte view with both quotes and brackets, without a
temporary string. Other arrays use the existing escaped encoder. Equal arrays
retain the existing full canonical row comparison.
Decoder-derived exact row sizes enforce the full evidence-byte limit, including
the current locale's multiplicity spelling. Rows without that exact immutable
size fact retain the full encoder. Generic validation is reused only for rows
from their exact privately validated query owner; descriptor, type, World, FK,
closure and stricter projection checks still run. The payload work formerly
covering validation, full encoding and copying is omitted only when all three
operations are physically absent. ABI-surface rows retain that payload charge
because their scalar encodings still construct complete candidate keys. Geometry,
prefix construction, comparisons, owner lookup, alias construction and any
full-row fallback pay their actual work.
Default and raw callers retain the existing full validation, encoding and copy.
ABI evidence and limits have new binary layouts and require consumer rebuilds.

## Read-only query rows

Contract: `cxxlens.sdk.query-readonly-rows/1`. The independent consumer is Monet's
original declaration identity index. Its current bridge copies query rows and
subsequently repeats generic row validation, including temporary annotation
copies. `query_result::readonly_rows()` instead returns a `result_row_range`
holding the exact immutable query owner. `rows()` exposes a const span whose
lifetime follows the range, including range copies and moves, rather than a
cursor's generation. Creating the range performs no row copy or new allocation.

`rows_validated()` reports only whether that private owner has already completed
generic `annotated_row::validate()` for every stored row. The decoder supplies
this fact; an arbitrary row, pointer or caller-supplied flag cannot construct a
range. A false value requires ordinary generic validation. Consumers must still
enforce their own type, domain, World, FK, text and resource limits. Reuse applies
only to rows from the retained owner, and any mutable row overlay follows the
ordinary validation path. The query result retains all coverage, closure,
unresolved, conflict, guarantee and provenance side channels; the range changes
no semantic state and does not infer completeness. Callers retain the query
backing reservation while any range or span user survives.

This additive API preserves existing query-result and cursor layouts and their
behavior. It changes no relation or wire format. The implementation scope is the
new owner range, its accessor, public API catalog and lifetime/validation controls,
followed by Monet's explicit internal borrowing path. Support follows the existing
query runtime. Native results without the decoder fact use the ordinary validator;
failed decoding cannot produce an admitted range.
