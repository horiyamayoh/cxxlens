# ADR 0102: Dedicated source-closure transport

- Status: Proposed
- Date: 2026-08-21
- Decision owner: repository owner
- Decision issue: #261
- Implementation issue: #261
- Design feedback: DF-0261
- Amends: ADR 0096
- Depends on: ADR 0101

## Context

ADR 0101 fixes source-closure identity and the read-only compiler VFS, but intentionally leaves
request/task transport unresolved. Request 2.1 and task v3 carry one main-source input through
`input_descriptor`/`input_chunk`; treating a multi-file closure as that opaque input would hide
manifest, blob, phase, cancellation, and replay semantics from protocol validation. The rejected
PR #353 candidate also mixed obsolete VFS code with a closure-in-task representation and is not an
implementation authority.

Heartbeat already owns message type 23. NG1 durable resume must not allocate the same IDs or treat a
source-closure cache receipt as a resume token.

## Decision

Adopt request 2.2, task v4, Provider Protocol minor 1.2, and required capability
`task-source-closure-v1`. Keep request 2.1/task v3/protocol 1.1 byte semantics unchanged. A task that
requires a closure cannot downgrade: absence of the capability is a typed rejection before closure
bytes are accepted.

The normative proposed contract is
`schemas/cxxlens_ng_source_closure_transport.yaml`. Until this ADR is Accepted, the live accepted
protocol remains 1.1; the proposed contract reserves these IDs:

| ID | Message | Direction | Purpose |
| ---: | --- | --- | --- |
| 24 | `source_closure_manifest` | host to provider | Stream a descriptor plus contiguous chunks that bind task, closure identity, ordered members, blob census, and bounds. |
| 25 | `source_closure_blob` | host to provider | Start exactly one canonical-order blob. |
| 26 | `source_closure_chunk` | host to provider | Transfer a contiguous bounded occurrence of that blob. |
| 27 | `source_closure_seal` | host to provider | Bind manifest and every recomputed blob digest into one terminal digest. |
| 28 | `source_closure_ack` | provider to host | Attest complete validation and durable task-local staging. |
| 29 | `source_closure_reject` | provider to host | Return a typed phase-authentic failure and cleanup receipt. |

### State machine and phase-authentic fields

The only success path is:

`task-v4-sealed -> manifest-open -> manifest-streaming -> manifest-validated`
`-> blob-open -> blob-streaming -> blob-sealed`
`-> (blob-open ...)* -> closure-sealed -> closure-acknowledged -> task-accepted`.

`cancel` is legal from every nonterminal closure state and transitions to `cancelling`. On a live
connection it produces exactly one `source_closure_reject` with reason `source-closure.cancelled`;
connection loss or worker crash instead produces a host-local typed terminal and never fabricates a
peer reject. Cancel wins over a subsequently valid peer reject; a crash after observed cancel is
reported as `provider.crash` with cancel-observed evidence. Any missing, duplicate,
reordered, overlapping, extra, post-seal, or cross-task frame rejects the task. Worker crash or
connection loss discards the task-local spool; replay starts again at the manifest and transfers
every blob. A replay prefix, ack, or digest from another task/session is never reusable.

Field availability is phase-authentic:

| Phase | Available authority | Forbidden claims |
| --- | --- | --- |
| before manifest | task/session, task-v4 digest, expected closure/manifest identity, and bounded invalid-control frame count | member/blob census or received closure/payload bytes |
| manifest streaming | declared manifest size, observed bytes, offset, streaming digest | member/blob authority or terminal digest |
| manifest validated | closure/member/blob census and declared digests | blob bytes or terminal digest |
| blob streaming | current blob ordinal, offset, observed bytes, streaming digest | later blobs or closure completeness |
| closure sealed | recomputed blob/manifest/closure transfer digests | VFS mount or compiler outcome |
| acknowledged | task-local spool receipt and cleanup owner | execution/output success |
| rejected | failure phase, typed reason, observed bounded counters, cleanup receipt | values belonging to later phases |

`source_closure_ack` is not a compiler or publication success. `task_accepted` remains impossible
until task v4, manifest, all blobs, and the terminal seal validate.

### Identity, canonicality, and replay

Every message uses deterministic closed-map CBOR, the exact open-task stream, a contiguous shared
session sequence, zero flags, and the exact control/payload fields in the machine contract. The
control identities are typed, not free text: session is
`provider-session:sha256:<64-lower-hex>`, task is
`task:semantic-v2:sha256:<64-lower-hex>`, and spool/cleanup owner/cleanup receipt use their exact
`semantic-v2` domains from the contract. The executable transfer witness rejects a foreign identity,
non-contiguous manifest/blob index or offset, non-canonical blob ordinal, incomplete seal census, or
ack not bound to the recomputed terminal digest. The
manifest `kind` field is the descriptor/chunk discriminant. The manifest is first and its canonical
member/blob order is the ADR 0101 order. Message 24 first
declares its total length and digest, then repeats with contiguous chunks of at most 1 MiB; semantic
fields are unavailable until the complete canonical manifest is revalidated. Blob descriptors
must follow manifest blob order; chunks are contiguous from offset zero with monotonically
increasing indices. Each frame payload has the existing frame SHA-256, each completed blob is
recomputed against its manifest digest, and the seal binds task ID, task-v4 digest, manifest digest,
session ID, the streaming digest of the canonical ordered blob-receipt array, blob count, total bytes,
and closure digest. The complete receipt array is never placed in message 27; its single semantic
digest keeps the terminal control below 64 KiB even for 4096 blobs. These are independent projections; a
matching frame checksum cannot substitute for blob or closure validation.

