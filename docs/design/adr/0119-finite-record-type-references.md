# ADR 0119: Finite record type-reference observations

- Status: Accepted
- Contract: `cxxlens.record-type-references/1`
- Authority: NG integrated design, relation registry, ADR 0113 and 0117
- Scope: additive record payload, owned SDK projection, Clang producer, both consumers
- Stability: versioned pre-release additive columns

monet-code needs an observed class's actual type coupling. cxxmonster needs the
same nominal type relationships for structural topology and signatures. Existing
`uses_type` edges cover simple declaration types, but their absence cannot prove
that function results, parameters, member pointers, or template arguments contain
no record references. Display spelling is insufficient for this calculation.

Profile `clang22-explicit-nonsystem-nominal-type-uses/1` enumerates the distinct
canonical record and enum entities referenced by direct explicit field types, static data
member types, direct explicit method signatures, and base types. It follows
canonical pointer, reference, array, function, member-pointer, atomic, vector,
complex, and observed template argument structure. It includes function results
and parameters. It does not expand the referenced record's members or instantiate
a specialization. Compiler system-header nominal entities are excluded; their
observed template type arguments are still visited. Consumer first-party and
self exclusions remain separate from this compiler system-header policy.

The optional `type_reference_count`, `type_reference_targets`,
`type_reference_state`, `type_reference_profile`, and `type_reference_reason`
columns form one finite local observation. The count is the observed target-set
cardinality even when partial. A complete empty definition proves an empty
reference set under this policy. A forward declaration is unknown. Dependent or
unsupported structure and missing nominal identities produce partial results
with an actionable reason. No unknown target becomes an exact empty population.
The optional target-set column uses ordinary conditional references: absent
means no assertion, a present empty set resolves vacuously, and every element of
a present nonempty set must resolve in the same semantic condition. An unknown
set cannot assert absence. Optionality does not relax target identity or type.

The owned SDK validates type, count, state, and profile separately from ordinary
member enumeration. A missing producer capability leaves this observation
unknown while preserving member counts. Invalid cardinality is conflicting;
incompatible candidate payloads and all original query side channels remain
available. The producer emits `uses_type` edges for its observed targets with the
actual record declaration source and compilation condition. A local set does not
grant incoming-reference or whole-project closure.

The AST traversal bounds depth, visited types, targets, and retained target bytes.
The portable projection charges targets and evidence against its existing row,
operation, reference, retention, and cancellation limits. No Clang pointer leaves
the producer. Positive, negative, native, condition-separation, determinism, and
resource tests cover nested signatures, static members, self references, system
exclusions, dependent templates, forward declarations, and conflicting sets.

An unknown result requires a source definition, an observed specialization, or
support for the reported type constructor. Consumer work follows the shared
projection: class coupling in monet-code, then structural topology in cxxmonster.
Ordinary tests and Git provide development history; no quality certificate,
tested-SHA ledger, or additional operational evidence system is involved.
