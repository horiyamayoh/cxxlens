# ADR 0117: Owned finite record surfaces

- Status: Accepted
- Contract: `cxxlens.record-surface/1`
- Authority: NG integrated design, ADR 0113, relation registry and public API catalogue
- Scope: portable query projection, Clang observer, installed relation tags and both consumers
- Stability: versioned; additive pre-release relation and public API

monet-code needs the actual member surface of a class for object metrics, and
cxxmonster needs the same surface for structural signatures, containment and
state-access topology. The existing declaration and typed edge scans retain
observations but do not establish a finite empty member set. Counting an absent
ownership edge as zero would confuse an empty class with an incomplete input.

The compiler emits `cc.record_surface.v1` for each observed record declaration.
The record identity is its compile unit, semantic record entity, declaration
source and the profile `clang22-explicit-record-surface/1`. Counts and member
contents are payload, never identity. A complete definition enumerates distinct
explicit direct methods, instance fields and direct base specifiers. Static
methods and explicitly defaulted/deleted methods remain included; implicit
compiler-generated methods and static data members are excluded. Constructors,
destructors, conversions and declared member function templates are methods in
this policy. Redeclarations and inherited members do not add duplicate members.
The relation retains canonical member/base entity sets, declared cardinalities,
enumeration state and an actionable reason. An unmapped member or dependent base
retains its declared cardinality and makes the enumeration partial. Forward or
invalid declarations have unknown enumeration, never an empty complete surface.
Dependent record definitions may enumerate their explicit member declarations
while retaining an unresolved base frontier; no instantiation is forced.
Template wrappers sharing the templated declaration's semantic identity do not
produce a second incompatible entity kind or a typeless member detail. The
actual templated declaration supplies its kind, type, and member properties;
template structure and parameter observations remain separate relations.

The portable `query.project_record_surfaces` returns value-owned record candidates
with exact universe, variant, interpretation and compile unit. It validates
typed columns, source/file references, entity kinds, finite cardinalities,
direct ownership and matching declaration details before marking a surface
complete. Equal repeated observations retain all evidence. Incompatible rows
remain separate candidates with conflicting state; no first candidate wins.
Membership is direct and does not recursively flatten nested classes. The
projection retains every original query and its coverage, closure, guarantees,
unresolved items, conflicts and differential disagreements. Finite local member
enumeration does not supply relation-wide or whole-project closure.

Source bindings include each member declaration, not just the owning record.
Conflicting source snapshots or ranges remain conflicting even when each range
is independently within bounds. A declaration-only `inline` spelling may differ
between in-class and out-of-line declarations; its union and all original
declarations are retained without inventing a member conflict. Differences in
semantic properties such as static or virtual remain conflicts.

The outcome is proved for a validated local enumeration, disproved when its
declared cardinality or binding contradicts observed rows, unknown when a
definition/source/identity is unavailable, partial for an observed subset, and
conflicting for incompatible conditioned candidates. Consumers must keep those
states when calculating metrics or topology. Missing definitions can be supplied
through source/compile inputs; dependent bases require an observed specialization;
resource exhaustion requires a larger explicit budget or a narrower query.
An implementation gap is reported as development work rather than missing user
input.

Row, condition expansion, retained evidence, reference, output surface/member and
source-plan budgets apply before retention. Cancellation and allocation failures
are typed errors. No Clang pointer, filesystem path, timestamp or quality
certificate is part of the public value. The write scope is this ADR, the registry
and catalogue, generated relation header, private observer, query projection and
ordinary tests and consumer adapters. No source edit or external effect occurs.

The implementation order is the relation and native cardinality producer, the
shared validated projection, native consumer metrics/signatures, then archive and
navigation. Tests cover explicit empty versus forward records, static/implicit
and out-of-line members, templates, variant separation, duplicate/conflicting
rows, incorrect cardinality/source/owner, missing scans and resource/cancellation
faults. Real native analyses exercise the same projection in both applications;
API or green test counts alone do not establish consumer completion.
