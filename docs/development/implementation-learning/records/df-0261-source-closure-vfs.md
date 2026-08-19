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

2026-08-18: Independent review of the candidate implementation staged on
`agent/issue-261-source-closure-vfs-implementation` (PR #353; ADR 0101 and the
`cxxlens.clang22-source-closure-contract.v1` schemas). Rebased cleanly onto
exact `main` `f3b02df3507ec85c689ae11c379710617ed3af60` (the one conflicting
file, `materialization_public_report.cpp`, had the same use-after-move fix
applied independently on both sides; main's version was kept verbatim).
**Verdict: not accepted — two blocking findings, implementation is not
qualification-ready.**

1. `source_closure_native.cpp`'s VFS denies any filesystem touch outside the
   closure and outside a fixed `qualified_read_roots` allowlist. A real Clang
   driver invocation performs speculative, distro/toolchain-dependent probes
   (e.g. `/etc/os-release`, GCC-installation candidate directories) that Clang
   itself tolerates on ENOENT but that this design hard-fails the whole task
   on. No static allowlist can enumerate Clang's full candidate set; this is
   why `adapter.clang22-source-closure-native` fails today. The fail-closed
   policy needs a way to distinguish genuinely-required closure input from
   Clang's own optional/speculative probing, which is exactly the kind of
   security-relevant rule this record already flags as needing accepted
   authority (see "Working mental model" / symlink-policy note above) — not
   something to decide unilaterally while reviewing.
2. `with_source_closure_translation_unit` only consults
   `missing_failure()` (an absent project/generated closure member — the
   record's core invariant: a missing member is a determinate failure, never
   an ambient fallback) inside the `!outcome` branch, i.e. only when Clang's
   own run already reports failure. `policy_failure()` (ambient/toolchain
   denial) is checked unconditionally and does fail closed correctly. Any
   construct where Clang tolerates a missing header without erroring
   (`__has_include`, conditional generated-file probing) could let extraction
   proceed against an incomplete closure undetected. The shipped test passes
   only because its one negative case happens to make Clang itself fail too;
   nothing in the code ties the two signals together.
3. Structural: `compiler_vfs.*`, `clang_compiler_vfs.*`, `provider_task_v4*.*`,
   and `materialization_request_v2_2.*` (~10 files) are an orphaned earlier
   design iteration — they reference a type vocabulary that no longer exists
   in `source_closure.hpp` and fail `-fsyntax-only`. They are not referenced
   by the root `CMakeLists.txt`/`tests/CMakeLists.txt`, only by the
   branch-local `tests/issue261{,-worker}/CMakeLists.txt` that the PR
   description itself says will be removed before merge. These must be
   deleted, not merged.

What does hold up under review: `source_closure.{hpp,cpp}` (closure
value/identity — exact-segment traversal rejection, NFC+casefold collision
detection, order-independent digest, all inspected and exercised) and
`source_closure_vfs.{hpp,cpp}` (logical-path mapping/include resolution).
These are a sound basis for unit 1/3 once findings 1-2 above have an accepted
resolution. Unit 2 (bounded transfer/capability contract, task.v4) has no
working implementation — only the dead code in finding 3. Unit 4
(qualification) is premature until units 2-3 are functionally complete.

Remains `proposed / blocked`. Next increment: accept a policy for
distinguishing mandatory closure members from Clang's optional/speculative
filesystem probes (finding 1), make `missing_failure()` enforcement
unconditional and prove it with a test where Clang itself does not also fail
(finding 2), and delete the dead task.v4/compiler_vfs/request-v2.2 files
before any further qualification work.

2026-08-19: Follow-up implementation addressing both blocking findings from
the 2026-08-18 review, on branch `agent/issue-261-source-closure-vfs-v2`
(built on current `main`). This entry describes an implementation increment,
not a review verdict; the independent review this record already requires
still has to happen before any acceptance boundary moves.

Carried `source_closure.{hpp,cpp}` and `source_closure_vfs.{hpp,cpp}` over
from `agent/issue-261-source-closure-vfs-implementation` (PR #353, tip
`7a0717b`) byte-for-byte unchanged, plus the small `unicode_nfc.{hpp,cpp}`
refactor that adds `nfc_casefold_utf8` (used by the case-collision check in
`source_closure.cpp`). Both were already found sound by the prior review and
remain so; nothing in either file changed. `source_closure_invocation.{hpp,cpp}`
(the `qualified_read_roots` admitted-toolchain-root contract) was also
carried over unchanged; it was already the correct single source of truth for
finding 1's fix, not something needing new authority. `source_closure_native.
{hpp,cpp}` (the Clang-facing bridge) was rewritten to fix both findings. The
~10 dead-code files from finding 3 (`compiler_vfs.*`, `clang_compiler_vfs.*`,
`provider_task_v4*.*`, `materialization_request_v2_2.*`) were not carried
over at all -- `git grep` over `src/` and `tests/` on the new branch confirms
nothing references them.

Finding 1 fix (fail-closed VFS vs. Clang's own speculative toolchain
probing): `closure_routing_file_system` still classifies every path into
exactly the same three regions as before -- the closure's synthetic project
root, the admitted qualified toolchain root(s), or neither -- reusing
`source_closure_invocation.cpp`'s existing `qualified_read_roots` contract as
the one source of truth for "the admitted toolchain" rather than inventing a
second one. What changed is the "neither" case: it used to record a
`policy_failure` that unconditionally aborted the whole task; it now answers
with a plain `ENOENT` (`std::errc::no_such_file_or_directory`) to whatever
asked, and touches no audit state at all. Real ambient content is
structurally unreachable through that route (the real filesystem is never
consulted for a denied path), so the fail-closed guarantee against ambient
bytes holds by construction, not by an audit-and-abort step that can be
bypassed or mis-ordered the way finding 2 showed the old audit-based approach
could be. Reproduced the underlying problem concretely first: `strace -f -e
trace=openat,newfstatat,access` on a bare `clang++-22 -std=c++23 -nostdinc
-nostdinc++ --gcc-toolchain=/usr -resource-dir=<resource-dir> -c ...`
invocation on the verification host shows the driver touching, among others,
`/etc/os-release`, `/etc/lsb-release`, `/opt/rh`, and `/etc/env.d/gcc` -- all
outside a `{"/usr","/lib","/lib64"}`-style allowlist -- purely as ordinary
GCC-installation-candidate and distro-detection probing that Clang itself
tolerates gracefully on ENOENT. The new `adapter.clang22-source-closure-native`
test (scenario 4) admits *only* the exact LLVM/Clang 22 install root the
adapter is itself linked against (resolved from `find_package(LLVM 22.1
CONFIG)` by `tests/CMakeLists.txt`) as the qualified root -- no `/usr`, no
`/etc`, nothing else ambient on the host -- and runs a real `clang++-22` with
no `--gcc-toolchain` pinned, so the driver's own candidate search runs
completely unconstrained. Temporarily reverting just this fix (restoring the
old `deny()`/`policy_failure()` behavior while keeping everything else as-is)
reproduces the failure concretely: the task now aborts with
`source-closure.toolchain-input-unqualified / compiler-vfs / /opt/rh` on the
very first, ordinary-success scenario, against this host's real toolchain
layout. With the fix restored, all five scenarios in that test pass.

Finding 2 fix (asymmetric fail-closed enforcement): the closure-completeness
signal (`missing_failure()`, simplified to `missing_path()` now that
`policy_failure()` no longer exists to be asymmetric with) is checked
unconditionally in `with_source_closure_translation_unit`, immediately after
the Clang run returns and regardless of what `outcome` says, rather than only
inside a `!outcome` branch. A new scenario 3 in the same test exercises this
directly: the closure omits a header that `main.cpp` only probes through `#if
__has_include("optional_generated.hpp")`, a construct Clang tolerates without
ever diagnosing an error. The test asserts `callback_ran` is `true` (proving
Clang's own run independently reached `HandleTranslationUnit`, i.e. succeeded
on its own terms) *and* that the overall result is still
`source-closure.member-missing` -- the two signals are a genuine independent
variable here, not coupled by construction the way the previously shipped
negative case was. Reverting just the unconditional-check fix (gating the
same check back inside `if (!outcome)`) reproduces the exact failure this
finding described: the scenario reports a closure-incomplete `__has_include`
probe parsing as successful. The original fatal-`#include` negative case
(scenario 2) is kept as a baseline regression, but its old `!callback_ran`
assertion was dropped: on this Clang 22 build, `HandleTranslationUnit` was
observed to still run even after a fatal preprocessor "file not found" error
(the AST consumer still gets invoked with whatever partially-parsed AST
exists), so neither Clang's return code nor whether the callback fired is a
reliable signal for closure completeness in general -- reinforcing that the
explicit, unconditional `missing_failure()` check is the only thing that may
be relied on, which is exactly what finding 2 was about.

Also wired the sound units into the real build for the first time:
`source_closure.cpp`, `source_closure_invocation.cpp`, and
`source_closure_vfs.cpp` were added to `cxxlens_clang22_materialization_codecs`
(top-level `CMakeLists.txt`) and its private sealed-worker mirror
`cxxlens_clang22_worker_codecs_internal` (`cmake/CxxlensClangTargets.cmake`,
required so the sealed `cxxlens-clang-worker-22` closure keeps resolving the
symbols `source_closure_native.cpp` needs); `source_closure_native.cpp` was
added to `cxxlens_clang22_worker_core` alongside `provider_worker.cpp`. Four
tests are wired into `tests/CMakeLists.txt`: `adapter.clang22-source-closure`,
`adapter.clang22-source-closure-vfs`, and
`adapter.clang22-source-closure-invocation` (unchanged from the reviewed
branch), and `adapter.clang22-source-closure-native` (rewritten; five
scenarios covering ordinary success, a fatal missing include, the finding-2
asymmetry, the finding-1 ambient-probing case, and an ambient-shadow-file
regression). The native test resolves its compiler binary, resource
directory, and admitted toolchain root from the same `find_package(LLVM 22.1
CONFIG)` result the real adapter links against, rather than an assumed host
path such as `/usr/bin/clang++-22`.

Verification: `CXX=clang++-22 cmake --preset dev-clang -DLLVM_DIR=<llvm>/lib/
cmake/llvm -DClang_DIR=<llvm>/lib/cmake/clang` against a real local LLVM/Clang
22.1.0 install, a full `cmake --build build/dev-clang` (206 build steps, no
errors, including the sealed `cxxlens-clang-worker-22` closure), and `ctest`
for all 28 `clang22`-labeled tests plus the four new/changed ones run
individually -- all pass. `quality.ownership`, `quality.ng-ci_supply_chain`,
and `quality.ng-foundation_completion` (asset migration ledger and
design-package checksum inventory regenerated for the new files) pass.

This disposition still does not accept a source-closure identity, request
version, wire feature, reuse/cache rule, production-facing compiler VFS
wiring, or production qualification -- unit 2 (bounded transfer/capability
contract) remains entirely unbuilt, and this unit is not wired into the real
materializer's request/task processing path. The tracking issue #261 remains
open and `implementation_disposition` remains `blocked`; this entry does not
change that field. An independent reviewer is required before it can change.

2026-08-19 (second increment): Independent security review of the increment
above (commit `c691fbf`) rejected it with two blocking findings. The review
confirmed the ambient-read barrier itself holds under adversarial probing and
that the closure identity/traversal/digest layer is intact, but found that the
finding-1 fix had been applied only *outside* the synthetic project root while
the identical conflation survived inside it. This entry records the repair.

Blocking finding A -- the missing-member audit fired on any absent path under
the synthetic root, not only on members the manifest actually claims.
`closure_routing_file_system::classify()` routed purely by prefix, so every
miss beneath `/__cxxlens_project__` was recorded and converted into
`source-closure.member-missing`. The code comments justified this by asserting
the manifest "already claims this member exists", which was simply false: the
manifest claims things only about `closure.members`, and nothing compared the
probed path against that set. Clang probes non-member paths inside the project
namespace constantly, so the reviewer was able to show -- against real
`clang++-22`, with a complete closure that Clang compiled successfully -- that
all of the following failed the task spuriously: the includer-directory-first
rule for a quoted `#include` (`project://src/answer.hpp` probed before the
`-I` entry that holds the member), the same under `-iquote`, ordinary
multi-`-I` search order, an `-I` naming a member-less directory, and
`__has_include` in project code. The practical effect was that any project
using the conventional `include/` + `src/` split -- precisely the "ordinary
project whose main source includes a project header" this record exists to
enable -- could not be materialized at all. This was fail-closed rather than a
bypass, but it defeated the unit's purpose and made the
`source-closure.member-missing` signal mean something other than its name,
which is the kind of pressure that eventually gets a real signal relaxed.

The fix makes the audit member-aware. `mount_native_vfs` now records the exact
synthetic path of every closure member alongside the in-memory content, and a
miss beneath the synthetic root consults that set: a claimed member that could
not be served still records the determinate failure, while every other miss
returns the same plain ENOENT the outside-root case already returned. The
consequence worth stating plainly is that an *incomplete* closure is no longer
reported as `member-missing` at all -- Clang emits its own ordinary
file-not-found diagnostic and the task fails through `native.parse-failed`.
`member-missing` is now reserved strictly for claimed-but-unservable members,
which, given a validated closure and a successful mount, is a defence-in-depth
invariant rather than a reachable state. Because that makes it unreachable
through the public entry point, a testing-only withholding seam
(`with_source_closure_translation_unit_withholding_member`, guarded by
`CXXLENS_CLANG22_SOURCE_CLOSURE_TESTING`, set only under `BUILD_TESTING`
following the existing `CXXLENS_CLANG22_MATERIALIZATION_REPORT_TESTING`
precedent) manufactures the state so the invariant stays under test. Verified
by `nm` that the seam's symbol is present in the `BUILD_TESTING=ON` worker
archive and absent from a `BUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release`
build.

Blocking finding B -- the previous increment's tests encoded the defective
semantics. Its scenarios 2 and 3 both asserted `source-closure.member-missing`
for probes of paths that were never closure members, so a correct
implementation would have failed the suite; and all five scenarios used a
single-directory `src/main.cpp` + `src/answer.hpp` layout, the one layout that
happens to avoid the bug. The suite was reworked rather than extended. It now
has thirteen scenarios in four groups: group A pins that ordinary layouts
materialize (`include/` + `src/` with `-I`, the same with separate-token
`-iquote`, an angle include resolved from the second of two `-I` entries, an
`-I` naming a member-less directory, `__has_include` of an unclaimed path, and
unconstrained driver toolchain probing); group B pins that an incomplete
closure surfaces as `native.parse-failed` and specifically not as
`member-missing`; group C uses the withholding seam to pin that a
claimed-but-unservable member hard-fails both when Clang also fails and --
the finding-2 regression from the first increment -- when Clang itself
succeeds via `__has_include`, with `callback_ran` asserted to prove Clang's
outcome is a genuine independent variable; group D pins the ambient-read
barrier with cases the previous suite lacked, including an absolute `#include`
of a file that genuinely exists on disk outside the admitted roots, relative
traversal (`../../../../etc/passwd`), the `/__cxxlens_project__evil` prefix
boundary, and a strengthened working-directory shadow test that now uses a
`static_assert` on the header's value so compiling ambient bytes would fail
the build rather than pass silently. Both regressions were reproduced against
the real compiler before and after: reverting member-awareness makes scenario
A1 fail with exactly `source-closure.member-missing / project://src/answer.hpp`
on a complete closure, and re-gating the audit inside `!outcome` makes C2 fail.

Medium finding -- callback-before-verdict ordering. Confirmed: the callback
runs inside `runToolOnCodeWithArgs` and has therefore already executed, and
may already have emitted output, on a call that ultimately returns a failure.
This is safe for the current consumer (`provider_worker.cpp` discards its
local batch on `!outcome`) and a successful callback never overrides a failed
verdict, but the header documented the invariant as "reported unconditionally"
without noting the ordering. `source_closure_native.hpp` now carries an
explicit ordering contract: callers must treat all callback output as
provisional until the function returns success.

Also corrected in the same pass: the `qualified_read_roots` documentation
claimed the roots were "the exact toolchain surface the materializer itself
selected and pinned", which nothing in the code enforces -- it now says the
roots are trusted verbatim, that selecting them is the caller's security
responsibility, and that because reads beneath an admitted root are delegated
to the real filesystem (which follows symlinks) an admitted root bounds names
rather than bytes. `source_closure_invocation.hpp`'s claim that unqualified
host paths "fail closed" was likewise too strong: `-F`, `-B`, `-iframework`,
`-iwithprefix{,before}`, `-iwithsysroot`, `-isystem-after`,
`-fcrash-diagnostics-dir=`, and `-Xclang -internal-isystem <dir>` pass through
verbatim (verified by grep that none are in the recognized set), so the
comment now states the recognized set is not exhaustive and that containment
for the rest comes from the VFS denying those paths downstream, not from
argument admission. A `source-closure.blob-missing` construction in
`mount_native_vfs` that put the logical path in the `field` slot with an empty
`detail`, unlike every other call site, was corrected. `working_directory_` is
documented as single-thread-owned (contrasted with the mutex-protected audit),
and `setCurrentWorkingDirectory` is noted to leave it unchanged on a rejected
target rather than half-updated.

Verification for this increment: full `cmake --build build/dev-clang` clean,
including the sealed `cxxlens-clang-worker-22` closure; all 28
`clang22`-labeled ctest tests pass, including the reworked
`adapter.clang22-source-closure-native`; a separate `BUILD_TESTING=OFF`
Release configure/build confirms the testing seam is compiled out. The
specific behavioral claim the review asked to be confirmed holds: a real
`include/` + `src/` project with `-I` now materializes successfully, while a
genuinely-claimed-but-unservable member still hard-fails with
`source-closure.member-missing`.

Still `proposed / blocked`, unchanged by this entry. Unit 2 remains unbuilt,
this unit is still not wired into the materializer's request/task path, and
the semantics change described above (`member-missing` no longer covering
closure incompleteness) is itself a contract decision that the next
independent review should weigh explicitly rather than inherit.
