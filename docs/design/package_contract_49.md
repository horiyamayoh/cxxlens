# Issue #49 generation・mock・method harness・copy・fuzz Contract Candidate

## Authority and scope

This document and `schemas/cxxlens_package_contract_candidates.yaml#issue-49` are normative for the
15 APIs in `generate`, `mock`, `method_harness`, `copy`, and `fuzz`. They remain unimplemented and
blocked. #49 defines contracts only; #52 owns high-risk spikes, #53 installed-header integration,
and #54 freezing.

## Shared header and ownership

The candidate `<cxxlens/generate.hpp>` is LLVM-free. `generate` owns immutable surface census,
decision, artifact, plan, and result values. Nested `generate::mock`, `generate::method_harness`,
`generate::copy`, and `generate::fuzz` own only their request/options and decision domains. Stable
`surface_id`, `artifact_id`, `symbol_id`, `plan_id`, evidence, coverage, path, workspace snapshot,
and execution context remain core/facts/workspace-owned values.

Every immutable builder and plan is safe for concurrent const reads. Workspace planning acquires one
immutable catalog/fact snapshot and never retains AST pointers. Callback exceptions are isolated as
typed failure rows; cancellation/deadline/memory/output budgets remain distinct from a complete
empty result.

## Census, decisions, identity, and accounting

Relevant surfaces include ordinary/static/virtual/pure methods, conversions/operators, constructors,
destructor/assignment, templates, inline/defaulted/deleted/implicit members, static data, nested and
using-exposed declarations, C functions, inaccessible/unspellable/macro-derived and unknown rows.
Every relevant surface occurs exactly once. Surface identity hashes semantic owner, canonical
signature/type identity, role, source identity, and verified variants; it excludes display spelling,
absolute root, worker order, time, and addresses.

Each generation kind declares required decision axes. Every surface has a row per axis with requested,
accepted, excluded, unsupported, ambiguous, failed, or deferred state, stable code, evidence, and any
payload/artifact references. Accepted generation requires a typed payload and existing artifact row.
Unknown axes, dangling references, same-ID unequal payloads, or coverage that does not conserve the
requested universe invalidate the plan. No emitter, report, manifest, or CMake projection may
recompute a semantic decision.

Artifact ID hashes kind, canonical root-relative case-sensitive path identity, payload/content digest,
dependencies, satisfied surface IDs, verified variants, schema/semantics versions, and ownership
marker policy. Artifacts sort by path, kind, and full ID; dependencies and provenance sort/unique.
Equivalent rows deduplicate, while case-fold, parent/child lifecycle, same-path unequal-content, and
variant collisions are explicit conflicts. `present`, `publishable`, `usable`, `link_ready`, `listed`,
and `quarantined` are independent facts and are never inferred from file existence alone.

## Preview, validation, apply, and #48 reuse

Preview is a side-effect-free pure projection of validated artifact bytes with UTF-8/LF, canonical
record order, explicit encoding/newline and record-boundary truncation. A truncated preview does not
truncate the authoritative plan. Generated C++ uses typed internal TypeIR/NameIR/CodeDOM; pretty-type
or ad-hoc string concatenation is not semantic identity.

Apply defaults to `transform::apply_mode::dry_run`. Generation owns artifact lifecycle and
`generation_result`, but reuses #48's one-writer transaction service, authorization, root/symlink/
case-fold safety, source/model/config/template/toolchain stale checks, formatter/reparse validation,
prepare-all/commit-all, reverse rollback, and recovery artifacts. It does not create a second writer.
Artifact create/update/delete is not falsely represented as a source-range edit; both immutable plan
types lower into the shared transaction writer through distinct validated adapters.

Manual edits or missing ownership markers reject overwrite unless an exact explicit digest policy is
present. Silent rebase, first-wins collision, partial commit, orphan deletion without owned marker,
and best-effort publication are forbidden. Any format/reparse/compile/link failure quarantines the
artifact and prevents publishable/usable/link-ready claims. Rollback failure returns the shared typed
terminal state and secure recovery paths, never success.

## Mock and fake decisions

Targets resolve by semantic class/symbol/header identity; zero, one, and many candidates remain
distinct. Include/exclude method selectors are normalized immutable predicates. Explicit exclusion
precedes inclusion, which precedes options defaults; overlap is reported, not first-wins. Empty
selection is successful only with complete census and coverage.

Every surface records mock, fake-definition, fake-dispatch, and link-effect axes. Signature payloads
preserve return/parameter TypeIR, cv/ref/noexcept, calling convention, attributes, templates,
namespace, overload/operator identity, access and origin. Unsupported inheritance, ABI-sensitive,
macro-derived, incomplete, inaccessible, or unspellable surfaces remain accounted for. Framework
output is a pure payload projection and introduces no runtime dependency into the cxxlens library.

## Method specification and harness

`parse_method_spec` has a versioned grammar for qualified class/method names, canonical parameter and
optional return types, cv/ref/noexcept and template arguments. Escaping is deterministic; errors carry
stable code and UTF-8 byte offset; parse-normalize-serialize-parse round trips. Pretty type strings are
not equality—the resolver compares canonical type/symbol facts.

Resolution returns zero/one/many candidates with evidence, unresolved, and coverage and never chooses
the first overload. Inherited/using/implicit/template/dependent members are explicit. Inspection
classifies field/parameter/local/helper/virtual/global/allocation/I/O/synchronization/exception/
this-escape/lifetime/byte-operation features as kernel-safe, live-required, unsupported, or diagnostic.
Planning emits self/shape/port/kernel/scenario/live-reference/manifest artifacts only from the
inspection rows. Unknown dynamic type, raw-this escape, ODR/macro/variant risk cannot fall back to an
unsafe fake-this harness.

## Semantic copy and fuzz harness

Copy targets a semantic symbol and computes exported/reachable declarations plus a transitive type
closure including template/default arguments, bases, members, aliases, inline/constexpr bodies,
attributes, visibility, license/source provenance and forward-declaration eligibility. Namespace
projection promises source shape only, never ABI compatibility. Cycles are represented; private,
macro, ODR, extension, unspellable, and variant-divergent dependencies are rejected or unresolved.

Fuzz target resolution preserves overload/variadic/template/member eligibility. Explicit input models
override finite inference rules. Pointer-length, ownership/nullability/range/encoding, enum/object
invariants, non-trivial construction, global state, exception and side effects remain risk disclosures;
unknown relationships are unsupported, never guessed. Engine/entrypoint/corpus/dictionary/sanitizer
and resource budgets are deterministic plan inputs. Inference promises byte decoding mechanics, not
semantic validity or safety.

## Shared dependencies and #52 spike

- #44 owns snapshots, semantic facts, variants, filesystem/process ports and provenance; #45 owns
  selectors; #48 owns transaction/stale/path/format/reparse machinery; #50 supplies optional versioned
  model facts without becoming an implicit requirement.
- #52 must exercise exhaustive C++ surfaces, overload/cvref method resolution, macro/ODR/private/type
  closure, path/case/symlink collisions, manual edits/stale templates, formatter/frontend/compiler/link
  failure, multi-file rollback and rollback failure, orphan policy, fuzz pointer-length ambiguity, and
  jobs/root/cache/variant deterministic bytes.

Every API record has exact declaration/fingerprint, positive/negative/ambiguous acceptance, ownership,
provider/schema dependencies, and Doxygen obligations.
