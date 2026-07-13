# Public contract conventions

This document is the normative Phase B convention shared by every public cxxlens API. The
machine-readable authority is
`schemas/cxxlens_global_contract_conventions.yaml`; this prose explains the rules but cannot weaken
the manifest, catalog, or integrated-design invariants. Package candidates use
`docs/design/contract_candidate_checklist.md` and remain non-production design artifacts.

## Naming and family shape

Public namespace, type, function, member, and enum names use lower snake case. A coherent factory,
builder, overload, or method family is one indivisible atomic unit and records every member in one
exact declaration fingerprint. Constructors establish unconditional invariants; fallible or
policy-bearing construction uses a named factory. Boolean switches and unstable default-argument
sets use an options value. An overload cannot silently change ownership, failure, or coverage
semantics.

`[[nodiscard]]` is required when discarding a result loses failure, plan, report, or evidence data.
Single-argument semantic conversions are `explicit`. Each candidate records default/copy/move
support and why a range, span, view, tag type, free function, or member is appropriate. No ABI
stability is implied unless a contract explicitly names it.

## Values, ownership, and lifetime

Values are the default public boundary. Nullable relationships use explicit nullable handles or
value types; raw owning pointers are forbidden. Every value, reference, pointer, view, and callback
argument states one of owned, shared, borrowed, callback-scoped, or immutable-view lifetime. A
returned span, range, or string view names its backing owner and all invalidation events.

Snapshots, builders, plans, reports, and registries declare value semantics. AST pointers never
outlive a frontend job and are never stored or moved across threads. Stable headers expose no
`clang::*`, `llvm::*`, or LLVM/Clang header. Raw interop is an explicit opt-in through
`<cxxlens/interop/clang.hpp>`.

## Const, noexcept, threads, and callbacks

Logical constness permits a cache only when observable semantics are unchanged and synchronization
is documented. Every type and callable is classified for concurrent reads, concurrent writes, and
thread affinity. `noexcept` is used only when every reachable failure is represented as stable data.

A callback contract states invocation thread, order, reentrancy, exception boundary, cancellation
points, borrowed-argument lifetime, and whether the callback may retain any value. Callback
exceptions never cross an unspecified boundary. Valid rows produced before cancellation, deadline,
or budget exhaustion are retained only when their authority can still be established.

## Execution context

Operations that can block, traverse, invoke tools, or enumerate candidates accept an execution
context or an equivalent explicitly owned policy. The common vocabulary is cancellation, deadline,
time budget, memory budget, path/depth budget, state/candidate budget, and output budget. Contracts
state preflight, in-flight, and post-completion observation points.

Timeout, caller cancellation, worker crash, and unsupported capability are stable failure data.
Cancelling one caller does not cancel coalesced work still needed by another live caller. A budget
limit cannot turn omitted work into empty success.

## Result and failure decision table

`result<T>` and `result<void>` form the public operation-failure boundary. Exceptions are contained
at adapters and callbacks and translated to stable errors. The six outcomes below are mutually
exclusive and are encoded exactly in the convention manifest.

| Outcome | Channel | Payload | Error | Unresolved | Complete coverage | Preserves authoritative rows |
|---|---|---|---:|---:|---:|---:|
| empty success | value | empty | no | no | yes | yes |
| unresolved | value | partial | no | yes | no | yes |
| unsupported | error | none | yes | no | no | no |
| ambiguous | value | partial | no | yes | no | yes |
| partial | value | partial | no | yes | no | yes |
| failure | error | none | yes | no | no | no |

An operation error says the operation could not produce an authoritative value. A diagnostic
describes execution or input. A finding is a domain assertion with stable identity and evidence.
An unresolved row accounts for requested work whose semantic answer is not established. These
roles are not interchangeable. Stable errors record code, failure scope, retryability, nested
cause, and bounded structured attributes; prose is not control flow.

