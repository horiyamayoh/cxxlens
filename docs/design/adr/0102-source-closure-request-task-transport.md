# ADR 0102: Source closure request/task transport

- Status: Proposed for independent review
- Date: 2026-08-20
- Decision owner: repository owner
- Decision issue: #261
- Implementation issue: #261
- Design feedback: DF-0261
- Amends: ADR 0096
- Depends on: ADR 0101, the accepted `task-input-chunks-v1` feature in
  `schemas/cxxlens_ng_provider_protocol.yaml`

## Context

ADR 0101 accepted a digest-addressed source-closure identity
(`source_closure_snapshot`, `cxxlens.clang22.source-closure.v1`) and a
read-only compiler-facing VFS (`source_closure_vfs`) that Clang 22
materialization can mount to resolve project headers, generated headers,
forced includes, and macro files beyond the one main-source payload ADR 0096
already streams. It deliberately left the request/task wire representation
as a non-goal, requiring "a future ADR" to define "bounded decoded/retained
byte limits, chunk canonicality, duplicate-chunk handling, cache/reuse
authority, cancellation, and replay evidence... before any request or task
schema changes." This ADR is that future ADR, scoped to exactly that
transport question and nothing else.

An earlier candidate attempt (never merged, never accepted; see ADR 0101's
Context) tried to solve delivery by inventing a whole new request v2.2
schema, a new `task.v4` wire message sequence, and new
`compiler_vfs`/`clang_compiler_vfs` bridge types, in the same change that
introduced closure identity. Independent review rejected the branch outright
and none of that wire-format layer -- `materialization_request_v2_2.*`,
`provider_task_v4*.*`, `compiler_vfs.*`, `clang_compiler_vfs.*` -- was
carried forward; it never compiled against the closure/VFS shape that was
ultimately accepted, and never went through independent review as a
standalone design. This ADR does not resurrect that code or that shape.

The accepted Provider Protocol (`schemas/cxxlens_ng_provider_protocol.yaml`,
protocol 1.1, feature `task-input-chunks-v1`) already defines a bounded,
hardened mechanism for delivering exactly one input blob to exactly one task:
`open_task` declares a single `task_input_digest`; `input_descriptor` plus
zero-or-more `input_chunk` frames deliver up to `maximum_input_chunks: 64`
chunks of at most `maximum_chunk_payload_bytes: 1048576` (1 MiB) bytes each,
for at most `maximum_logical_input_bytes: 67108864` (64 MiB) logical bytes
total, terminated by a streaming SHA-256 check against the declared digest.
Before task acceptance this transport already rejects missing, duplicate,
reordered, overlapping, extra, short, truncated,
descriptor-mismatch, chunk-mismatch, payload-digest-mismatch,
terminal-digest-mismatch, and limit-exceeded chunk streams
(`task_input_transfer.rejection.before_task_accepted`). A `cancel` message
(id 18) and a `[requested, acknowledged, grace_expired, killed, terminal]`
cancellation state machine already exist in the accepted protocol,
independent of what kind of input is being transferred.

ADR 0101's own closure bounds -- at most 4096 members, 4096 unique blobs,
4096 UTF-8 bytes per logical path, 16 MiB per blob, and 48 MiB of aggregate
unique blob content per closure (`source_closure.cpp`'s
`maximum_members`/`maximum_unique_blobs`/`maximum_logical_path_bytes`/
`maximum_blob_bytes`/`maximum_unique_blob_bytes`) fit entirely inside the
existing transport's 64 MiB logical-input ceiling, with headroom for a
manifest section.

## Decision

Transport one `source_closure_snapshot` as the existing single per-task
input, encoded as one canonical byte sequence (the "closure container"),
delivered through the already-accepted `task-input-chunks-v1` mechanism
completely unmodified. No new wire message, frame type, message ID, or
protocol minor version is introduced. The request/task schema's existing
single `task_input_digest`/`input_digest` binds to this container's bytes
exactly as it already binds to any other task input today.

### Closure container encoding (`cxxlens.clang22.source-closure-container.v1`)

A closure container is the canonical serialization of one
`source_closure_snapshot` as a single logical byte string, independent of
transfer chunking (chunking is purely a transport-level concern already
solved by `task-input-chunks-v1`; the container format never encodes chunk
boundaries).

Layout, all multi-byte integers big-endian:

