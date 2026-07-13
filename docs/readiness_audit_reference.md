# Parallel-wave readiness audit

The final audit independently regenerates and validates the catalog task packets, ownership map,
dependency DAG, ready predicate, completion manifests, prompt resolutions, and CI shard handoffs.
The authoritative catalog currently contains 22 packages and 124 API entries; older issue prose that
mentions 123 entries is not an authorization input.

```sh
python3 tools/agent/readiness_audit.py check --root .
```

The canonical authorization is `schemas/cxxlens.readiness.authorization.v1.json`. It enumerates each
API and atomic unit exactly once and binds the decision to catalog, packet, ownership, dependency,
foundation, design-package, and CI-workflow fingerprints. Any relevant input drift makes the checked
artifact stale and therefore expires authorization automatically.

The dependency fingerprint includes typed shared package contracts, provider ownership and
semantics versions, edge provenance, blocking chains, and package-integration prerequisites. A
provider declaration without a unique owner, an incomplete required semantics version, or an
undeclared shared component denies readiness even when the provider is otherwise marked available.

## Current decision

The current manifest denies a new parallel implementation wave: 47 units are already complete, 77
are blocked with concrete steward and issue references, and no incomplete unit is ready. This is a
successful fail-closed audit, not a foundation failure. Agents must not dispatch complete or blocked
units and must never infer readiness from contract maturity.

## Operator checklist

1. Start from a clean checkout of the exact revision to authorize.
2. Run the M0, M1, and M2 replay commands listed in the manifest with Clang 22.
3. Run `cxxlens-readiness-audit-check` and the normal `cxxlens-quality` target.
4. Require the GitHub `Quality gates` jobs named by the manifest to report success.
5. Dispatch only `authorization.ready_unit_ids`, in the listed topological waves.
6. Use the exact task packet, owner prefixes, and acceptance command resolved by the API runner.
7. Re-run the audit after resolving a dependency request; do not edit blocker state by hand.

Canonical acceptance commands are executable, not resolution previews. Keep `--execute`, retain the
unit-local positive/negative/ambiguous stages, and preserve the resulting commit-bound artifact.
Package integration must consume matching successful unit artifacts; a declaration-only shard or a
result from another unit/SHA is not conformant input.

Immediately stop and re-audit on catalog/signature, schema/task-packet/shard, ownership/shared
contract, dependency/provider, completion-manifest, design-package, or required CI workflow drift.
Previously issued authorization is not reusable after any such change.
