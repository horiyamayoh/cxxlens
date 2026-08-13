---
id: DF-0233
title: Define NG1 provider hardening lifecycle and evidence authority
status: proposed
kind: contract-contradiction
impact: contract
confidence: high
implementation_disposition: blocked
scope:
  - provider.ng1-heartbeat
  - provider.ng1-durable-resume
  - provider.ng1-progress-rate
  - provider.ng1-spill-recovery
  - release.ng1-qualification
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0010-provider-wire-streaming-atomicity.md
  - docs/design/adr/0038-provider-runtime-protocol-state-validation.md
  - schemas/cxxlens_ng_provider_protocol.yaml
  - schemas/cxxlens_ng_provider_runtime_contract.yaml
  - schemas/cxxlens_ng_provider_ng1_conformance_vectors.yaml
  - schemas/cxxlens_ng_security_profile.yaml
  - schemas/cxxlens_ng_release_bundle.yaml
tracking_issue: '#233'
implementation_issues:
  - '#183'
resolution_refs: []
review:
  mode: independent
  status: pending
  author: codex-agent-ng1-authority
  reviewer: null
  refs: []
created: '2026-08-13'
---

# Define NG1 provider hardening lifecycle and evidence authority

## Observation

The accepted Provider Protocol and integrated design require NG1 durable resume,
  heartbeat, progress-rate enforcement, spill staging, hung-worker recovery, and
  long-run qualification. The machine-readable protocol currently names those
  features and reserves message type 23, but it does not define the typed
  heartbeat control fields and direction, host-receipt clock/liveness policy,
  progress-rate arithmetic and boundary behavior, durable token projection,
  spill framing/integrity/cleanup, restart/recovery state transitions, or
  executed negative vectors.

The current C++ runtime stops at message type 22, treats resume as unsupported,
uses one in-memory process output, and has no production NG1 capability or
qualification evidence. The existing NG0 green suite therefore cannot be
promoted to an NG1 claim.

## Working mental model

NG1 is an opt-in provider profile layered on the existing NG0 transcript. The
host and provider share one typed state machine. Heartbeats are liveness
control and are bound to the exact provider/session/task/stream identity;
progress is an authenticated monotonic sample stream evaluated against a
deterministic monotonic clock; resume is permitted only from an fsync-confirmed
spill acknowledgement; and a worker restart replays from the last durable
acknowledgement without adopting an open dependency group.

Spill bytes are transport occurrences, not semantic claim authority. A sealed
validated output group remains the only adoption input. Any heartbeat, rate,
resume, spill, or recovery ambiguity fails closed and leaves the previously
published snapshot unchanged.

## Mismatch or opportunity

Implementing the named features directly would invent public protocol semantics
and could turn a self-reported progress or spill receipt into authorization.
The gap is material to correctness, security, compatibility, and release
qualification, so #183 must remain blocked until the lifecycle contract is
accepted and independently reviewed.

## Evidence

- `include/cxxlens/sdk/provider.hpp` ends the closed message enum at `close=22`.
- `src/sdk/provider_runtime.cpp` rejects `resume` and `close` as unsupported and
  interprets `progress` as one NG0 evidence record set rather than an NG1 rate
  sample stream.
- `src/sdk/provider_runtime_internal.hpp` exposes only a completed-process
  output path; no live heartbeat, durable token, spill, or recovery port exists.
- `schemas/cxxlens_ng_provider_protocol.yaml` now binds the proposed hardening
  contract and typed registry, but it still has no executed NG1 lifecycle.
- `schemas/cxxlens_ng_provider_runtime_contract.yaml` has no NG1 resource,
  clock, spill, or recovery authority.
- Existing protocol/runtime quality tests cover NG0 vectors only; they do not
  prove stale/replay token rejection, liveness timeout, rate enforcement,
  corruption recovery, or long-run evidence.

## Alternatives and trade-offs

1. Define a versioned private NG1 hardening contract and add a shared typed
   state machine, deterministic clock/spill ports, and exact positive/negative
   vectors before exposing an opt-in capability. This preserves NG0 behavior
   and keeps raw spill/transcript data non-authoritative.
2. Treat the existing message names and generic evidence as sufficient. This
   is rejected because direction, replay, timeout, and corruption semantics
   remain ambiguous and could produce false production claims.
3. Advertise NG1 from manifest feature names alone. This is rejected because
   provider self-claims are not certification authority.

## Recommendation

Accept one exact NG1 contract covering typed heartbeat probe/ack, host-receipt
clock/deadline and progress-rate rules, token bindings with durability receipt,
append-only bounded spill framing with checked quota/integrity and cleanup,
restart/replay state, stable failure reservations, and a measured qualification
tuple plus vectors. Then implement the shared state machine, connect the live
process port, execute the vector matrix, and bind the static/shared certificate
before adding any certification claim.

## Disposition

2026-08-13: Proposed and blocked. #183 remains open. The authority amendment,
independent review, and exact implementation/qualification evidence are
required before this record can become accepted or #183 can close.