Empty success means the requested surface was fully and successfully inspected and contained no
rows. Unsupported means the requested semantic operation is unavailable. Ambiguous means multiple
non-equivalent candidates remain. Partial means some authoritative rows exist but the request is
not fully accounted for. Partial values retain evidence, coverage, guarantee, and every unresolved
or failed unit.

## Evidence, coverage, confidence, and guarantee

Structured evidence is authoritative and prose is a projection. For every request, the requested
set is conserved exactly once across covered, excluded, failed, unresolved, and not-applicable.
Exclusion is caller policy, not silent omission. Coverage is complete only when no requested unit
is failed or unresolved and every exclusion is explicit.

Confidence does not upgrade precision, coverage, or guarantee. Unsupported surface, cancellation,
budget exhaustion, variant divergence, or provider failure downgrades the guarantee. Representative
examples are bounded and never replace complete accounting.

## Determinism, duplicates, and variants

Every serialized collection, identity input, and externally visible enumeration has a canonical
total order and a final stable tie-breaker. Input arrival, filesystem iteration, thread completion,
hash seed, backend selection, and checkout root are forbidden ordering sources.

Each row family chooses exactly one duplicate policy: identity collapse, semantic merge, preserve,
or reject. Merge retains provenance and rejects incompatible semantics. Variant records are
classified as equal, divergent, or conflicting; divergent and conflicting records are never
silently first-wins.

## Serialization and versioning

Canonical JSON, a registered schema identifier, and a schema version are mandatory for public
serialization. Unknown fields are accepted only when the declaring schema explicitly permits them;
unknown enum/kind values are rejected unless an explicit extension rule exists. Round trips preserve
all authoritative known data.

C++ API, public schema, fact schema, semantic contract, and adapter versions are independent axes.
Compatibility produces one of compatible, migrate, rebuild, or reject. Stable error, capability,
fact, and schema registries use deterministic ordering and never recycle a meaning under an old ID.

## Schema ownership

`schemas/cxxlens_contract_ownership.yaml` assigns exactly one owner to every shared public type,
package component, provider subject, and checked-in `*.schema.yaml`. Schema registry mechanics are
owned by `AU-CORE-001` / `steward.schema`; package contracts remain responsible for their domain
semantics. A consumer records a dependency edge and never becomes a second owner. Missing,
duplicate, dangling, or non-canonical owner rows fail the global convention checker.

## Public contract changes

Changes are additive, source-breaking, semantic-breaking, or schema-breaking. Every class changes
the contract input fingerprint. Breaking classes require a major version and migration. Deprecation
requires a documented replacement, migration, and removal boundary. Implementation details may
change only when signature, meaning, evidence, coverage, determinism, failure, and version promises
remain unchanged.

Changing a frozen contract requires #54 approval, reissued manifests, and invalidates Phase C
authorization. Signature, semantics, ownership, dependency, schema, decision-table, or evidence
drift expires authorization automatically.

## Ownership and state transition

Package contract state is independent of declaration status, implementation state, and readiness.
The states are `draft`, `unresolved`, `candidate`, and `frozen`. Package owner issues #43 through #51
may move draft/unresolved contracts to candidate only after every checklist field is complete.
Only #54 may move candidate to frozen. A package issue that records itself as the transition issue
for `frozen` is rejected.

An exact declaration or an implementation marked conformant does not imply a frozen public
contract. Conversely, a candidate/frozen contract does not imply any production implementation.
The existing agent ownership "skeleton" binds declaration text to a source digest; its lock state is
not the Phase B public `contract.state`.

## Propagation and validation

The catalog points to the conventions and ownership registry. Every task packet inherits package
contract state, candidate owner, transition issue, conventions fingerprint, and ownership-registry
fingerprint. Ownership and ready reports carry the same state; readiness authorization fingerprints
both manifests, so drift expires it. Candidate validation requires positive, negative, and ambiguous
fixtures and cannot use production implementation as Phase B evidence.
