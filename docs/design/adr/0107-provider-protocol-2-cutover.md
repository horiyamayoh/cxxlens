# ADR 0107: Provider Protocol 2.0 cutover

- Status: Accepted for implementation
- Date: 2026-08-23
- Decision owner: repository owner
- Decision issues: #183, #261
- Supersedes: ADR 0102 for transport versioning and compatibility
- Depends on: ADR 0101, ADR 0099, ADR 0100
- Normative contract: `schemas/cxxlens_ng_provider_protocol_v2.yaml`

## Context

The accepted provider implementation is protocol 1.x, while source-closure transport, task v4,
and the hardened NG1 lifecycle are represented by disconnected proposals and source-private
fixtures. The previous source-closure proposal also treated the bytes of v2.1/v3 implementation
files as authority. That couples product behavior to a particular checkout and makes ordinary
refactoring, formatting, and compiler upgrades appear to be protocol changes.

Protocol 2.0 must make the semantic wire contract authoritative. It must carry source-closure
identity and bounded transfer explicitly, connect NG1 liveness/recovery to the same session state
machine, and reject an unsupported or legacy peer before accepting task payload. Product content
digests, provenance, trust, sandbox, VFS, and crash-safety receipts remain required; implementation
SHA ledgers and development qualification records do not.

## Decision

Adopt `cxxlens.provider-protocol.v2`, major 2 minor 0, as the sole implementation target for the
next cutover. The normative message registry, capability rules, task/request authority, closure
transport, bounds, NG1 rules, and product boundary are in the schema named above.

Protocol 1.x is not a compatibility target for the cutover. A peer advertising another major,
request 2.1/task v3, or a missing required capability is rejected before payload or ambient file
access. No implicit downgrade, first-wins negotiation, legacy shim, or v1 byte-preservation test
is permitted. The final integration owner may change the existing public provider headers and
runtime only after this contract handoff; this commit intentionally does not change those shared
hot files.

### Wire and identity

The existing 104-byte framed header, independent SHA-256 frame checksums, strict deterministic
closed-map CBOR, fixed stream, contiguous sequence, and pre-allocation bounds remain the transport
mechanics. The negotiated major is exactly 2 and the negotiated minor is exactly 0. Unknown
required extensions, reserved flags, duplicate keys, non-shortest encodings, invalid UTF-8,
indefinite CBOR, and invalid phase transitions fail closed.

Message IDs 1--22 retain their semantic names. ID 23 is heartbeat only. IDs 24--29 are exclusively
source-closure manifest, blob, chunk, seal, ack, and reject. A heartbeat receipt is not a resume
token, and a closure receipt is not a compiler or publication success.

### Request, task, and closure

Protocol 2.0 requires request 2.2 and task v4 for closure-bearing work. Source bytes and
`content_base64` are not request authority. The task v4 projection binds task input, invocation,
toolchain, environment, closure, manifest, and the canonical main member metadata.

The only closure success path is:

`task-v4-sealed -> manifest-open -> manifest-streaming -> manifest-validated -> blob-open`
`-> blob-streaming -> blob-sealed -> closure-sealed -> closure-acknowledged -> task-accepted`.

Every count, length, offset, digest, and resource sum is checked before allocation or write. The
provider stages only in a private task-local bounded spool, validates the complete manifest and
all blobs, and mounts only the validated closure through the read-only compiler VFS. Ambient CWD,
physical checkout content, and unqualified filesystem paths cannot satisfy a closure member.

### NG1

NG1 heartbeat, progress rate, durable resume, spill, and recovery use the same Protocol 2.0 session
and task identity. Host-injected monotonic receipts are the liveness/rate authority. Resume is
authoritative only after the exact spill frontier is fsynced and bound to the provider, session,
task, invocation, toolchain, environment, sandbox, and output group. Stale, foreign, mutated, or
replayed tokens are rejected. A hung or crashed worker is killed/cleaned as a process group and
cannot publish output.

## Removed authority

The following are explicitly non-authoritative and must be deleted during integration:

- implementation-file SHA and byte-exact legacy bindings;
- `LEGACY_BINDINGS` and source-byte drift checkers;
- lint exclusions justified only by protocol byte preservation;
- qualification JSON, issue execution receipts, checkpoints, and other development completion
  ceremony.

This does not remove product digests, semantic identity, provenance, closure/coverage/unresolved
state, provider binary identity/signature/revocation, sandbox policy, or crash/recovery receipts.

## Non-goals of this contract commit

This contract commit does not alter `include/cxxlens/sdk/provider.hpp`, `src/sdk/provider.cpp`,
`src/sdk/provider_runtime.cpp`, CMake, workflows, public catalog, or installed release behavior.
Those are owned by the later integration wave. It also does not claim #183 or #261 complete until
the live worker/VFS/materializer path and direct positive, negative, fault, determinism, and
resource tests pass.
