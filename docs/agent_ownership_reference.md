# Agent ownership and dependency requests

`schemas/cxxlens.agent-ownership.v1.json` is the machine-readable change-control contract consumed
after task-packet generation. It gives every tracked repository path one exclusive shared steward,
generator, or package-integration role. Each atomic implementation unit receives three disjoint
write prefixes for implementation, unit tests, and semantic fixtures. All existing shared files are
read-only to implementation units.

Exact task-packet declarations produce frozen skeleton records. Unresolved declarations remain
blocked and cannot acquire a usable skeleton from contract maturity alone. Signature, declaration
source, task-packet, or repository-path drift makes the manifest stale.

## Commands

```sh
python3 tools/agent/ownership_generator.py generate --root .
python3 tools/agent/ownership_generator.py check --root .
python3 tools/agent/ownership_generator.py preflight --root . \
  --requester AU-CORE-005 \
  --changed-path src/core/agent_units/au-core-005/finding.cpp
```

Preflight and post-diff audit use the same validator. An implementation unit may write only below
its listed prefixes. A shared or generated path is rejected before compilation with the path,
exclusive owner or generator, requesting unit, and the dependency-request alternative. Package
aggregators and global build/install files are writable only by their integration roles.

## Dependency request protocol

`schemas/cxxlens.dependency-request.v1.schema.yaml` defines the only supported escalation path for
a missing shared contract, fact, capability, port, query primitive, schema, or fixture. A request
names the blocked atomic unit and APIs, target steward, structured evidence, requested behavior,
and acceptance criteria. State transitions are `pending -> accepted -> resolved` or rejection.
Resolution produces a deterministic reissue fingerprint tied to the affected frozen skeletons and
task-packet corpus; it does not silently edit shared files.
