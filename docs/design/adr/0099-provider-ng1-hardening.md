# ADR 0099: Provider NG1 hardening lifecycle and evidence

- Status: Proposed
- Date: 2026-08-13
- Decision owner: steward.ng-provider-runtime
- Decision issue: #233
- Implementation issue: #183
- Amends: ADR 0010, ADR 0038

## Context

The integrated design makes NG1 provider hardening a distribution 1.0
requirement and names durable resume, heartbeat, progress-rate enforcement,
spill staging, hung-worker recovery, and long-run qualification. The accepted
Provider Protocol reserves message type 23 and lists the feature names, but
does not define the typed lifecycle semantics needed to implement or certify
them. The existing runtime is NG0-oriented and must not infer NG1 capability
from a manifest or from NG0 tests.

## Decision proposal

Adopt `schemas/cxxlens_ng_provider_ng1_hardening.yaml` as the exact NG1
hardening contract. NG1 remains opt-in at protocol minor 1 and preserves all
NG0 wire, credit, batch, coverage, unresolved, and atomicity invariants.

The contract defines:

- typed heartbeat probe/ack controls bound to provider, session, task, stream,
  monotonic sequence, and injected monotonic time;
- deterministic liveness interval/deadline and progress-rate arithmetic;
- durable resume tokens bound to provider binary/semantic identity, task,
  dependency group, batch, stream, acknowledged sequence, and staged digest;
- append-only bounded spill records with canonical digests, fsync-before-token,
  contiguous replay, fail-closed corruption handling, and explicit cleanup;
- worker crash/hang/cancel recovery states that never adopt an open group or
  alter the previously published snapshot;
- exact static/shared and long-run qualification cases bound to one revision,
  tree, provider identity, protocol minor, and hardening-contract digest.

Heartbeat and progress controls are transport occurrences and are not claim or
partition authority. Only the shared typed validator's sealed output groups
may be adopted. A provider self-claim, non-durable acknowledgement, adjacent
provider, or unavailable platform cannot satisfy NG1 qualification.

## Consequences

The process port must expose a deterministic clock and bounded spill/recovery
operations to the NG1 session state machine. The existing completed-process
`run()` path remains valid for NG0 and cannot be silently reclassified as NG1.
The public protocol enum may expose message type 23 only together with the
typed controls, feature negotiation, negative vectors, and catalog/acceptance
traceability.

Failure is fail-closed: heartbeat clock drift, liveness timeout, rate failure,
stale/foreign/mutated resume, spill corruption, or unknown cleanup effect
produces a stable failure and leaves the prior published snapshot unchanged.

## Alternatives rejected

- Treating named NG1 features as sufficient without typed semantics.
- Using wall-clock time or provider-reported progress as host authority.
- Keeping all output resident and calling it spill staging.
- Resuming from an un-fsynced acknowledgement or using an ambient path/FD.
- Falling back to NG0 or an adjacent provider when NG1 is unavailable.

## Acceptance gate

This ADR remains Proposed until #233 records an independent review of the
schema, checker, state-machine transitions, overflow and replay boundaries,
and exact negative qualification matrix. Only then may #183 implement or
advertise NG1.
