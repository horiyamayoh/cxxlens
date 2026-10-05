# ADR 0120: Finite observed record-definition inventories

- Status: Accepted
- Contract: `cxxlens.record-inventory/1`
- Authority: NG integrated design, ADR 0113 and 0117, relation registry and public API catalogue
- Scope: installed values/relations, private Clang observer, SDK projection and both consumers
- Stability: versioned, additive pre-release surface

monet-code needs incoming coupling and direct/transitive inheritance populations.
cxxmonster needs the same bounded record population for containment and structural
signatures. A complete member surface establishes one record's outgoing facts;
it cannot establish that all records in the selected compiler inputs were observed.

The compiler emits `cc.record_inventory.v1` once per successfully traversed compile
unit and semantic condition. Its identity contains the compile unit and profile
`clang22-explicit-admitted-record-definitions/1`. Counts, contents, classification
and completion state are payload. The profile enumerates distinct explicit record
definition surfaces admitted by the frozen source closure, including dependent
definitions and observed template specializations. Forward declarations and
compiler-created implicit records are excluded. The inventory also identifies
the subset whose declaration locations Clang classifies as system headers.
Application first-party policy remains the consumer's decision.

The observer counts an admitted definition even when its identity, declaration
source or valid definition surface cannot be retained. Such an inventory is
partial with a reason; omitted definitions cannot turn it into a complete empty
set. A traversal failure or exhausted producer budget fails the partition rather
than publishing a complete subset. Empty successful translation units have a
complete zero inventory. A failed or missing translation unit has no complete
inventory. Unsupported profiles remain unknown.

`project_record_surfaces` additionally owns these inventories. It validates the
declared count against the retained definition-surface set, the system subset,
actual record definition bindings, exact unit/universe/variant/interpretation,
and the absence of extra observed definitions for that unit. Contradictory
cardinalities, classifications and candidates remain conflicting. Missing
bindings remain partial. Equal observations retain all evidence. Member, base
and type-reference completion remain independent prerequisites; a dependent
signature does not invalidate the record population by itself.

The projection separately reports whether the independent build-unit scan
enumerated its selected input domain completely. Raw-value callers must supply
this assertion explicitly; its default is false. The application-query overload
derives it from that scan's execution, input completeness and conflict axes,
and retains the entire original query result. Inventory completion never
supplies scan completion: the independent inventory scan's input/execution and
conflict axes are reported separately, with the same false default for raw input.
Neither boolean
supplies general relation-wide or whole-project closure. A consumer may close
an inverse calculation only over a declared finite observed domain when every
selected unit in that semantic condition has a complete inventory and every
required local relationship facet is complete. Uncompiled files, unobserved
variants and future template instantiations remain outside that domain.

Consumers merge repeated header definitions by conditioned canonical record
identity and compare all relationship payloads before using one population
member. First-party and system exclusions use the actual conditioned source
classification and compiler system subset. Unknown classification is a
frontier, not an exclusion. Incoming type use requires every included record's
finite type-reference facet; inheritance requires the actual finite base
population. Cycles and missing internal endpoints prevent a closed transitive
inheritance result. An external base remains an explicit boundary.

The outcome is proved for this validated finite population, disproved by
contradictory counts or bindings, unknown for missing profiles/units, partial
for retained subsets, and conflicting for incompatible observations. Completion
actions are source/compile-input corrections, observed specialization or larger
explicit budgets. No quality admission, test-run record or Git revision is part
of the contract. This is semantic input to product calculations.

Existing row, condition, byte, evidence-reference, member, operation, query-plan
and cancellation limits apply before retention; inventory output also has an
explicit count limit. No compiler pointer escapes the observer. Implementation
order is schema and producer, shared projection, both consumer calculations,
then their saved/output/UI paths. Ordinary tests exercise empty/forward/system/
dependent definitions, duplicate headers, missing or failed units, variant and
unit separation, wrong counts/subsets/bindings, conflicts, reordered inputs and
resource/cancellation failures.
