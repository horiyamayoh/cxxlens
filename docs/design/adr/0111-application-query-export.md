# ADR 0111: Lossless application query export

Status: Accepted

## Consumer and gap

CXXMONSTER consumes function, direct-call, build, and source relations to produce a
data-only scene package. The application-analysis SDK already publishes immutable
snapshots and executes these queries. Its installed command currently returns row
counts, which cannot drive an independent consumer or preserve row evidence.

## Contract

`cxxlens run --bundle ... --worker ... --trusted-worker-digest ... --query-results`
and `cxxlens-clang22-materialize --query-results` return
`cxxlens.application-query-results.v1`. Each entry contains a relation ID,
the canonical scan Logical Query IR, and the unchanged public
`cxxlens.query-execution-result.v1` for that scan. All scans bind the same immutable
snapshot and publication. Relation entries are sorted and unique.

Coverage, closure IDs, unresolved items, conflicts, differential disagreements,
producer contracts, contributor edges, guarantees, provenance, explanations,
execution status, and partiality remain in their owning query result. The export
does not combine independent scans into a join or certify completeness. An empty
query retains its coverage and closure disposition; a partial query remains
partial. Thus witnesses can be proved, absence requires applicable complete
closure, and unknown/partial/conflicting results remain distinguishable.

The schema is the process contract authority. Existing SDK query and Store
contracts remain the semantic authorities. External JSON cannot grant provider
trust or Store write authority. This CH-1 Experimental command writes stdout
only after constructing a complete bounded response. It does not write a new
Store, certificate, qualification report, or compatibility shim. The existing
command output without `--query-results` is unchanged.

## Completion and falsification

The immediate dependency is a lossless scan export, followed by CXXMONSTER's
bounded reader and real compiler-to-scene integration. Unsupported or missing
compiler inputs must keep their actionable completion reasons; export never
repairs them. Tests compare each nested result and IR against direct public SDK
execution, check input ordering, duplicate and absent relations, and reject an
output exceeding its finite byte budget without emitting a partial document.

The installed provider is selected by its configured executable and compatible
protocol. Neither this export nor materialization requires a quality certificate,
qualification registry, or release test report. Analysis fidelity is described
by the query's actual coverage and unresolved results.
