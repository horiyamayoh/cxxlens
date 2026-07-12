# Ready DAG and API task runner

The ready evaluator combines the task-packet corpus, ownership manifest, foundation completion
manifests, fact/capability providers, and dependency requests. Its DAG nodes are atomic
implementation units. Contract maturity is never a readiness input by itself.

```sh
python3 tools/agent/ready_evaluator.py check --root .
python3 tools/agent/ready_evaluator.py resolve --root . \
  --prompt 'API-CORE-005 を実装してください'
python3 tools/agent/api_task_runner.py run --root . --api-id API-CORE-005
```

Resolution always returns the exact packet/unit, allowed write prefixes, evidence paths, shard, and
single acceptance command. A complete or blocked unit has `start_authorized=false`; `run` refuses it
before creating an implementation workspace or executing commands. Only a `ready` unit may pass
ownership preflight and execute its argv-based shard.

Every shard includes task-packet and ownership drift checks, configure/build/test, and the complete
quality target. Positive, negative, and ambiguous fixture categories are mandatory. Package
integration remains a separate role and receives only conformant unit/shard digests. Its generated
shard is blocked while any package unit is incomplete, and its preflight accepts only paths owned by
that package integration role.

```sh
python3 tools/agent/api_task_runner.py integrate --root . --package-id configuration
```
