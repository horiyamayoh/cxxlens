# Issue #48 transform package Contract Candidate

## Authority and scope

This document and `schemas/cxxlens_package_contract_candidates.yaml#issue-48` are normative for all
nine transform APIs. All remain unimplemented/blocked. #48 does not implement mutation. #52 owns
high-risk spikes, #53 header integration, and #54 freezing.

## Plan identity and edit model

Every operation produces an immutable `edit_plan`; planning and preview never write. Plan ID hashes
semantics/schema version, workspace catalog/fact snapshot, normalized operation/selector/options,
and canonical edit IDs. Edit ID hashes kind, canonical workspace-relative file identity, half-open
byte range, replacement bytes, base digest, variants, origin, and atomic group. Display path,
message, worker order, or absolute root is excluded.

Each edit owns its source digest, file identity, catalog version, fact snapshot, directly-spelled
requirement, all verified variants, evidence, and reason. File order is canonical semantic path;
edits order by file, begin, end, kind, full ID. Same-ID equal rows merge provenance. Same range/same
replacement deduplicates; overlap, insert-order ambiguity, lifecycle collision, macro-origin
conflict, or variant divergence is an explicit `edit_conflict` and invalidates the plan. Adjacent
edits remain separate. No offset rebasing or first-wins conflict resolver exists.

Complete zero edits is success only for complete coverage and a uniquely resolved zero-match.
Ambiguous target, unsupported construct, missing facts/capability, partial coverage, or budget leaves
typed unresolved and a non-applicable plan. Valid rows are not omitted.

## Operation and source safety

Call rewrites resolve semantic overload/receiver/argument identity; `replacement_template` has typed
argument placeholders and cannot evaluate arbitrary code. Rename expands declaration/reference,
overload and override families according to explicit facts. Include changes preserve spelling,
conditional region, source order, duplicates, forward declarations, and module boundaries.

Expansion-only macro locations, token paste/stringize, unsafe body/definition, non-token boundary,
invalid UTF-8/newline splits, generated/system/read-only sources, symlink/path traversal/root escape,
and case-fold/rename races reject before a plan becomes valid. Argument spelling or declared
definition edits require explicit policy and retain downgraded evidence/guarantee. Every relevant
variant must agree; selected-only or unavailable variants remain explicit and cannot claim a safe
all-variant edit.

## Formatting, overlay reparse, and preview

Pipeline order is semantic selection, safety precheck, immutable edits/digests, conflict validation,
include edits, formatter on changed ranges/files, conflict/digest recheck, overlay-VFS reparse of all
required affected variants, diagnostic delta validation, then final plan. Formatter identity,
version, config digest, argv structure, cancellation/deadline/output limits, and frontend compile
snapshot are recorded. Commands are structured arguments through ports, never shell concatenation.

Unavailable/crashed/timed-out formatter or frontend, partial reparse, a new diagnostic at/above the
threshold, or variant-specific failure prevents write eligibility. Pre-existing diagnostics remain
separate. Preview is a pure projection of the final formatted/reparsed candidate, UTF-8/LF, canonical
path/file order, fixed context, and record-boundary output budget. Truncation is explicit and never
means a partial plan.

## Apply and fail-closed transaction

`apply_options.mode` defaults to `dry_run`. Dry-run reruns every authorization/path/digest/file
identity/catalog/snapshot/conflict/variant/macro/formatter/reparse precondition against an overlay
and returns `dry_run_validated` without filesystem writes.

Write mode acquires one workspace writer lock and rechecks all touched files before any commit.
All temporary files are created through filesystem ports inside the authorized root with restricted
permissions; metadata is preserved deliberately. Every file reaches prepared state before atomic
rename/commit. Partial commit is forbidden. Any failure/cancel performs reverse canonical rollback.
Post-write validation failure also rolls back. Rollback failure yields `rollback_failed` plus secure
recovery artifacts and never success. Double apply is stale/already-applied, requiring explicit
replan; no silent rebase/retry occurs.

`apply_result` owns overall and per-file states, transaction ID, before/after digests, diagnostics,
unresolved, coverage, and recovery artifacts. Success means every intended file committed and every
required postcondition passed. `committed` is false for dry-run, prepared, rolled-back, and recovery
states.

## Shared ownership and #52 spike

- `transform` owns edit/plan/conflict/apply/transaction schemas, edit planner, conflict detector,
  formatter orchestration, overlay reparse validator, and transaction writer.
- #44 workspace/facts own snapshots, variants and ports; #45 selectors/search own target identity;
  #43 owns evidence/coverage/diagnostics; #47 report/testing consume plans read-only.
- #49 generation owns census/decisions/artifacts but must reuse #48 edit rows, transaction writer,
  stale/path/conflict/reparse rules for source artifacts; it cannot implement a second writer.
- #52 must spike multi-file prepare/commit/rollback and injected rollback failure, stale race after
  prepare, symlink/root escape, variant contradictions, macro argument/definition/tok-paste cases,
  formatter crash/timeout, partial reparse, diagnostic delta, double apply, and jobs/root/cache
  deterministic plan/diff bytes.

Every API record includes positive, negative, and ambiguous acceptance, exact signature, owner/
provider/schema dependencies, and Doxygen obligations.

## Issue #52 validation backlink

Validated by `docs/design/high_risk_contract_validation.md#decisions`: stale/overlap rejection, default dry-run,
two-file mid-write rollback and explicit rollback-failure recovery were reproduced. Fingerprint
`sha256:7f18fade87f89da2014f84a0dc9725e6225f67cca3c044bebd5c44f557158038` was unchanged.
