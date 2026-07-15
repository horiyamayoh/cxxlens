---
cxxlens_document_status: archived
cxxlens_authority: non-normative
cxxlens_replacement: docs/design/cxxlens_next_generation_integrated_design_ja.md
cxxlens_removal_issue: "#72"
---
# Ready DAG and API task runner

> [!CAUTION]
> 旧atomic API runnerのdispatch authorityはIssue #57により失効した。本書は移行監査用provenanceであり、
> 新規実装を認可しない。

The ready evaluator combines the task-packet corpus, ownership manifest, foundation completion
manifests, fact/capability providers, and dependency requests. Its DAG nodes are atomic
implementation units. Edges cover API dependencies, package engines, shared public types,
schema/fixture contracts, and provider implementations; package-integration prerequisites are
recorded as a separate edge scope. Every edge carries a reason, owner/steward, source contract, and
required semantics version when applicable. Contract maturity is never a readiness input by itself.

```sh
python3 tools/agent/ready_evaluator.py check --root .
python3 tools/agent/ready_evaluator.py resolve --root . \
  --prompt 'API-CORE-005 を実装してください'
python3 tools/agent/api_task_runner.py run --root . \
  --api-id API-CORE-005 --execute
```

Resolution always returns the exact packet/unit, allowed write prefixes, evidence paths, shard, and
single acceptance command. The canonical `run` and `integrate` commands contain `--execute`;
removing it is rejected by both schema validation and the runner instead of silently printing a
resolution. A blocked unit cannot execute. A `ready` unit may start implementation, while a
`complete` unit may only replay its acceptance shard for integration evidence.

Every shard has an ordered execution plan: shared task/ownership/configure/build setup, three
unit-local steps, then the full test and quality gates. Each positive, negative, and ambiguous step
resolves catalog evidence to exact CTest names through `unit_local_gate.py`; the report check rejects
a complete/ready unit whose target does not resolve. The result artifact under
`build/dev-clang/agent-results/` records the input commit SHA, command argv/environment, scope,
fixture category, evidence paths, exit status, and stdout/stderr digests for every executed step.

Package integration remains a separate role. It is blocked while any package unit is incomplete,
accepts only paths owned by that package integration role, and verifies that every conformant input
has a successful unit artifact for the same commit SHA and exact shard digest before executing its
global gates.

A provider being named or globally available is insufficient: its owner unit must have completed
the required semantics version. Blockers include the component/provider and recursively visible
owner chain. Topological waves therefore place the package contract owner before leaf API units,
and the report digest binds dispatch authorization to the full typed graph.

```sh
python3 tools/agent/api_task_runner.py integrate --root . \
  --package-id configuration --execute
```