The worker uses only the validated task-local spool to construct ADR 0101 values and mount its VFS.
Ambient filesystem content, a physical checkout, process CWD, or an unqualified system path cannot
satisfy a closure member.

The manifest is the closed `cxxlens.source-closure-manifest.v1` object defined by
`schemas/cxxlens_ng_source_closure_manifest_v1.schema.yaml`. Request 2.2, task v4, manifest,
blob-receipt, and transfer identities use the exact domains and projections in the machine contract.
Projection objects use sorted-key, no-whitespace UTF-8 canonical JSON; semantic digests wrap those
bytes with the repository `cxxlens-semantic-digest-v2` canonical-binary framing. Reject reasons and
the only counters available at each failure phase are fixed by `failure_phase_matrix`; later-phase
values are omitted rather than synthesized.

### Bounds

- members and unique blobs: 4096 each;
- logical path: 4096 UTF-8 bytes;
- one blob: 16 MiB; aggregate unique blob content: 48 MiB;
- manifest: 40 MiB in at most 40 one-MiB payload chunks; blob chunk payload: 1 MiB; at most 16 chunks
  per blob and 4144 blob chunk frames per closure, covering the 4096-small-blob rounding case;
- task-local spool: at most 88 MiB including manifest and content;
- resident transport working set: at most one 1 MiB chunk plus 256 KiB of parser/digest state.

Counts, lengths, offsets, and additions are checked before allocation or write. Before
`task_accepted`, a host-monotonic five-second send-progress deadline and five-second seal-to-ack
deadline bound the lifecycle without reusing NG1 heartbeat or resume semantics. The spool is
backend-owned, private to one task, digest-bound, sealed before semantic use, and removed on reject,
cancel, crash recovery, or task terminal. No complete closure-sized memory copy is permitted.

### Cache and compatibility

Cross-task blob cache is excluded from v1. Every closure is transferred completely for every task.
No cache capability may be advertised until a separate accepted ADR defines lifetime, eviction,
restart, tenant/session binding, and negative replay behavior.

Request 2.2 is a full authority derived from request 2.1: all non-source authority remains, worker
and trust protocol selections become 1.2, and task source bytes are replaced by task-v4 closure
references. It never nests an executable 2.1 request and never carries `content_base64`. Protocol
1.1 peers continue request 2.1/task v3 only. Protocol 1.2 accepts request 2.2/task v4 only, requires
`task-source-closure-v1`, and transfers the canonical complete task-v4 metadata through the bounded
task-input frames before closure frames so the provider independently recomputes its identity.
The main-source manifest member is cross-bound byte-for-byte to task v4's file ID, logical path,
content digest, byte size, encoding, and read-only flag; path agreement alone cannot reseal divergent
metadata.
Unknown required message IDs and
attempted implicit downgrade fail closed. Message 23 remains heartbeat and IDs 24--29 cannot be
allocated by NG1.

## Crash/effect matrix

| Event | External effect | Recovery |
| --- | --- | --- |
| before manifest validation | none | reject without spool |
| during manifest/blob/chunk | private unsealed spool only | remove spool; full replay required |
| after seal, before ack | sealed private spool only | remove on lost session; full replay required |
| after ack, before execution | task-local sealed spool only | cancel removes it; no cache survives |
| worker crash | no publication or ambient mutation | authenticated cleanup scan removes orphan spool |
| disk full/resource limit | no task acceptance | typed reject with observed counters and cleanup receipt |

## Counterexamples that must be rejected

Missing/duplicate/reordered manifests or blobs; overlapping/gapped/duplicate chunks; content or
terminal tamper; count, path, chunk, aggregate, or spool limit overflow; blob not referenced by the
manifest; manifest member without a blob; task/session/digest rebinding; frames after seal; stale or
foreign ack/replay; cancellation followed by content; capability omission; protocol downgrade;
message ID collision; ambient shadow content; and using an NG1 resume token as closure evidence.

## Consequences and non-goals

This adds an explicit transport rather than overloading `input_descriptor`/`input_chunk`. It costs a
full transfer per task in v1, but gives bounded staging, precise failure phases, and replay without
hidden state. It does not implement the codec, worker/VFS wiring, installed integration, cxxmonster
E2E, or NG1 hardening; those remain dependency-ordered implementation and direct-test work.

## Acceptance and implementation

Acceptance requires the machine checker and direct positive, negative, fault, determinism, and
resource-bound tests for the protocol state machine. The proposal witness covers the six phases,
counterexamples, canonical ordering, replay identity, cleanup, cancellation, and checked bounds.
A later bounded implementation unit must add codec/state-machine integration tests and the full
provider/VFS path before the IDs are enabled. Until then the live registry remains 1.1 and fails
closed; this ADR does not change the supported release surface.
