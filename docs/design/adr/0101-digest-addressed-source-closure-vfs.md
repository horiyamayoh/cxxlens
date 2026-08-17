# ADR 0101: Digest-addressed source closure and compiler VFS

- Status: Proposed for independent review
- Date: 2026-08-17
- Decision owner: repository owner
- Decision issue: #261
- Implementation issue: #261
- Design feedback: DF-0261
- Amends: ADR 0096, ADR 0010

## Context

The installed Clang 22 materialization boundary can authenticate and stream one
main-source payload, but an ordinary translation unit also depends on project
headers, generated headers, forced includes, and macro files. Those bytes are
not representable in request v2.1 or task.v3. Letting Clang discover them from
the host checkout would make semantic output depend on mutable ambient state
that is absent from request, task, and snapshot identity.

The SQLite `materialization_rooted_vfs` is an effect-rooting Store VFS and is not
a compiler input filesystem. The missing surface is an immutable source closure
and a compiler-facing read-only VFS constructed from that closure.

## Decision

Introduce the app-neutral contract
`cxxlens.clang22-source-closure-contract.v1` and keep every existing v2.1 and
task.v3 byte projection unchanged.

The successor materialization path is:

```text
cxxlens.clang22-materialization-request.v2 / request_version 2.2.0
  -> cxxlens.clang22.task.v4
  -> Provider Protocol 1.1 with required features
       [task-input-chunks-v1, task-source-closure-v1]
  -> read-only compiler VFS
```

`task-input-chunks-v1` remains only the authenticated physical byte transport.
`task-source-closure-v1` is a separately negotiated semantic capability and
therefore does not widen the meaning of an existing feature.

A source closure is an ordered finite map from canonical `project://` logical
paths to immutable regular-file records and content-addressed blobs. It binds:

- one closure snapshot ID and semantic digest;
- exactly one `main` member selected by the task;
- zero or more `header`, `generated`, `forced-include`, and `macro-file`
  members;
- exact file identity, role, encoding, byte count, content digest, and read-only
  state;
- one deduplicated blob table keyed by full SHA-256 content digest;
- the selected effective invocation, logical working directory, toolchain, and
  system-input policy through the enclosing task.

The closure digest is independent of transfer chunking, task order, physical
staging, cache placement, and host checkout location. Member order is ascending
canonical UTF-8 byte order by logical path. Blob order is ascending content
digest. Duplicate logical paths, Unicode-normalization aliases, Unicode default
case-fold aliases, conflicting metadata, missing blobs, digest mismatches, and
multiple or missing main members are rejected.

Logical paths are strict UTF-8 NFC, use the exact `project://` scheme and `/`
separator, contain no empty, `.` or `..` segment, control character, backslash,
query, fragment, authority replacement, or trailing slash. Symlink, hard-link,
device, socket, FIFO, and magic-link records are not representable.

The compiler VFS mounts the closure at a private synthetic root. Every project
or generated path is resolved only through the closure. A lookup beneath that
root that is absent from the closure returns a typed missing-input failure and
must not delegate to the process working directory or host filesystem.
Toolchain builtin headers, an admitted resource directory, and an admitted
sysroot remain a separate qualified surface. They are usable only when the
exact effective invocation and toolchain authority admit them; no silent
system-path discovery is permitted.

The first implementation profile is bounded to 4096 members, 4096 unique blobs,
4096 UTF-8 bytes per logical path, 16 MiB per blob, and 48 MiB of unique closure
content per task.v4. The request-level aggregate remains bounded separately and
tasks are decoded, mounted, executed, and released one at a time. Immutable
host-side blob storage may be shared across tasks by digest without changing
closure or task identity.

## Failure contract

Malformed closure input fails before parser, worker output, Store, or publication
effects. Stable failures distinguish at least:

- `source-closure.path-invalid`;
- `source-closure.path-not-nfc`;
- `source-closure.path-collision`;
- `source-closure.case-collision`;
- `source-closure.duplicate-member`;
- `source-closure.role-invalid`;
- `source-closure.blob-missing`;
- `source-closure.digest-mismatch`;
- `source-closure.limit-exceeded`;
- `source-closure.main-invalid`;
- `source-closure.member-missing`;
- `source-closure.ambient-fallback-denied`;
- `source-closure.toolchain-input-unqualified`.

No failure path publishes claims, a snapshot head, or a formal artifact.

## Compatibility

Request v2.1 and task.v3 remain valid for the existing self-contained
single-source profile and retain their exact canonical identities. A v2.1 host
or worker cannot claim source-closure support. A v2.2 task requires both feature
names and fails closed when either side omits them. Downgrade to task.v3, copied
headers, a staged checkout, or ambient path lookup is forbidden.

## Constructibility witness

The executable phase graph is:

```text
raw-task-bytes
  -> task-v4-decoded
  -> closure-schema-validated
  -> closure-identity-bound
  -> closure-vfs-mounted
  -> clang-executed
  -> detached-output-validated
  -> closure-released
```

Failure branches may expose only values available at or before their phase.
The implementation uses bounded streaming/spooling, one task and one mounted
closure at a time, and no write-capable compiler filesystem. The VFS creates no
external filesystem effect, so crash recovery consists of process-local object
release; Store publication remains governed by ADR 0096.

The minimal witness is a main source including nested project and generated
headers from an in-memory closure while an ambient shadow file contains
conflicting bytes. The closure bytes must win, and removing the required member
must produce a typed failure rather than ambient success.

## Alternatives rejected

- Passing a checkout path or file descriptor not bound by closure identity.
- Copying headers into the main source.
- Adding consumer-specific request fields.
- Reinterpreting `task-input-chunks-v1` without a new required feature.
- Overlaying an in-memory filesystem on the real working directory with
  fallthrough for project/generated paths.
- Mutating task.v3 canonical bytes.

## Acceptance gate

This ADR is proposed on the implementation branch. Before its status becomes
Accepted and DF-0261 becomes `may-proceed`, an independent reviewer must examine
the whole contract, especially path/case normalization, digest projection,
resource bounds, toolchain separation, and ambient-fallback denial. Merge and
production qualification remain separate from bounded implementation.
