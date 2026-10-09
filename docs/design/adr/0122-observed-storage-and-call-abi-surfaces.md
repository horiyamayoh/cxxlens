# ADR 0122: Observed storage and call ABI surfaces

- Status: Accepted
- Contract: `cxxlens.abi-surface/1`
- Authority: NG integrated design, relation registry, public API catalogue, ADR 0113 and 0117
- Scope: installed portable values/relations, private Clang observations, native-only compiler borrow, monet-code and cxxmonster
- Stability: versioned additive pre-release surface

monet-code needs padding divided by the actual size of a class and the number of
distinct observed ABI fingerprints of the same semantic class or function across
its selected compiler inputs. cxxmonster needs actual storage and interface
structure for signatures and variant explanations. Existing `cc.layout_fact`
size, alignment and field offsets do not enumerate base or hidden storage, and
its digest is not a complete storage/call-interface fingerprint. Consumers do
not infer padding by adding field sizes or treat one arbitrary variant as a
complete population.

`cc.abi_surface.v1` identifies an observation by compile unit, semantic entity,
actual declaration source and profile `clang22-storage-and-call-interface/1`.
Its ABI and occupied-layout states are independent. Counts, ranges and the
fingerprint are payload. Missing or dependent definitions remain unknown;
retained storage with an unsupported facet remains partial. Equal observations
retain their evidence and incompatible conditioned candidates remain
conflicting. No source, display name, LLVM type name, worker order or arbitrary
preprocessor option is fingerprint authority.

## Occupied storage

For a complete observed record, size and alignment come from `ASTRecordLayout`.
The profile measures bytes reserved for direct member complete-object storage,
recursively observed nonvirtual base payload and hidden ABI storage. A member
record's internal padding belongs to that member's complete-object storage; it
is not attributed again as padding of its owner. Bases are walked as base
subobjects, excluding their separately laid-out virtual bases. The most-derived
record adds each actual virtual base once at its compiler offset. Own virtual
function and virtual base pointers use the compiler's actual placement and
pointer width. No guessed base width or pointer offset supplies a range.

Named bitfield payload occupies each byte touched by its actual bit offset and
width. Unnamed and zero-width bitfields supply no payload range. Flexible and
zero-length array members supply no fixed-size payload. References occupy the
actual target pointer storage rather than the referred type's size. Packed
records, unions and overlapping members are observed through the same compiler
layout. A direct member's complete storage is atomic in this profile. Empty
objects and empty base/member subobjects supply a one-byte identity-address
storage marker when their actual address lies within the most-derived object;
markers can overlap other storage. This keeps the ABI-required empty-object
identity byte from becoming padding while permitting empty-base and
`no_unique_address` reuse. All ranges are unioned, so overlapping storage,
shared hidden pointers and repeated empty markers are counted once.

The relation retains the canonical union of occupied byte ranges as concatenated
little-endian `uint64 begin, uint64 end` pairs. Each pair is nonempty,
`0 <= begin < end <= byte_size`; pairs are strictly ordered, disjoint and
nonadjacent. A complete empty set is distinct from absent/unknown input. The
portable projection validates framing, canonical order and bounds and computes
`occupied_bytes = sum(end-begin)` and `padding_bytes = byte_size-occupied_bytes`.
Contradictory ranges are conflicting, never silently repaired by the consumer.
A zero-sized extension remains a real zero size; it cannot supply a nonzero
padding-ratio denominator. Unsupported compiler storage is an explicit frontier.

## ABI profile

The actual compiler context binds the Clang version, target triple, target data
layout, target ABI and C++ ABI, Clang ABI compatibility policy and vtable layout
policy. General `-D`, source names, physical roots, optimization and quality
settings are excluded. Actual field/base offsets, storage types, virtual base
placement, hidden pointers and Itanium vtable slot kinds, call shapes and thunk
adjustments form the record storage/call-interface signature. Canonical LLVM
storage types are represented structurally; their names and host pointers are
excluded. This is a physical interface fingerprint, not a proof of source API,
linkage-name, semantic dispatch or whole-program binary compatibility. In
particular, swapping two identically lowered virtual methods does not assert
that their behavior or named interface is interchangeable.

Function observations use the actual compiler `CodeGenOptions`, not defaults,
with Clang CodeGen ABI lowering. They retain actual LLVM calling conventions,
return/argument types, ABI attributes and `CGFunctionInfo` argument kinds,
coercions, extensions, indirect alignment/address space and inalloca structure.
Constructor complete/base and destructor complete/base/deleting entry points
are observed separately inside one declaration signature. A body is not needed
to observe a declaration's call ABI. Dependent, incomplete or unsupported
interfaces remain unknown, without forcing a template or method-body
instantiation. A function has no object-layout observation; this does not
invalidate its known call ABI. Microsoft vtable interfaces and unsupported
lowered type constructors remain explicit implementation frontiers; unsupported
platforms are not advertised as supported by this addition.

`borrowed_translation_unit::code_generation_options()` is a native-only,
callback-scoped const borrow of the actual `CompilerInstance` options, with the
same lifetime as its AST and preprocessor. Clang pointers and headers do not
enter the portable SDK, a relation, a snapshot, or a consumer output.

The ABI context digest uses domain `cc.clang22.abi-context.v1`. The fingerprint
uses `sdk::semantic_digest("cc.clang22.abi-surface.v1", payload)`, where payload is
`u64le(context_length), context_utf8, u64le(signature_length), signature_bytes`.
The portable projection verifies that digest and retains the opaque versioned
signature bytes. Unsupported profiles do not become complete by merely carrying
a digest.

## Binding, results and completion

`query.project_abi_surfaces` owns all candidates, original relation rows and
application-query side channels. Its default evidence remains detached and
mutable. Explicit `abi_surface_limits::evidence_ownership = shared_immutable`
retains the exact immutable query row through `abi_surface_evidence::original_row()`;
raw-span callers keep detached evidence. Strong evidence ownership survives
input destruction and resetting the projection's `source_queries`. The caller
keeps its query backing reservation while any shared evidence survives. This
opt-in follows `cxxlens.sdk.projection-evidence-ownership/1` and requires rebuilding
SDK consumers because ABI evidence and limits gain fields. It changes neither
canonical evidence order nor relation/wire formats or semantic availability.
It validates exact compile unit, universe,
variant and interpretation; actual entity kind, declaration source, source
snapshot and range; sizes, ranges, profile and digest. ABI and layout facets
preserve independent conflict/frontier states. Local completion never supplies
relation-wide closure. Independent compile-unit and ABI-scan input-completion
flags default false for raw callers; the application-query overload derives them
from the original scans and preserves every original query.

A validated local observation is proved under this profile; bad ranges, digest
or binding disprove it; unavailable definitions/profiles are unknown; retained
subsets are partial; incompatible observations are conflicting. Missing input
requires the actual definition/compile input or an observed specialization.
Unsupported compiler layouts require implementation work, not a user quality
certificate. Variant populations remain the consumer's finite selected-domain
calculation and require every relevant input and observation before producing
an exact count.

Operations, depth, signature bytes and occupied ranges are bounded before
retention. Ordinary portable/native tests cover empty, packed, union,
bitfield/reference, base/overlap/virtual storage, actual call lowering, changed
and unchanged ABI across variants, source relocation, condition separation,
missing/conflicting bindings, malformed bytes, budgets, cancellation and input
ordering. Implementation order is schema and producer, shared projection,
consumer padding/ABI evaluators and parent aggregation, then saved/report paths.
There is no quality admission, certificate, Git SHA ledger or test-record system.
