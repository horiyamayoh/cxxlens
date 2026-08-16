---
id: DF-0261
title: Bind materialization tasks to a digest-addressed source-closure VFS
status: proposed
kind: missing-assumption
impact: security
confidence: high
implementation_disposition: blocked
scope:
  - provider.clang22-source-closure-vfs
  - provider.materialization-source-snapshot
  - provider.source-closure-path-admission
  - release.clang22-project-e2e
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_clang22_materialization_request.schema.yaml
  - schemas/cxxlens_ng_provider_task.schema.yaml
  - schemas/cxxlens_ng_provider_protocol.yaml
tracking_issue: '#261'
implementation_issues:
  - '#261'
resolution_refs: []
review:
  mode: independent
  status: pending
  author: codex-agent-source-closure-preflight
  reviewer: null
  refs: []
created: '2026-08-16'
---

# Bind materialization tasks to a digest-addressed source-closure VFS

## Observation

The installed Clang 22 materialization request and worker task currently carry one
inline main-source object. The source object binds one snapshot ID, one file ID,
one logical path, one content digest, one size, one encoding, one line-index ID,
and one base64 body. The worker SDK then invokes Clang through
`runToolOnCodeWithArgs()` with that single in-memory main file. There is no
request-level representation for the authenticated project-header, generated
input, module, or other compiler-visible source closure needed to reproduce a
normal non-self-contained translation unit.

The current positive path can therefore prove parsing of a self-contained source
fixture while still being unable to reproduce an ordinary project whose main
source includes a project header. Allowing Clang to satisfy that include from an
ambient working tree would make the execution depend on bytes that are absent
from the request identity, source snapshot, task digest, and publication
evidence. Refusing the include is fail-closed, but it leaves the real-project
installed end-to-end path blocked.

Issue #261 already named this implementation-learning record as its authority
entry point, but the record did not exist in the repository. The existing
single-source SDK preflight also tested `logical_path.find("..")`, treating any
pair of dots as traversal. That rejects legitimate components such as
`foo..bar.hpp` and offers no reusable exact-segment primitive for the later
multi-file boundary.

## Working mental model

Compiler input is an immutable, authenticated graph rather than one privileged
source string or a directory that Clang may inspect freely. A materialization
task needs three separately governed input surfaces:

1. A project/generated source closure whose members have stable logical paths,
   exact byte digests, encoding metadata, and snapshot membership.
2. A qualified toolchain surface for built-in headers, an admitted sysroot, and
   other compiler-owned resources whose identity is bound by the selected
   toolchain contract rather than copied into every task.
3. Effect-bearing Store and publication facilities, which are not compiler
   source inputs and must remain behind their existing capability boundaries.

The source closure is a finite map from canonical logical path to immutable file
content and metadata. Its identity must be independent of transfer order,
chunking, cache placement, physical staging directory, and host checkout. The
compiler-facing filesystem is a read-only view over that map plus only the
explicitly qualified toolchain surface. A missing project/generated member is a
determinate input failure; it is not permission to fall back to the ambient
filesystem.

Path admission is part of this boundary but is not, by itself, the source-closure
contract. Exact parent components must be recognized by segment, so `..` is
rejected while `foo..bar.hpp` remains valid. Unicode normalization, case model,
logical scheme grammar, collision rules, symlink policy, and toolchain-path
mapping still require accepted authority before the closure wire format or VFS
is implemented.

## Mismatch or opportunity

ADR 0096 requires the installed request to bind source/input/invocation identity
and to fail closed before effects on malformed or mismatched input. The current
strict v2.1 request schema has no extension point for a source closure and
advertises only `task-input-chunks-v1`; adding an unversioned sibling field or
silently widening that feature would change canonical request identity,
transport bounds, worker capability negotiation, replay semantics, and report
traceability at once.

The implementation seam is likewise too narrow. `runToolOnCodeWithArgs()` is
convenient for one main file, but it is not an authority for a bounded project
filesystem. A direct jump from the present schema to a large VFS implementation
would couple five decisions that need independent review: closure identity,
logical-path canonicalization, bounded transfer and reuse, compiler filesystem
composition, and negative qualification against ambient lookup. Discovering a
contradiction in any one of those after implementation would force a broad
rewrite and stall the next development lane.

There is an opportunity to remove reversible blockers now without pretending
that the high-risk contract is accepted. The missing feedback record can be
restored, the work can be split into reviewable units, and the existing
single-source path check can use exact path segments. Public request fields,
wire features, source-closure identity, and compiler VFS behavior remain
blocked.

## Evidence

