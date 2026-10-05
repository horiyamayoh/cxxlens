# ADR 0121: Finite written direct-call populations

- Status: Accepted
- Contract: `cxxlens.body-direct-calls/1`
- Authority: NG integrated design, relation registry, ADR 0113, 0117 and 0118
- Scope: additive body payload, native Clang observations, ordinary public queries
- Stability: versioned pre-release additive columns

monet-code needs the union of a record's explicit methods and their resolved
direct callees for RFC. cxxmonster needs conditioned call occurrences for flow
and component structure. Existing call rows do not establish a complete empty
method population, and a project-wide query frontier need not invalidate an
independently observed method body.

Profile `clang22-written-syntactic-direct-calls/1` enumerates statically named
`CallExpr` occurrences in an actual written function body and its explicitly
written constructor initializer expressions. Declaration default arguments,
other functions and nested callable bodies are separate populations. Implicit
constructor/destructor invocations and runtime virtual or pointer targets are
not part of this syntactic direct-call profile. A virtual call retains its
statically named declaration without asserting its runtime target. A known
indirect call has no syntactic direct target; a dependent call remains partial.

The observed body carries `direct_call_count`, `direct_call_state`,
`direct_call_profile`, `direct_call_sites` and `direct_call_targets`. Count and
site IDs describe distinct original `cc.call_site` occurrences; target IDs are
the distinct original `cc.call_direct_target` entities. Repeated traversals do
not add observations. Each native written call must bind to its actual detached
source, caller, site and target. Missing identity/source/binding makes the local
population partial, never complete empty. No new template instantiation or
compiler-generated method is requested to obtain observations. Absence of the
optional payload is unknown, including observations from other producers.

Consumers join by compile unit, universe, variant and interpretation, validate
the complete profile, declared count, site references and target set against
the original independent scans, and keep all body/declaration sources and query
side channels. Direct-call completeness is independent of CFG construction,
member-access completeness and relation-wide closure. Missing rows or unknown
targets remain frontiers; incompatible payloads or contradictory sets are
conflicting. A complete local empty set proves only that this observed method
has no written syntactic direct callee.

Dependency order is the native body population, public typed relation/schema,
consumer RFC calculation and parent distributions, then report/archive use.
Native and consumer tests cover zero/repeated/self/external/static/virtual
calls, constructor initializers, nested lambdas, declaration defaults, indirect
and dependent calls, missing or contradictory witnesses, conditions, bounds,
cancellation and deterministic ordering. Compiler pointers remain local to one
callback. Existing output limits plus precharged population storage bound native
retention; consumer work and retention limits bound joins. This is semantic
input data, without certificates, quality admission or development ledgers.
