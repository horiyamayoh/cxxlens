# ADR 0118: Finite method member-access observations

- Status: Accepted
- Contract: `cxxlens.body-member-access/1`
- Authority: NG integrated design, relation registry, ADR 0113 and 0117
- Scope: additive body payload, Clang observer, both ordinary consumer paths
- Stability: versioned pre-release additive columns

monet-code's LCOM/TCC calculations need every direct method's actual field
accesses. cxxmonster's state topology needs the same facts. Existing access edges
provide witnesses but their absence alone cannot distinguish no access from
incomplete observation.

An observed body carries an optional `member_access_count` and
`member_access_state` under profile `clang22-explicit-member-access/1`. The count
is the number of distinct source-span/field-entity accesses observed while
traversing that method, including written constructor member initializers.
Calls to methods and references to static data are separate populations.
The typed `member_access_targets` set identifies field targets independently of
whether the admitted source scope contains their declarations; the set and
distinct access cardinality are both checked against the actual access edges.
Repeated traversals of the same semantic access do not add weight. A missing
source or field identity retains a partial state, never a complete empty set.
No compiler-generated method is introduced or instantiated to obtain a body.

Consumers compare the declared count to the same-unit, same-universe,
same-variant, same-interpretation `accesses_member` edges targeting fields.
Resolved accesses to inherited or external fields remain witnesses, while direct
record cohesion includes only that record's direct field population. The full
body, source, query snapshot, and record membership are retained. A malformed
endpoint or conflicting payload disproves the binding; an absent body is unknown;
an observed subset is partial; incompatible candidates are conflicting. Complete
local access enumeration grants no project-wide type-use or call closure.

Missing method definitions require source input; dependent accesses require an
observed specialization. Producer capability gaps are development work, not
input faults. Retention uses the existing bounded compiler fact output and
portable projection/evaluation budgets. No compiler pointer crosses the SDK.
Native and consumer tests cover empty accesses, reads/writes, initializers,
inherited/static fields, missing rows, conflicting payloads, and condition
separation. This adds semantic data, not a quality certificate or test ledger.
