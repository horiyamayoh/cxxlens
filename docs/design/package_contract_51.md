# Issue #51 review・qa package Contract Candidate

## Authority and scope

This document and `schemas/cxxlens_package_contract_candidates.yaml#issue-51` are normative for the
nine APIs in `review` and `qa`. All remain unimplemented/blocked. #51 defines contracts only; #52 owns
high-risk feasibility/cost spikes, #53 public-header integration, and #54 freezing.

## Snapshot, identity, values, and ordering

Review and QA values are immutable, detached from AST/process lifetime, and bound to explicit repository
root, commit/dirt state, workspace snapshot, configuration, tool/model and schema versions. Const observers
permit concurrent reads. Absolute relocation roots, PID, timestamps, temporary paths, output prose and
enumeration order do not enter authoritative identity. Canonical rows sort by normalized path, coordinate,
semantic finding ID, step ID and association ID; equal duplicates merge provenance and unequal conflicts
remain typed.

## Diff and baseline decision tables

`diff_view` distinguishes base/head commit, merge base, index, worktree and untracked state. Added, deleted,
modified, renamed, copied, binary, submodule, file mode and newline state are retained. `contains` requires
an explicit before/after coordinate and uses spelling source spans; zero-width, macro expansion, truncated,
combined or overlapping hunks never silently match. Git is invoked through the argv-only process port with
root/cwd constraints. Invalid diff, missing Git, drift and process failure are distinct outcomes.

Baseline equivalence hashes stable finding/rule version, semantic symbol/source/variant and evidence class,
excluding diagnostic prose and observation line alone. Results are exact, equivalent, changed, new,
resolved or ambiguous. Rename/moved-line/root relocation requires explicit mapping evidence. Duplicate or
conflicting rows, expiry, stale source fingerprint and undeclared migration remain unresolved or fail load.
Load is bounded/untrusted; save is canonical and atomic through filesystem ports.

## Gate and review workflow

Gate aggregation yields pass, warn, fail or indeterminate with stable reason codes and exit mapping. Missing
or partial coverage, unsupported analysis, budget/cancellation, baseline ambiguity and provider failure can
never become pass. Suppression, baseline and changed-scope decisions retain their independent provenance.
Only a complete evaluation may pass; a gate finding is not a tool failure.

Review stages bind one workspace snapshot and run provisioning, query/rules, baseline classification,
changed-line reporting, affected-context analysis, optional validated fix planning and gate evaluation in
canonical order. A stage/rule/baseline/Git failure retains valid rows, coverage and downgraded guarantee.
Report identity binds commit/diff/tool/config/input fingerprints. Re-running identical inputs is stable.

## QA profile and external process security

QA profiles contain build/test/lint/static-analysis/sanitizer/coverage steps, required/optional state,
dependencies, finite timeout/output/retry/parallel budgets and immutable precedence. External execution is
argv-only. Shell is disabled unless an explicit separately validated policy enables it. Executable identity
and version, cwd under declared root, environment allowlist, redaction, descriptor/network policy, timeout,
cancel, byte limits, exit, signal and crash are recorded. Tool unavailable/version unsupported and test
failure are distinct. Required step failure stops dependent steps; only explicitly optional steps may
continue. Secrets and raw unbounded output never enter reports.

## Coverage import and association

Coverage import validates format/version, profile checksum, binary build ID and normalized source mapping.
Line, branch, function, region, macro/generated source and merged-run counters retain their dimensions.
Stale, mismatched, corrupt, partial, unknown-file and zero-executable-unit cases are explicit and cannot
claim complete coverage. Association IDs bind finding, test, coverage unit, artifact and evidence; the
many-to-many result preserves ambiguous and unmatched rows, confidence and deterministic bounded order.

## Shared boundaries and #52 spike

- `review` owns diff parsing, baseline matching and gate evaluation; #43 owns findings/coverage, #47 owns
  rule/report rendering, #44 owns workspace snapshots, and #45/#46 provide scope/navigation facts.
- `qa` owns profiles, QA orchestration, coverage parsing and association. Runtime owns process, filesystem,
  time and hash ports; QA never concatenates shell commands.
- #52 must spike rename/binary/dirty/combined diff, stale/ambiguous baselines, indeterminate gates, process
  timeout/crash/output/redaction, coverage format/build/path mismatch, association ambiguity and deterministic
  behavior across jobs/root/cache/variants.

Every API record has exact declaration/fingerprint, positive/negative/ambiguous acceptance, typed ownership,
provider/schema dependencies and Doxygen obligations.
