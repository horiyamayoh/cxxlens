# Parallel-wave readiness audit

> [!CAUTION]
> 旧124 API Phase C authorizationはIssue #57により失効した。checked-in auditは移行baselineの
> fail-closed証拠としてのみ保持し、新規atomic unit dispatchには使用しない。

The final audit independently regenerates and validates the catalog task packets, ownership map,
dependency DAG, ready predicate, completion manifests, prompt resolutions, and CI shard handoffs.
The authoritative catalog currently contains 22 packages and 124 API entries; older issue prose that
mentions 123 entries is not an authorization input.

```sh
python3 tools/agent/readiness_audit.py check --root .
```

The canonical authorization is `schemas/cxxlens.readiness.authorization.v1.json`. It enumerates each
API and atomic unit exactly once. The checked-in form is intentionally unbound and denied: command
declarations and required-status strings are not execution evidence. A dispatchable runtime
authorization additionally binds the decision to an exact clean commit SHA and tree SHA, the
catalog, packet, ownership, dependency, foundation, design-package and workflow fingerprints, and a
verified observed gate-evidence bundle.

Use one of the two evidence providers from a clean checkout:

```sh
# Execute every canonical global and M0/M1/M2 command locally.
python3 tools/agent/readiness_audit.py replay --root .

# Observe one explicit GitHub Actions workflow run for the current HEAD.
python3 tools/agent/readiness_audit.py online --root . \
  --repository horiyamayoh/cxxlens --workflow-run-id RUN_ID
```

The commands write ignored evidence and authorization artifacts under `build/readiness/`. `verify`
accepts an existing pair through `--evidence` and `--output`. Every observation contains provider and
job identity, source/tree SHA, conclusion or exit status, start/end time, command/environment/toolchain
fingerprints, and log/provider/record digests. Missing jobs, non-success conclusions, a different SHA,
dirty source, workflow or command drift, and stale record digests all fail closed. Multiple GitHub
workflow runs for the same SHA are ambiguous unless `--workflow-run-id` selects one explicitly.

The dependency fingerprint includes typed shared package contracts, provider ownership and
semantics versions, edge provenance, blocking chains, and package-integration prerequisites. A
provider declaration without a unique owner, an incomplete required semantics version, or an
undeclared shared component denies readiness even when the provider is otherwise marked available.
The API-ready report records global gates as `required`; node prerequisites separately record that
their declarations exist and that no execution has yet been observed. A candidate `ready` state is
therefore never equivalent to dispatch authorization.

## Current decision

The current manifest denies a new parallel implementation wave: 47 units are already complete, 77
are blocked with concrete steward and issue references, and no incomplete unit is ready. This is a
successful fail-closed audit, not a foundation failure. Agents must not dispatch complete or blocked
units and must never infer readiness from contract maturity.

## Operator checklist

1. Start from a clean checkout of the exact revision to authorize.
2. Run `replay` with Clang 22, or run `online` after every required GitHub job has completed.
3. Verify that gate evidence has `status: success`; a plain `check` remains denied without evidence.
4. Run `cxxlens-readiness-audit-check` and the normal `cxxlens-quality` target.
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
