# Issue #44 workspace・facts・interop package Contract Candidate

## Authority and scope

This document and `schemas/cxxlens_package_contract_candidates.yaml#issue-44` are the normative
Phase B candidate for all 22 catalog entries owned by `workspace` (8), `facts` (10), and `interop`
(4). The common rules in `cxxlens_global_contract_conventions.yaml` remain authoritative. The
candidate does not install headers, add production implementations, freeze the API, or change the
20 conformant / 2 unimplemented implementation states. Issues #53 and #54 own those later steps.

## Workspace catalog, scope, snapshot, and provisioning

| Decision | Candidate rule |
|---|---|
| Root identity | Compilation database path and working directories are normalized through the filesystem port. Lexical aliases/symlink escape reject; ambient current directory never changes identity. |
| Compile commands | All relevant commands are retained as distinct build variants. Missing is empty success only for `command_for`; ambiguity is a multi-row value, never first-wins. |
| Canonical order | Compile units use full `compile_unit_id`; commands use normalized file, variant, directory, argv, and full ID. Filesystem/hash/arrival order is excluded. |
| Scope | `all/files/compile_units/changed_files` form an explicit universe. Header and variant filters are immutable; duplicate identities collapse and conflicting aliases reject. |
| Snapshot | `facts()` returns a handle to one immutable committed snapshot. A concurrent `ensure()` may publish a newer snapshot but cannot mutate a reader's snapshot. |
| Provisioning | `ensure()` builds only missing `(fact kind, scope unit, variant, precision)` coverage, honors cancellation/deadline/budgets, and commits staged rows transactionally. |
| Partial result | Valid rows and exact failed/unresolved coverage survive a frontend failure. No failed variant is omitted and no confidence value upgrades precision or guarantee. |
| Doctor | Side-effect-free structured diagnostics report exact catalog/capability/resource state; prose is never control input. |

## Fact profile, immutable query snapshot, and coverage

`fact_profile` expands to sorted concrete kinds and an exact precision level. `full()` excludes
`custom`; a custom kind is selected only after exact extractor/schema registration. Unknown kinds,
filters, schema versions, and precision levels reject without nearest-version fallback.

Every `fact_store` query evaluates one immutable snapshot. A complete zero-match query returns an
empty vector; incomplete prerequisites return a structured error or an incomplete coverage report
and cannot be represented by the same empty vector. Typed accessors are projections of the same
generic fact rows and preserve IDs, provenance, variants, evidence, and canonical order. Same-ID
same-payload duplicates collapse; same-ID conflicting payloads fail the snapshot transaction.

Coverage uses exact accounting over requested scope × variants × fact kinds × precision:
`requested = covered + excluded + failed + unresolved + not_applicable`. The report is complete
only when no requested unit is failed/unresolved and achieved precision satisfies requested
precision. An empty request may be completely empty; an unsupported request may not.

## Borrowed Clang corridor

`borrowed_clang_tu`, every Clang reference returned from it, and `fact_sink` are valid only during
the synchronous callback on its owning thread. Copy, retention, coroutine suspension, cross-thread
transfer, and persistence of `Decl*`, `Stmt*`, `Type*`, `ASTContext`, `SourceManager`, token/location
handles, or addresses are forbidden. Cancellation/deadline inside a user callback is cooperative;
bounded workspace parsing remains isolated by the workspace scheduler.

The raw corridor is explicitly LLVM-major-scoped. `linked_clang_version()` reports linked reality;
absence or a mismatched major is a typed capability failure, never an adjacent-major fallback.

## Custom fact and extractor state machine

`interop::custom_fact` owns only detached namespace/schema/version/semantic-key/payload/source
values. `fact_sink` supplies the current extractor identity, compile unit, variant, and job
provenance. `cxxlens_custom_fact.schema.yaml` recursively rejects native pointer/address/AST-shaped
payloads and requires exact schema identity.

| State | Allowed transition | Invariant / failure |
|---|---|---|
| unregistered | `register_extractor` → registered | Non-null shared owner, valid reverse-domain ID/version, and unique extractor/namespace/schema ownership are required atomically. |
| registered | operation acquisition → executing; `unregister_extractor` → removed | Registration order is not execution order; canonical extractor ID/version order is used. |
| executing | success/partial/failure → registered | Callback is synchronous and thread-confined. Each extractor has an isolated staged batch. |
| executing | unregister request | Fails as busy; it never races by destroying an active owner. |
| removed | any token reuse | Fails as stale. Tokens are workspace-scoped and cannot be used in another workspace. |

An extractor failure discards only that extractor's staged rows, records typed failed/unresolved
coverage and evidence, and allows independent extractors to finish. The operation's requested
guarantee decides whether the aggregate result is partial or failed. Duplicate namespace/schema
owners, nondeterministic repeated output, conflicting fact IDs, unknown schema versions, and schema
violations are structured errors; first/last-wins and prose matching are forbidden.

## Registry and DAG ownership

- `workspace` owns catalog, scope, provisioning trace, doctor, and workspace error/schema registries.
- `facts` owns fact/profile/query/snapshot/coverage semantics and built-in fact/schema registries.
- `interop` owns the raw Clang capability corridor and custom extractor/schema registration state.
- Downstream packages consume immutable facts/workspace values and cannot redefine identity,
  coverage, variants, custom-schema ownership, or borrowed lifetime.
- Provider order and consumer scheduling come from explicit API/fact/capability edges. No shell
  command concatenation, unordered iteration, silent command/variant fallback, or AST pointer edge
  is permitted.

## API disposition and acceptance evidence

| Package | API IDs | Declaration | Implementation after #44 |
|---|---|---|---|
| workspace | API-WS-001..008 | exact existing public headers | 8 conformant |
| facts | API-FACT-001..010 | exact existing public headers | 10 conformant |
| interop | API-INT-001..002 | exact existing raw header | 2 conformant |
| interop | API-INT-003..004 | exact non-installed candidate fragment | 2 unimplemented / blocked until #53 integration |

Each manifest API row binds its exact signature fingerprint, atomic unit, owner/provider/schema
references, dependency APIs, Doxygen obligation, and positive/negative/ambiguous acceptance case.
The package candidate validator rejects missing API rows, signature drift, duplicate registry
owners, dangling ownership/dependency/trace references, public-header pre-emption, and fingerprint
drift. C++23 syntax checks compile the candidate usage without linking a production implementation.
