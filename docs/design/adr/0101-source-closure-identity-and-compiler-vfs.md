# ADR 0101: Source closure identity and read-only compiler VFS

- Status: Accepted
- Date: 2026-08-19
- Decision owner: repository owner
- Decision issue: #261
- Implementation issue: #261
- Design feedback: DF-0261
- Amends: ADR 0096

## Context

The installed Clang 22 materialization boundary (ADR 0096) can authenticate
and stream exactly one main-source payload per task. An ordinary translation
unit also depends on project headers, generated headers, forced includes, and
macro files that are not representable in the current request/task contract.
Letting Clang discover those bytes from an ambient host checkout would make
semantic output depend on mutable state absent from request, task, and
snapshot identity -- refusing the include is fail-closed but leaves every
non-self-contained real project unmaterializable (DF-0261's Observation).

An earlier candidate implementation (branch
`agent/issue-261-source-closure-vfs-implementation`, PR #353) attempted the
full path from a versioned closure identity through a new request/task wire
format (`request v2.2`, `task.v4`) to a compiler-facing VFS in one
cross-cutting change. Independent review rejected it: two real defects in the
VFS bridge (a fail-closed audit that hard-failed on Clang's own ordinary
speculative filesystem probing, and an asymmetric enforcement gap for missing
closure members) plus roughly ten files of dead, non-compiling code from an
abandoned earlier iteration of the wire-format layer. Only the closure
identity and VFS logical-path layers survived review as sound.

A rebuilt, narrower attempt (worktree `agent/issue-261-source-closure-vfs-v2`,
now on `main` as commits `4f86142`, `1955d57`, `232a76d`, `e3fe17e`) fixed both
defects and went through two independent adversarial review rounds against a
real Clang 22 build. This ADR formalizes exactly what that reviewed code
implements -- closure identity and the compiler-facing VFS -- and nothing
more. It does not define a request/task wire format; see "Non-goals" below.

## Decision

### Closure identity (`cxxlens.clang22.source-closure.v1`)

A source closure is a finite map from canonical logical path to immutable
regular-file content and metadata (`src/llvm/clang22/source_closure.{hpp,cpp}`).
It binds:

- one closure snapshot ID and a content-binding closure digest;
- an ordered list of members, each with a `file_id`, a canonical
  `project://`-scheme logical path, a role (`main`, `header`, `generated`,
  `forced-include`, or `macro-file`), an encoding tag, an exact byte count, a
  content digest, and a read-only flag;
- a deduplicated blob table keyed by full `sha256:<64 lowercase hex>` content
  digest, independent of member count (multiple members may share one blob).

Exactly one member has role `main`; the builder
(`make_source_closure_snapshot`) rejects zero or multiple `main` members,
duplicate logical paths, orphaned blobs (a blob no member references), and any
member whose declared digest/size does not match its actual content.

Logical paths are strict UTF-8, Unicode NFC, and use exactly the
`project://` scheme with `/` separators. A path is rejected if it is empty,
exceeds 4096 bytes, contains a NUL or C0/DEL control character, is not valid
UTF-8, is not already in NFC form, has a leading or trailing `/`, contains
`\`, `?`, or `#`, or if any `/`-delimited segment is empty, `.`, or exactly
`..`. Segments containing dots without being exactly `.` or `..` (for example
`foo..bar.hpp`) are legal -- traversal rejection is exact-segment, not
substring. Two distinct logical paths that fold to the same string under
Unicode NFC + default case-fold are rejected as a collision, closing the
case/normalization-alias attack this closes off from ADR 0096's identity
model.

Member and blob order in a validated snapshot is canonical: members ascend by
logical path, blobs ascend by content digest. The closure digest is computed
over that canonical order and is therefore independent of transfer order,
chunking, physical staging location, and host checkout -- the same snapshot
content always produces the same digest regardless of how it was assembled.

Bounds enforced by the builder: at most 4096 members, at most 4096 unique
blobs, at most 4096 UTF-8 bytes per logical path, at most 16 MiB per blob, and
at most 48 MiB of unique blob content per closure (`source_closure.cpp`'s
`maximum_members`, `maximum_unique_blobs`, `maximum_logical_path_bytes`,
`maximum_blob_bytes`, `maximum_unique_blob_bytes`). These bounds govern the
closure value type only; no request- or task-level transport bound is defined
by this ADR (see Non-goals).

### Compiler-facing VFS

`with_source_closure_translation_unit`
(`src/llvm/clang22/source_closure_native.{hpp,cpp}`) mounts a validated
closure into an `llvm::vfs::ProxyFileSystem` for exactly one Clang invocation.
Every lookup resolves into one of three disjoint regions:

1. **Closure** -- beneath the closure's private synthetic root. Served from
   authenticated in-memory content built directly from the validated closure;
   never from the real filesystem.
2. **Qualified** -- beneath one of the caller-supplied, trusted-verbatim
   `qualified_read_roots` (the toolchain installation the materializer itself
   selected and pinned). Delegated to the real filesystem: real content that
   exists is served, and an ordinary miss is reported exactly as the real
   filesystem reports it. Root selection is the caller's security
   responsibility; this unit does not verify a root corresponds to any
   particular toolchain. Because this delegation follows symlinks, an
   admitted root bounds the *names* a lookup may use, not the *bytes* those
   names ultimately resolve to.
3. **Denied** -- everything else, including paths beneath the synthetic
   project root that are not closure members. This is where Clang's own
   speculative, distro/toolchain-dependent driver probing lands outside the
   project root (GCC installation candidates, `/etc/os-release`, and
   similar), and, inside the project root, where the includer-directory-first
   rule for quoted includes, each `-I`/`-iquote` entry in search order, an
   `-I` directory holding no closure members, and constructs such as
   `__has_include` routinely land even when the closure is complete. No
   static allowlist can enumerate Clang's full candidate set, and Clang
   itself already tolerates absence at all of these locations gracefully.
   Answered with a plain `ENOENT`, indistinguishable from a path that simply
   does not exist; the real filesystem is never consulted for a denied path,
   so ambient content is structurally unreachable through this route rather
   than excluded by an audit step that could be bypassed or mis-ordered.

**Member-aware missing-input enforcement.** The one filesystem event that
fails the whole task is a path the closure's own manifest claims as a member
(present in the validated closure's member set) that the mounted filesystem
could not serve. This is a determinate input failure
(`source-closure.member-missing`) and is checked unconditionally after the
Clang invocation returns, independent of whether Clang's own run reports
success -- a construct such as `__has_include` can make Clang tolerate the
very absence that must still fail this task, and gating the check on Clang's
own success/failure would let an unservable claimed member escape detection
whenever Clang itself happens not to need it. A closure that is merely
*incomplete* relative to what the source actually needs is a different case:
the probed path was never a member the manifest claimed, so Clang reports its
own ordinary "file not found" diagnostic and the task fails through the
Clang-tool-invocation result itself, not through `source-closure.member-missing`.
This distinction is deliberate: it is what makes an ordinary `include/` +
`src/` project layout materializable at all, since without it the
determinate-failure check cannot be told apart from Clang's own routine,
tolerated, speculative search misses.

**Ordering contract.** The translation-unit callback runs *inside* the Clang
invocation, before the closure-completeness verdict above is consulted. It
can therefore have already executed, and already produced output, on a call
that ultimately returns failure. Callers must treat everything the callback
emits as provisional until this function returns success, and must discard it
otherwise; a successful callback never overrides a failed verdict.

**Working-directory and traversal handling.** Every path is made absolute
against the VFS's current working directory and normalized
(`llvm::sys::path::remove_dots` with `..` removal) *before* being classified
into one of the three regions above, so a relative path or an embedded `..`
segment cannot escape classification. `setCurrentWorkingDirectory` accepts a
target only if it classifies into the closure region; a rejected target
leaves the working directory unchanged rather than half-updated.

## Non-goals

This ADR does **not** define:

- A request- or task-level wire representation for a source closure (no
  `request v2.2`, no `task.v4`, no new Provider Protocol capability). The
  rejected candidate's attempt at this layer
  (`materialization_request_v2_2.*`, `provider_task_v4*.*`,
  `compiler_vfs.*`, `clang_compiler_vfs.*`) was deleted rather than revived;
  none of that code compiled against the closure/VFS shape this ADR
  describes. A future ADR must define that transport layer -- bounded
  decoded/retained byte limits, chunk canonicality, duplicate-chunk handling,
  cache/reuse authority, cancellation, and replay evidence -- before any
  request or task schema changes.
- Wiring this unit into the real materializer's request/task processing path
  (`tools/clang22/materialize_main.cpp`, `provider_worker.cpp`). It exists
  today only as a standalone, independently reviewed and tested library
  component.
- Installed/relocated qualification: the full negative matrix against real
  multi-file projects, memory/SQLite publication parity, and release
  qualification evidence.
- A resolution of whether the `member-missing` narrowing described above
  should someday be paired with a distinct signal for "closure was
  incomplete" versus "the source has an ordinary compile error", should a
  future consumer need to tell those apart. Today's callers do not.

## Rejected alternatives

1. **Stage a physical checkout and pass its directory to Clang.** Rejected:
   directory contents, symlinks, races, case behavior, and unrecorded host
   files could affect parsing without changing task identity.
2. **Inline every project/generated file directly into the existing v2.1
   task object.** Rejected: silently changes the strict request shape,
   canonical digest projection, maximum input accounting, worker
   negotiation, and replay/report semantics of an already-shipped contract.
3. **A provider-specific opaque application field.** Rejected for the
   reference materializer: hides path, digest, bound, and capability
   semantics from installed qualification checkers.
4. **A single combined "closure record has no unclaimed-miss distinction"
   design** (the rejected candidate's actual behavior before this rework):
   any filesystem miss beneath the synthetic project root, claimed or not,
   was a hard task failure. Rejected because it made the includer-directory-
   first quoted-include rule, ordinary multi-`-I` search order, and
   `__has_include` -- none of which are closure-completeness questions --
   indistinguishable from a genuinely missing input, making any conventional
   multi-directory project unmaterializable. Empirically confirmed against a
   real `clang++-22` build during independent review.

## Consequences

- A real, non-self-contained project whose main source includes project
  headers via a conventional `include/` + `src/` layout can now be
  materialized through this unit in isolation, once request/task wiring
  (a future ADR's scope) exists to feed it.
- The closure and VFS layers give a future wire-format ADR a validated,
  independently-reviewed foundation to build on rather than a fresh
  from-scratch security design.
- `source-closure.member-missing` is a narrower signal than "the closure did
  not fully satisfy the source": it fires only for a path the manifest
  itself claims as a member. Any future consumer that needs to distinguish
  "you forgot to include a header in the closure" from "the source has a
  syntax error" needs a different mechanism than this error code; today no
  consumer needs that distinction, so none is added speculatively.
- The testing-only member-withholding seam
  (`with_source_closure_translation_unit_withholding_member`, guarded by
  `CXXLENS_CLANG22_SOURCE_CLOSURE_TESTING`) is compiled into every configure
  preset this repository currently ships, including `install-check`, because
  no preset sets `BUILD_TESTING=OFF`. It is not reachable today: its
  declaration lives in a source-private header never installed under
  `include/cxxlens/`, and no production call site invokes it. Before this
  unit is wired into production, its guard should be hardened (explicit
  hidden symbol visibility, or a genuinely test-only translation unit)
  rather than continuing to rely on a `BUILD_TESTING=OFF` boundary that does
  not exist in this repository's actual build practice.

## Verification

- `tests/adapter/clang22/source_closure_native_test.cpp` (13 scenarios
  covering realistic multi-directory project layouts, incomplete-closure
  diagnosis, the claimed-but-unservable hard-failure invariant including
  when Clang itself tolerates the absence, and the ambient-read barrier
  under an absolute include of a file that genuinely exists on disk,
  relative traversal, and the synthetic-root prefix boundary), plus their
  own separate coverage in `source_closure_test.cpp` (digest/order-
  independence and blob-tamper detection), `source_closure_vfs_test.cpp`
  (logical-path traversal, case-collision, and shadow-file resolution), and
  `source_closure_invocation_test.cpp` (argument rewriting, response-file
  and `-ivfsoverlay` rejection) -- all pass against a real local LLVM 22.1.0
  / Clang 22 build.
- Two independent adversarial review rounds against that same real build.
  Round 1 (on commit `c691fbf`) found and required fixing a genuine blocking
  defect: the fail-closed audit fired on Clang's own ordinary speculative
  probing inside the project root, confirmed empirically by compiling real
  multi-directory closures with `clang++-22` and observing spurious
  `member-missing` failures, and by reverting the fix and reproducing the
  same failure. (A separate, earlier `strace` trace was used only to verify
  the *outside*-project-root half of this fix, in the round of work that
  produced `c691fbf` itself -- not part of round 1's own discovery of the
  inside-project-root gap.) Round 2 (on commit `7cb7f98`) re-verified the
  fix -- reverting it again reproduced the same failure -- and found no
  further functional defect, only a documentation correction (the testing
  seam's build-gating claim, addressed in commit `232a76d`).
- Full `clang22`-labeled ctest sweep (28 tests) passes on exact `main`
  `e3fe17e`.

## Acceptance gate

This ADR, DF-0261's record update in
`docs/development/implementation-learning/records/df-0261-source-closure-vfs.md`,
and an independent review binding this exact revision were required to all
be accepted together before DF-0261's `status`/`implementation_disposition`
frontmatter could change from `proposed`/`blocked`. `check_ng_design_feedback.py`
enforces this mechanically for `impact: security` records: `status: accepted`
requires (a) a `resolution_refs` entry resolving to an accepted ADR, (b) a
`review.refs` entry that is specifically a
`https://github.com/horiyamayoh/cxxlens/issues/261#issuecomment-<N>` URL (a
local file reference does not satisfy this particular check, even though it
is otherwise a valid `review.refs` entry), and (c) `review.author` and
`review.reviewer` that differ case-insensitively, so the record cannot cite
itself as its own independent review.

That review pass completed 2026-08-19
(<https://github.com/horiyamayoh/cxxlens/issues/261#issuecomment-5346070745>),
independently re-deriving every concrete claim in this ADR from the current
source rather than trusting its prose, and found the document accurate
(three minor wording issues, none of which misstated code behavior, fixed in
the same revision this ADR was accepted at). This ADR is accepted **only**
for what it actually defines -- closure identity and the read-only compiler
VFS (units 1 and 3 of DF-0261's four-unit plan). It authorizes nothing beyond
what is already independently reviewed and merged to `main`; it does not
authorize wire-format work (unit 2), production wiring, or any qualification
claim (unit 4).
