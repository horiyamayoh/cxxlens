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

Propose adoption of `schemas/cxxlens_ng_provider_ng1_hardening.yaml` as the
exact NG1 hardening contract. NG1 remains opt-in at protocol minor 1 and preserves all
NG0 wire, credit, batch, coverage, unresolved, and atomicity invariants.

The contract defines:

- typed heartbeat probe/ack controls bound to provider, session, task, stream,
  monotonic sequence, host receipt time, and an injected host monotonic clock;
- a closed lifecycle receipt contract whose session-start, task-start, and
  terminal events are host-observed before shared validation; before the first
  valid acknowledgement the task-start grace deadline is the only liveness
  deadline, and after that acknowledgement probe deadlines and idle timeout
  use host receipt deltas;
- deterministic liveness interval/deadline and overflow-safe progress-rate
  arithmetic using host receipt deltas; an acknowledgement is rejected at or
  beyond the latest probe receipt plus the inclusive timeout, and a first
  progress sample is required strictly before the task-start grace deadline;
  provider timestamps are only ordered within their bound scope, with host
  receipt time remaining the rate/liveness authority;
- durable resume tokens bound to provider binary/semantic identity, task,
  input/selection identity, dependency group, batch, stream, acknowledged
  sequence, staged digest, and a host-observed fsync receipt defined by the
  closed `schemas/cxxlens_ng_provider_spill_fsync_receipt.schema.yaml`;
- append-only bounded spill records with deterministic framing, canonical
  digests, checked quota arithmetic, fsync-before-token, contiguous replay,
  fail-closed corruption handling, and explicit cleanup;
- an explicit worker crash/hang/cancel transition matrix whose shared sealed
  output is the only adoption input and never alters the previously published
  snapshot on failure;
- exact static/shared and long-run qualification cases plus declarative
  positive/negative vectors plus the closed
  `schemas/cxxlens_ng_provider_ng1_qualification_report.schema.yaml`, whose
  measured report binds one revision, tree, provider identity, protocol minor,
  hardening-contract digest, report-schema digest, and vector digest. The
  checked-in vectors are explicitly authority-only and provider-unbound.

Heartbeat and progress controls are transport occurrences and are not claim or
partition authority. Only the shared typed validator's sealed output groups
may be adopted. A provider self-claim, non-durable acknowledgement, adjacent
provider, or unavailable platform cannot satisfy NG1 qualification.

## Consequences

The process port must expose a deterministic clock, bounded spill/recovery
operations, and host-observed durability receipts to the NG1 session state
machine. The existing completed-process
`run()` path remains valid for NG0 and cannot be silently reclassified as NG1.
The public protocol enum may expose message type 23 only together with the
typed controls, feature negotiation, registered failure reservations, executed
vectors, and catalog/acceptance traceability. Until implementation and
qualification are complete, those NG1 failures are reserved in the runtime
authority and are not active C++ terminals.

Failure is fail-closed: heartbeat clock drift, liveness timeout, rate failure,
stale/foreign/mutated resume, spill corruption, or unknown cleanup effect
produces a stable failure and leaves the prior published snapshot unchanged.

While this ADR is Proposed, NG1 failure reservations remain outside the active
execution-report terminal enum and outside the C++ runtime terminal registry;
the host-observed receipt schema and the conformance vectors are authority
inputs, not production qualification evidence.

## Alternatives rejected

- Treating named NG1 features as sufficient without typed semantics.
- Using wall-clock time or provider-reported progress as host authority.
- Comparing provider timestamps with host receipt time unless the negotiated
  session-host-monotonic clock domain is established; timestamp ordering never
  replaces host receipt authority.
- Keeping all output resident and calling it spill staging.
- Resuming from an un-fsynced acknowledgement or using an ambient path/FD.
- Falling back to NG0 or an adjacent provider when NG1 is unavailable.

## Acceptance gate

This ADR remains Proposed until #233 records an independent review of the
closed schema and checker, protocol/report bindings, state-machine transitions,
clock/rate overflow and replay boundaries, spill durability/cleanup, the
semantic-v2 receipt digest boundary, and exact negative qualification matrix.
Only after that review may #183 implement NG1; only after executed
static/shared evidence with the exact report binding may the contract maturity
change or NG1 be advertised.
