---
cxxlens_document_status: archived
cxxlens_authority: non-normative
cxxlens_replacement: docs/design/cxxlens_next_generation_integrated_design_ja.md
cxxlens_removal_issue: "#72"
---
# Issue #46 graph package Contract Candidate

## Authority and scope

This document and `schemas/cxxlens_package_contract_candidates.yaml#issue-46` are the normative
Phase B candidate for all six `graph` APIs. All six remain unimplemented/blocked. The candidate
fragment is non-installed; #53 owns stable-header integration, #52 owns the technical spike, and
\#54 alone owns the frozen transition.

## Authoritative graph snapshot and identity

A `semantic_graph` is an immutable detached value bound to one workspace catalog version and one
committed fact snapshot. It owns canonical node, edge, path, coverage, unresolved, and guarantee
rows through immutable shared backing. It never stores an AST/backend pointer. Returned spans and
references borrow only until graph destruction.

`graph_node_id` and `graph_edge_id` are SHA-256 typed IDs over semantics version, graph kind,
canonical semantic subject or endpoint IDs, relation role, normalized source/macro origin, and
variant set. A label, pretty type, display path, traversal order, confidence, or prose never enters
semantic identity. Existing `symbol_id`, file semantic keys, and call-site IDs remain the subject
authorities; graph-local IDs do not replace them.

| Condition | Contract |
|---|---|
| Parallel edges | Preserved when kind, source/call-site, dispatch role, spelling, or variant differs. |
| Exact duplicate | Same ID and equal payload merges sorted unique evidence contributors. |
| Conflicting duplicate | Same ID with different authoritative payload fails `graph.identity-conflict`. |
| Self edge | Preserved for recursion, self-include, or malformed hierarchy and participates in SCC/cycle accounting. |
| Variant divergence | Preserved per variant; never first-wins or unioned into an exact cross-variant edge. |
| Unknown endpoint | Uses an `unresolved_target` node plus typed unresolved; it is not omitted or converted to a display-name node. |

Canonical nodes order by kind then full node ID. Edges order by from ID, kind, to ID, source key,
variant IDs, then full edge ID. Paths order by length, node-ID sequence, then edge-ID sequence.
Unordered-container, discovery, filesystem, and worker completion order are forbidden authorities.

## Hierarchy, override, call, and include semantics

`class_hierarchy` represents direct base relations with access, virtual/dependent/incomplete state
in typed properties. Transitive reachability is derived by the shared closure engine; it does not
manufacture transitive authoritative edges. Diamonds retain both paths. Cycles/malformed inputs
produce SCC/cycle evidence and unresolved coverage rather than nontermination.

`override_graph` uses semantic method identity and authoritative override facts. Direct override,
transitive set, final/open-world, hidden/using overload, unresolved target, ODR, and variant
divergence remain distinct. Name/signature similarity alone never creates an edge.

`call_graph` has symbol and call-site nodes. Direct/static, possible dynamic, indirect/dependent,
implicit constructor/destructor, modeled, and unknown calls use distinct edge kind/property rows.
Multiple possible targets are parallel candidate edges with their own evidence and guarantee.
Recursion is an SCC, and open-world dispatch cannot claim exact closed-world completeness.

`include_graph` retains include spelling, resolved file identity, quote/angle/import kind,
conditional/macro/system/generated flags, source order, and variant provenance. Missing includes use
unresolved targets. Same target through different spelling or source occurrence remains distinct.

## Traversal, impact, coverage, and budgets

All graph operations use one shared adjacency/closure/SCC/path engine. `graph_options` fixes scope,
direction, inclusion of unresolved rows, execution context, and four positive finite limits:
depth, nodes, edges, and paths. Zero never means unbounded. Traversal is deterministic breadth-first
within canonical adjacency order; SCCs are condensed for convergence but authoritative nodes/edges
remain uncollapsed in results.

`impact_query` is an immutable builder seeded by typed symbol IDs or one normalized semantic
selector. Relation allowlist, direction, path policy, scope, and limits are explicit. Convenience
builders (`callers`, `derived_types`, `reverse_includes`, `tests`) only desugar to the same typed
relation/direction contract. `shortest` returns all equally shortest paths within limits;
`all_bounded` returns every canonical bounded simple path; `representative` returns the first
canonical path per reached node/SCC. Reachability and causal path rows remain separate.

Complete zero reachability is an empty graph with complete coverage and an exact/conservative
guarantee. Missing facts/capabilities, unsupported relations, unknown endpoints, excluded variants,
budget truncation, provider failure, and non-convergence are explicit coverage/unresolved states.
Valid rows survive partial results and the guarantee is downgraded. Unsupported schema/kind or
invalid/non-positive limits are structured errors and cannot become empty success.

## Projection contract

`subgraph` validates roots against the source snapshot, retains reached nodes/edges, adds typed
boundary nodes for excluded outgoing relations, and conservatively inherits coverage/unresolved and
guarantee. Dangling edges are invalid. JSON is canonical `cxxlens.graph.v1`. DOT is presentation
only: stable node names derive from graph IDs; labels are escaped; nodes, edges, and attributes use
canonical order; absolute paths/source text/native addresses are excluded. Neither projection may
requery, infer edges, hide partial state, or upgrade a guarantee. Output budget exhaustion is typed
truncation, never silent omission.

## Shared ownership and technical spike boundary

- `graph` owns graph IDs/value types, adjacency/closure/SCC/path algorithms, budget accounting,
  graph failure/schema registries, and JSON/DOT projection.
- `facts/workspace` (#44) own detached facts, provisioning, variants, catalog/fact snapshots, and
  coverage inputs. `search/select` (#45) own selector normalization and query/virtual-candidate
  semantics; graph consumes them without redefining identity.
- `flow` (#50) may reuse core evidence/coverage/ID primitives but its CFG/data-flow nodes and edges
  are not `semantic_graph` nodes/edges and do not share a registry namespace.
- #52 must spike diamonds/SCC recursion, virtual/open-world multi-target calls, include cycles and
  missing targets, variant divergence, all four budget limits, deterministic jobs/root/cache
  equivalence, JSON schema validation, DOT escaping/order, and provider failure preservation.

## Acceptance summary

Positive cases cover complete hierarchy/override/call/include and bounded impact graphs. Negative
cases cover invalid IDs/limits/schema, conflicting duplicates, dangling edges, and unsupported
relations. Ambiguous cases cover open-world candidates, unresolved endpoints, cycles/SCCs, variant
divergence, partial providers, and every budget truncation. All six API records bind these cases to
exact declarations, owner/provider/schema edges, Doxygen obligations, and #52 spike conditions.

## Issue #52 validation backlink

Validated by `docs/design/high_risk_contract_validation.md#decisions`: a cyclic/open-world graph was bounded
at five materialized nodes with an omitted count and deterministic seven-edge projection. Fingerprint
`sha256:6993968a9393a4f465aa9038b8ff58d236a44f0a5d7d3aa438882fdfa6a400e2` was unchanged.
