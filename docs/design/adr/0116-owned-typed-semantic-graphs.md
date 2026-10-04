# ADR 0116: Owned, conditioned typed semantic graph projections

- Status: Accepted
- Contract-ID: `cxxlens.typed-semantic-graphs.v1`
- Authority: relation registry, Public C++ API Catalog, ADR 0113
- Surface: installed LLVM-free `sdk/semantic_graphs.hpp`, target `cxxlens::query`
- Write scope: SDK values/projection, consumers and ordinary tests

Independent consumers are monet-code (typed graph measurements, architecture,
impact, UML and reading views) and cxxmonster (Core32 topology and explanatory
relation layers). Existing portable queries own every annotation but consumers
currently implement different endpoint/condition joins. Share this join and keep
application formulas, component policies and rendering in the applications.

`semantic_graph_input` supplies seven independent row groups: compile units,
source files, C++ entities, typed entity edges, call sites, direct/candidate call
targets and processed includes. `semantic_graph_spec` names a graph, node kinds
and edge kinds. `*` means all observed C++ entity kinds. File nodes are separately
requested. Special edge kinds `calls` and `includes` use their actual source
relations; other kinds select `cc.entity_edge`. No implicit edge-type union or
candidate-to-definite promotion occurs. Specs, ID sets and serialization use
bytewise canonical order. Spec identity describes the projection, not a Git SHA,
compiler pointer, display label or filesystem root.

Each graph has its exact universe, variant and interpretation. Endpoints join
only in that world. Calls join through the actual call-site ID and compilation
unit; includes join actual file IDs. Missing endpoints, target sets or scans
remain explicit edges/gaps. Duplicate equal observations merge all evidence;
overlapping unequal payloads remain conflicting candidates. A node excluded by
the requested node-kind policy remains a boundary rather than an invented node.

`complete` refers to validation and enumeration of the supplied observed graph,
never to whole-program or runtime closure. The row-input overload requires the
caller to state finite observation completeness explicitly (default false) and
cannot assert closure. The query overload checks complete execution/input for
each required independent scan. Missing scans, input frontiers and conflicts
prevent a complete graph. It carries actual query closure independently and
requires all needed closed scans and no graph frontier before setting `closed`.
No absence is proved by filtering an open graph into an empty graph.

The projection owns relation-labelled annotated evidence and original query
plans/results, preserving claim contributors, guarantee fragments, coverage,
closure, conflicts, unresolved items and differential disagreement. It retains
candidate edges with their actual resolution. A consumer can draw partial graphs
but cannot use them as exact graph-family input. Proved/disproved graph properties
need a closed domain or an actual positive witness; missing closure yields unknown.

Positive row/byte/spec/world/node/edge/evidence budgets are checked before
retention and condition expansion, including original plan copies. Cancellation,
malformed rows, invalid specs and allocation failure return typed SDK errors.
Output is deterministic under row/spec order and evidence repetition. It performs
no I/O, compiler execution, claim adoption or provider certification.

Completion order: public value contract/catalog; bounded join and negative tests;
Monet canonical graph ownership/formulas/derived tables and native archive/UI;
Monster input topology/Core32/scene layers; full regression and installed consumers.
Unknown prerequisites name the absent scan, endpoint, closure or exhausted budget.

The existing open `cc.entity_detail.v1.flags` column supplies record abstraction
evidence to both applications. `record_definition` marks an actual complete record
definition; `abstract` is present only when Clang's completed, non-dependent C++
record reports `isAbstract()`. Forward/dependent/incomplete definitions carry
`abstractness_unknown`. Consumers cannot interpret a forward declaration's absent
`abstract` flag as a concrete record. Observing these flags must not instantiate
a template or evaluate a lazy exception specification.