- `schemas/cxxlens_ng_clang22_materialization_request.schema.yaml` requires one
  strict `source` object per task and has no authenticated collection of
  compiler-visible project/generated files.
- The same schema fixes the worker `required_features` value to
  `[task-input-chunks-v1]`, so source-closure transport cannot be added as an
  implicit interpretation of the current capability.
- `src/llvm/clang22/provider_sdk.cpp` constructs the Clang invocation from one
  `translation_unit_input::source` and one compiler filename through
  `runToolOnCodeWithArgs()`.
- Before this preflight, `translation_unit_input::validate()` rejected every
  path containing the substring `..`, including a non-traversing filename such
  as `src/foo..bar.hpp`.
- `docs/design/adr/0096-clang22-installed-materialization-boundary.md` binds the
  installed tool to strict versioned request validation, exact source/input
  identity, bounded input, qualified execution, and fail-closed publication.
- Issue #261 records the missing real-project source closure and the requirement
  for digest-bound files, traversal-free paths, bounded transport/reuse,
  explicit VFS composition, and negative ambient-filesystem tests.
- The path named by issue #261,
  `docs/development/implementation-learning/records/df-0261-source-closure-vfs.md`,
  was absent before this record was added.

## Alternatives and trade-offs

1. Stage a physical checkout and pass its directory to Clang. This is simple to
   prototype but is rejected as authority because directory contents,
   symlinks, races, case behavior, and unrecorded host files can affect parsing
   without changing task identity.
2. Inline every file directly into the existing v2.1 task object without a new
   capability or version. This is rejected because it silently changes the
   strict request shape, canonical digest projection, maximum input accounting,
   worker negotiation, and replay/report semantics.
3. Add a provider-specific opaque application field. This avoids a generic
   contract but hides path, digest, bound, and capability semantics from the
   installed tool and qualification checkers. It is rejected for the reference
   materializer.
4. Define a versioned digest-addressed closure contract, transfer it through a
   separately negotiated bounded capability, and mount it into a read-only
   compiler VFS. This is the recommended direction, but it remains blocked
   until its identity and security rules are accepted by ADR and independently
   reviewed.
5. Continue reversible prework only. Creating this record, exposing an exact
   parent-segment helper, building independent fixtures, and measuring the
   Clang VFS seam do not choose the closure identity or wire contract. This is
   selected for the current preflight.

## Recommendation

Implement issue #261 as four ordered review units rather than one cross-cutting
change:

1. Accept an ADR and machine contract for closure membership, canonical logical
   paths, duplicate/collision handling, file metadata, closure digest,
   toolchain/system-resource separation, and missing-member failures. Include
   Unicode normalization, case model, symlink prohibition or semantics, and
   deterministic ordering explicitly.
2. Define a bounded, versioned transfer and capability contract. Bind decoded
   and retained byte limits, file/member counts, per-file limits, chunk
   canonicality, duplicate chunks, digest verification, cache/reuse authority,
   cancellation, and replay evidence. Do not reuse
   `task-input-chunks-v1` under widened semantics.
3. Implement the compiler-facing read-only VFS from the accepted closure and
   admitted toolchain surface. Project/generated lookup must not fall through
   to the process working directory. Keep physical staging, if used internally,
   non-authoritative and prove cleanup/race behavior.
4. Qualify installed and relocated artifacts with self-contained and real
   multi-file projects, missing/tampered/duplicate/traversal/case-collision
   inputs, ambient shadow files, bound exhaustion, cancellation, replay, and
   memory/SQLite publication parity. Bind the exact reports into release and
   production-scope evaluation.

Until unit 1 is accepted, implementation may proceed only on reversible
prework that cannot escape into public or wire authority. In particular, do not
add request fields, capability advertisements, cache keys, source-snapshot
identity rules, or ambient filesystem fallbacks. The segment-aware helper added
with this record is limited to the existing single-source SDK preflight and
intentionally states that the remaining path rules are unresolved.

## Disposition

2026-08-16: Proposed as a high-risk security record with
`implementation_disposition: blocked`. The immediate missing-record blocker is
removed, and the existing SDK now distinguishes exact parent path segments from
ordinary filenames containing two dots. The helper also recognizes both `/`
and `\` as parent-segment separators, rejects root-relative leading separators,
and rejects embedded NUL in the existing logical-path input.

This disposition does not accept a source-closure identity, request version,
wire feature, reuse/cache rule, compiler VFS, or production qualification. The
tracking issue #261 remains open. An independent reviewer and an accepted ADR are required
before implementation crosses any of those boundaries.
