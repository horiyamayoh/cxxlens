# Issue #50 flow・models package Contract Candidate

## Authority and scope

This document and `schemas/cxxlens_package_contract_candidates.yaml#issue-50` are normative for the
12 APIs in `flow` and `models`. All remain unimplemented/blocked. #50 defines contracts only; #52
owns high-risk feasibility/cost spikes, #53 public-header integration, and #54 freezing.

## Ownership, values, and availability

`models` owns versioned API effect/replacement/source/sink/barrier pack rows. `flow` owns the public
source/sink/barrier values, CFG availability, paths, taint/resource policies and reports, and effect
summary execution. #46's semantic graph IDs/edges are not CFG nodes. Shared symbol/type/call/CFG facts,
snapshot/variant/provenance and provider ports remain #44-owned.

Every policy/model/pack/report is detached from AST lifetime and immutable; builders return new values.
Const observers permit concurrent reads. Planning/execution binds one immutable workspace/catalog/fact/
model snapshot. No callback or provider exception crosses the API; diagnostics, unresolved, coverage,
guarantee, cancellation/deadline and budget state remain typed.

`flow.cfg` and `flow.effect-summaries` are explicit versioned capabilities with unique providers.
`inspect_cfg` distinguishes available, absent, unsupported, failed, partial, stale and variant-divergent.
It accounts for missing body, template/dependent body, macro/coroutine/exception/indirect-call limits and
never turns unavailable CFG into an empty successful analysis. Run APIs repeat the availability check.

## Model identity, value paths, and conflict

Source/sink/barrier identity hashes namespace/version, normalized selector, argument/receiver/return/
field/global role, value path, category/label/priority, barrier effect, context/variant restrictions and
provenance. Display prose, absolute root, address, observation/registration order and diagnostic text are
excluded. Value paths distinguish whole value, field, element, pointee and bounded alias; unsupported
path sensitivity is unresolved rather than silently collapsed.

Exact canonical symbol/type facts bind function/method models. Overload, template, inherited/virtual,
indirect/dependent and variant targets preserve zero/one/many state. Equal ID/equal payload rows merge
sorted provenance. Same ID/unequal payload conflicts fail validation unless merge has explicit left/right
precedence; built-in < project < user precedence is never implicit. Unknown target/model absence cannot
be an empty success.

Barrier `kill`, `transform`, `conditional`, and `partial` require dominating control-flow evidence.
A validation/sanitizer call elsewhere in the function is not a barrier. Conditional/partial effects
preserve both states and downgrade guarantees.

## Taint lattice, paths, budgets, and execution

`taint_policy` owns a finite normalized label lattice, source/sink/barrier sets and explicit propagation
rules. Default propagation covers declared assignment/call/return/field rules only. Alias/container/
implicit-flow behavior is requested sensitivity and provider-dependent; unsupported precision remains
unresolved. Unknown external calls use explicit preserve-unresolved, conservative-propagation or reject
policy. Contradictory/empty invalid policies fail before execution.

Path/state/context/step/time/memory/output budgets are positive finite inputs. Worklists, SCC order,
joins, widening and reduction use canonical IDs independent of jobs/root/cache/enumeration. Multi-label
join is commutative/associative/idempotent. Recursion reaches a typed fixpoint or returns non-convergent;
budget exhaustion retains valid rows, coverage and downgraded guarantee.

Path ID hashes policy/model/provider versions, source/sink semantic identities, ordered semantic steps,
barrier/label transitions, call context and variant. Equivalent paths deduplicate with provenance;
representative-path limiting occurs after stable order and records omitted count. Findings derive from
authoritative paths. Partial CFG/model/provider failure cannot claim complete flow absence.

## Resource protocol and effect summaries

Resource protocols declare resource, initial/terminal/error states and acquire/release/transfer/borrow/
use/escape/invalidate/error transitions. Object identity follows semantic allocation/handle plus bounded
alias/copy/move provenance. RAII destruction, exceptions, early returns and interprocedural summaries are
explicit edges. Unknown external effects, conflicting transitions and variant divergence remain unresolved.

Leak, double release, use-after-release and invalid transition are distinct finding kinds with stable
counterexample path IDs. Missing CFG/alias/summary facts are not findings and not clean success. Multiple
resources and nested protocols have separate state products under finite budgets.

Effect summary ID hashes callable semantic identity/signature, model pack ID/version, context abstraction,
variant and summary semantics version. The minimum domain covers reads/writes, allocate/free, throws,
taint/resource and calls. Recursive SCCs use deterministic fixpoint/widening. Unknown call/body, invalidated
dependency, oversized summary and non-convergence remain typed. Cache compatibility requires exact domain,
provider, model and dependency digests.

## API model pack I/O and compatibility

Pack ID is a lowercase namespaced identifier and version is semantic. `empty` is valid but contains zero
rows. Builder rows use normalized canonical target keys and return `result`; equal duplicates are
idempotent, conflicting duplicates fail. Replacement records call semantics only—it neither owns #48 edits
nor #49 artifact generation.

Merge is atomic and deterministic. Default rejects unequal rows; explicit prefer-left/right retains both
provenances and conflict decision evidence. Canonical JSON/YAML is UTF-8/LF and sorted by kind/key/full ID.
Load/save use filesystem ports, root/path/size/deadline policy, never shell. Untrusted user packs are fully
schema/semantic validated. Unknown kind/major version rejects; explicitly forward-compatible fields may be
preserved; migrations require a declared source/target path and cannot silently change semantics.

## Shared boundaries and #52 spike

- #45 supplies normalized selectors, #46 may supply semantic call/SCC topology but does not own CFG, #49
  reads versioned models for generation decisions, and review/report consume authoritative rows only.
- #52 must spike CFG missing/partial/coroutine/exception/indirect/template cases, label joins and barriers,
  recursive SCC convergence/widening/budget, alias/RAII/exception resource paths, model conflict/precedence/
  unknown-version/untrusted/size cases, and jobs/root/cache/variant determinism/cost.

Every API record has exact declaration/fingerprint, positive/negative/ambiguous acceptance, typed ownership,
provider/schema dependencies, and Doxygen obligations.

## Issue #52 validation backlink

Validated by `docs/design/high_risk_contract_validation.md#decisions`: seven CFG availability states,
recursive fixpoint/non-convergence, representative taint path and resource use-after-release counterexample
were bounded and dependency-acyclic. Fingerprint `sha256:2cbc4efec8cc4784fe10df0d4ac5048d943b73ff5d3d677afc6991d7c7a569ad` was unchanged.