- 16-byte magic `CXXLSRCCLOSUREv1` (ASCII, exactly 16 bytes).
- `member_count: u32` -- must equal `source_closure_snapshot.members.size()`
  and satisfy ADR 0101's `maximum_members` bound.
- `blob_count: u32` -- must equal `source_closure_snapshot.blobs.size()` and
  satisfy `maximum_unique_blobs`.
- The manifest section: `member_count` entries in the snapshot's existing
  canonical member order (ADR 0101's identity order, not transfer order),
  each: `logical_path_length: u16` + logical path UTF-8 bytes (bounded by
  `maximum_logical_path_bytes`), `role: u8` (matches
  `source_closure_role`'s existing enum values 1-5), `encoding: u8` (matches
  `source_closure_encoding`'s existing enum values 1-5), `read_only: u8`
  (0 or 1), `size_bytes: u64`, `content_digest_length: u16` + content digest
  ASCII bytes (`sha256:` + 64 hex characters, i.e. always 71 bytes in
  practice, but the length prefix is still validated rather than assumed).
- The blob section: `blob_count` entries in the snapshot's existing
  canonical blob order (content-digest order, matching
  `source_closure_snapshot.blobs`), each: `content_digest_length: u16` +
  content digest ASCII bytes, `size_bytes: u64`, then exactly `size_bytes`
  raw content bytes.
- 32-byte trailer: raw SHA-256 over every preceding byte (magic through the
  last blob's last content byte). This is a container-internal integrity
  check, not a substitute for the transport's own `task_input_digest`
  terminal check (see Replay evidence below) -- it exists so a decode error
  is diagnosable as "container corrupt" before semantic decode is attempted,
  the same layering ADR 0101 already uses for the closure digest versus the
  transport-agnostic member/blob validation.

Decode is fail-closed and exact, mirroring `source_closure.cpp`'s existing
validation discipline: any short read, any length-prefixed field whose
declared length does not fit the remaining bytes, any trailing bytes after
the trailer, any `member_count`/`blob_count` exceeding ADR 0101's bounds, any
`role`/`encoding` byte outside its accepted enum range, any
`content_digest_length` not matching the actual `sha256:`-prefixed 71-byte
form, or any manifest entry whose `content_digest` has no matching blob
section entry, rejects the whole container before a `source_closure_snapshot`
is constructed. A successfully decoded container is then handed to the
existing `make_source_closure_snapshot`-equivalent validation path (or,
since members/blobs are already individually valid and pre-sorted by
construction on the host side, the decoder may skip re-sorting but must
still re-run every `source_closure_member::validate()` /
`source_closure_blob::validate()` / aggregate-bound / closure-digest check
that `make_source_closure_snapshot` performs -- decode trusts no field as
pre-validated). The worker must never construct a `source_closure_vfs` from
a closure whose recomputed `closure_digest` does not match the digest
implied by the request (see Request/task binding).

### Request/task binding

A materialization request that needs a source closure for a task declares
the closure's `closure_digest` (ADR 0101's order-independent content-binding
digest) in the task's existing request-level fields, exactly as
`installed_executable_digest` and other content digests are already declared
elsewhere in `schemas/cxxlens_ng_clang22_materialization_request.schema.yaml`.
The provider worker, after decoding the container and reconstructing the
`source_closure_snapshot`, must reject the task if the reconstructed
`closure_digest` does not exactly equal the request-declared digest --
this closes the gap a container-internal trailer alone cannot close (a
correctly-checksummed container could still, in principle, carry a
different-but-internally-consistent closure than the one the request
committed to; the cross-check against the request-level digest is the
actual binding authority, matching D3's "stream header and trailer claims
are cross-checks, never completeness authority" principle already accepted
for DF-0200's transport layer).

### Cache/reuse authority: explicitly deferred, not activated

This ADR does **not** define any mechanism for a worker to reuse a
previously-delivered blob across tasks by digest alone. Every task that
needs a source closure transmits its complete container, even if an earlier
task in the same provider session already delivered an overlapping or
identical blob. This is the safe, stateless default: it costs bandwidth on
repeated-header-heavy workloads but introduces no session-scoped cache state,
no invalidation policy, and no crash/restart-recovery semantics to get
wrong.

A future capability `source-closure-cache-v1` is reserved (present as a
name in this ADR so a later ADR can activate it without a naming collision)
but is not part of the accepted Provider Protocol and must not be
negotiated, advertised, or implemented against this ADR. Activating
cross-task caching needs its own dedicated ADR covering at minimum: cache
key scope (per-process? per-session? per-worker-restart?), staleness/
eviction, what a worker must do if asked to reuse a digest it never actually
received intact, and interaction with the cancellation/crash paths below.

### Cancellation

No changes. A closure-carrying task is cancelled exactly like any other task,
through the existing `cancel` message and
`[requested, acknowledged, grace_expired, killed, terminal]` state machine.
Because closure delivery uses the ordinary `input_descriptor`/`input_chunk`
frames, a cancellation mid-transfer is already handled by whatever the
existing transport does for any other input cancelled mid-chunk-stream --
no closure-specific cancellation behavior is introduced or needed.

### Replay evidence

A closure container's bytes are exactly one task's declared input; they are
therefore already covered end-to-end by the existing input-transfer terminal
digest check (the streaming SHA-256 over received chunk bytes must equal the
declared `input_digest`) and by whatever task-receipt / execution-journal
machinery this repository already binds to task inputs generally (see
DF-0200 D3 for the shape of task receipts this repository uses elsewhere for
an analogous problem). No new replay-evidence primitive is introduced,
because closures are deliberately delivered as an ordinary task input rather
than through a bespoke side channel -- that uniformity is the point of this
ADR's Decision.

### Materializer wiring (left to implementation, not specified here)

This ADR fixes the wire contract two endpoints must agree on: a host-side
encoder (`source_closure_snapshot` to closure container bytes, most likely
as a small serializer next to `source_closure.cpp`) and a worker-side
decoder (closure container bytes to `source_closure_snapshot`, feeding
`source_closure_vfs::mount`). Wiring either endpoint into
`tools/clang22/materialize_main.cpp` and `provider_worker.cpp` -- deciding
when a task needs a closure at all, assembling the `source_closure_snapshot`
from real project files before encoding, and threading the mounted VFS
through to the actual Clang invocation -- is implementation work built on
top of this ADR, not decided by it.

## Non-goals

- Cross-task worker-side blob caching or dedup beyond the within-closure
  content-digest dedup the closure model already guarantees (see
  Cache/reuse authority above).
- Materializer request/task processing wiring itself
  (`tools/clang22/materialize_main.cpp`, `provider_worker.cpp`) -- a
  separate, independently reviewable implementation unit built on top of
  this contract.
- Installed/relocated qualification, memory/SQLite publication parity for
  materialized output, and release qualification evidence for this
  transport.
- Raising the closure bounds beyond ADR 0101's accepted limits, or the
  transport's existing 64 MiB / 64-chunk / 1 MiB-chunk limits. A closure
  that does not fit stays a hard failure, not a reason to renegotiate
  either bound.
- A second admitted `agent-context` golden path or any other unrelated #277
  scope; this ADR is only about the wire contract.

## Rejected alternatives

1. **One `input_descriptor`/`input_chunk` exchange per closure member**
   instead of one container carrying the whole closure. Rejected: the
   accepted protocol's `input_descriptor_control.binding` is
   `task-id-and-input-digest-exact-open-task-match` -- it assumes one input
   digest per task. Multiplying that per member would need new
   task-scoped-multi-input disambiguation the protocol was not designed for,
   for no benefit over a single container: the byte-level chunk/digest
   guarantees are identical either way, since `task-input-chunks-v1`
   already treats its one logical input as an opaque byte string.
2. **A whole new request v2.2 / task v4 schema**, i.e. the previously
   rejected approach. Rejected for the same reason ADR 0101 rejected
   reviving it: it changes an already-shipped, already-qualified
   request/task contract shape for a problem that does not need a contract
   version bump to solve.
3. **Cross-task worker-side caching bundled into this ADR.** Rejected for
   this pass: a real bandwidth benefit for repeated-header-heavy workloads,
   but real complexity -- session-scoped state, invalidation, and
   crash/restart recovery -- that deserves its own dedicated design and
   independent review rather than being folded into the base transport
   contract. See Cache/reuse authority above.
4. **A dedicated new frame/message type for closures** (e.g. a
   `closure_descriptor`/`closure_chunk` pair mirroring
   `input_descriptor`/`input_chunk` but closure-aware). Rejected: this
   would duplicate the entire hardened chunk-transport negative-case matrix
   (missing/duplicate/reordered/overlapping/extra/short/truncated/
   descriptor-mismatch/chunk-mismatch/payload-digest-mismatch/
   terminal-digest-mismatch/limit-exceeded) for a format-level distinction
   the existing transport does not need to know about; the container
   encoding already gives the worker everything it needs to tell a closure
   input apart from an ordinary main-source input (the container's own
   16-byte magic), without protocol-level cost.

## Consequences

- Delivering a source closure to a provider worker needs no Provider
  Protocol version bump, no new message type, and no change to
  `schemas/cxxlens_ng_provider_protocol.yaml`'s accepted 1.1 profile --
  only a new logical content format layered on top of a mechanism (
  `task-input-chunks-v1`) that is already accepted, already hardened, and
  already has its own independent negative-case test matrix.
- The request/task schema gains exactly one new digest-typed field (the
  declared `closure_digest`) rather than a new object graph; this keeps the
  change closer to "one more content digest, like several others the
  request already carries" than to a structural schema change.
- Cross-task caching stays a real, named, explicitly future capability
  (`source-closure-cache-v1`) rather than something implementation drifts
  into ad hoc; a later ADR can activate it without renegotiating this one.
- Because closure delivery reuses `task-input-chunks-v1` verbatim, every
  existing chunk-transport hardening test (the negative-case matrix, the
  64 MiB/64-chunk/1 MiB-chunk limits, cancellation-mid-transfer behavior)
  already applies to closure-carrying tasks without closure-specific
  duplication -- implementation only needs new tests for the container
  encode/decode layer itself and the request-digest cross-check, not for
  the transport those bytes ride on.

## Verification

Implementation of this ADR must include, at minimum, before any
`resolution_refs` entry can cite it as complete:

- Round-trip container encode/decode tests: an arbitrary valid
  `source_closure_snapshot` (including the boundary cases ADR 0101's own
  test suite already exercises -- empty-ish minimal closures, maximum
  member/blob counts, maximum path length, maximum blob size, maximum
  aggregate size) encodes and decodes back to an equal snapshot.
- Fail-closed decode tests: truncated container, trailing bytes after the
  trailer, corrupted trailer checksum, `member_count`/`blob_count`
  exceeding ADR 0101's bounds, out-of-range `role`/`encoding` bytes,
  `content_digest_length` not matching the exact `sha256:`-prefixed 71-byte
  form, a manifest entry whose `content_digest` has no corresponding blob
  section entry, and a blob section entry no manifest entry references --
  each must reject before a `source_closure_snapshot` is constructed, not
  merely produce a corrupt one.
- Request-digest cross-check test: a container that decodes and
  self-validates cleanly but whose recomputed `closure_digest` does not
  match the request-declared digest must still be rejected.
- A test proving closure-carrying tasks are correctly cancellable
  mid-transfer through the existing, unmodified `cancel` mechanism (no
  closure-specific behavior should be needed; the test exists to prove
  that claim rather than assume it).
- A test proving the existing `task-input-chunks-v1` negative-case matrix
  (missing/duplicate/reordered/overlapping/extra/short/truncated/
  descriptor-mismatch/chunk-mismatch/payload-digest-mismatch/
  terminal-digest-mismatch/limit-exceeded) still rejects malformed transfers
  of closure-container-typed input exactly as it already does for any other
  input -- i.e. that layering a closure on top introduced no bypass.
- An end-to-end test: a real multi-file project closure built with
  `make_source_closure_snapshot`, encoded, decoded on the "worker" side of
  a real (not mocked) `task-input-chunks-v1` exchange, and mounted with
  `source_closure_vfs::mount`, then used to satisfy a real `clang++-22`
  compile that needs an included project header -- proving the whole chain
  from host-side snapshot to a real Clang invocation, not just the codec in
  isolation.

## Acceptance gate

Per `check_ng_design_feedback.py`'s fail-closed rule for `impact: security`
design-feedback records (DF-0261 is one), this ADR must reach `Status:
Accepted` and be independently reviewed and bound to a
`https://github.com/horiyamayoh/cxxlens/issues/261#issuecomment-<N>` URL,
with a reviewer distinct (case-insensitively) from this ADR's author, before
any `resolution_refs`/`review` update citing it can move DF-0261's remaining
scope toward `accepted`. This ADR alone -- even once accepted -- does not by
itself change DF-0261's frontmatter; it only establishes the transport
contract that implementation and its own independent review must still be
measured against, exactly as ADR 0101 did for the closure-identity and VFS
units before those were separately implemented, reviewed, and only then
cited as resolution evidence.
