# ADR 0114: Owned finite control-flow projection

- Status: Accepted
- Contract-ID: `cxxlens.finite-control-flow-projection.v1`
- Authority: relation registry `cc.body/cfg_node/cfg_edge`, ADR 0113
- Stability: versioned public C++ source API; LLVM-free
- Write scope: public SDK/catalog, query component, both application adapters and tests

Monet evaluates body and graph metrics; Monster evaluates the complexity distribution
and needs body explanations. Both currently repeat cardinality and endpoint joins.
The shared SDK owns that join over already detached, conditioned public rows. Compiler
objects, application metrics, risk policy and morphology remain outside this surface.

`project_control_flow` accepts the three independent scans or their borrowed row spans.
The borrowed overload is a pure value projection for already validated claim rows; it
does not admit a provider, adopt claims into a Store or replace the transfer decoder.
The application-query overload retains every original scan and result side channel.
The result owns input evidence rows, with index references from each body/node/edge.
Coverage, closure, guarantee, producers, provenance, unresolved items, conflict and
disagreement are never replaced by a single confidence or structural-completeness flag.

The join key is body ID plus universe, one variant fragment and interpretation.
Nodes additionally match compile unit and function. Edges have no body column: both
endpoints must belong to the same unique body in that world and have the same actual
compile unit/function. A missing endpoint remains an explicit frontier. Distinct edge
IDs are counted even for parallel endpoints. Equal payloads merge all evidence refs;
unequal payloads with an overlapping conditioned key yield `conflicting`, never
first-wins. Output order and evidence refs are deterministic under input permutation.

`complete` proves only the finite local compiler CFG enumeration: body eligibility is
`closed`, distinct node/edge cardinalities match the declared counts, entry/exit exist,
all endpoints and owners agree, and no local gap/conflict remains. It does not prove
project-wide absence, external call behavior, points-to completeness or global closure.
`partial` represents unresolved local enumeration and retains observed nodes/edges
and stable actionable gap codes;
ineligible dependent or unavailable bodies do not become empty closed graphs.
`conflicting` retains competing body/node/edge evidence. Application-level
`not_applicable` requires an explicitly absent CFG domain; missing scans are partial.
No global claim is `disproved` merely because a finite projection contains no edge.

Limits cover input rows, condition expansion, retained and expanded canonical row bytes,
bodies and per-body nodes/edges. The application overload separately bounds the number
of retained scans and the owned bytes of all their plans, including unrelated scans.
Immutable result storage is shared; copying a plan does not copy its query-result rows.
Missing-source edges retain individual unresolved items, while each function world's
affected bodies receive one gap summary rather than a Cartesian expansion of all gaps.
Checked sums precede retention. Cancellation is checked
before work and during each bounded pass. Invalid limits, resource exhaustion or
cancellation produce a typed failure with no partially published result. Neither API
performs filesystem writes or other irreversible effects.

Completion order is SDK contract and fault/condition/determinism tests, then the actual
Monet function/graph evaluator and Monster complexity consumers, then native installed
SDK save/load tests. Unknowns require the missing relation, body cardinality, endpoint
or compatible conditioned owner indicated by their gap code; they are not zeros.
