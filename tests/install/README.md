# Installed Clang 22 materialization evidence

`clang22_materializer_success_test.py` still performs the authoritative positive
acceptance in the installed prefix. When `--evidence-dir` is supplied, it also
persists the exact request bytes, the exact canonical report stdout bytes, and an
external execution receipt for the selected backend:

```
<evidence-dir>/<static-or-shared>/<memory-or-sqlite>/
  cxxlens-clang22-materialization-request.json
  cxxlens-clang22-materialization-report.json
  cxxlens-clang22-materialization-execution-receipt.json
```

`tools/quality/check_ng_clang22_install_matrix.py` validates those triplets and
writes the contract-defined memory/SQLite report-set projection. A single
installed package is reported as `configuration-complete`; it is never reported
as the exact four-entry static/shared matrix. Pass `--require-exact-matrix` only
after evidence from both installed package jobs has been collected into one
evidence directory.

This is qualification evidence scaffolding, not a release decision. The release
authority remains `tools/quality/check_ng_release_qualification.py`, which also
requires the two install manifests and the other GR evidence. Each
`install-consumer` matrix job now uploads
`build/install-check/tests/install-consumer/materialization-evidence`; the release
authority aggregates the static and shared artifacts and requires all four
request/report/receipt tuples. No provider, platform, or four-configuration
qualification is inferred from one Linux job.

## Windows/MSVC boundary

The `shared-runtime-layout` phase has a Windows-only check for the exact DLL and
import-library destinations of each installed shared target. It is a portable
install-layout invariant, not native Windows/MSVC qualification.

Issue [#223](https://github.com/horiyamayoh/cxxlens/issues/223) remains fail-closed
until a native MSVC runner provides one exact merged-main configure/build/test and
installed static/shared consumer tuple, including Windows filesystem/process/
locking behavior, provider isolation, and available sanitizer-equivalent evidence.
The checked-in CI bootstrap is locked to Ubuntu 24.04 and the release authority
accepts only exact measured tuples; Linux install or semantic evidence does not
close this lane.

## Scale and resource evidence

`clang22_materializer_scale_test.py` is the bounded Linux scale runner for the
Protocol 1.1 ingress boundary. It uses the request driver for the complete
limit-adjacent census (one task, 4,096 tasks, exact 16 MiB source, exact 512 MiB
aggregate source, 1 GiB raw input, limit-plus-one, and pipe-fragmented input) and
records `wait4` peak RSS plus exact stdout/stderr digests. An explicitly selected
subset may also run through the installed materializer.

`clang22_materializer_negative_test.py` runs the installed executable through
authority-bound raw-input and Store negative paths: request-schema rejection,
invalid UTF-8, BOM, duplicate member, non-object, trailing value, and a fresh
SQLite non-genesis request whose `head_current` observation is the exact
`store.current-not-found`/`absent` case. Every case verifies the compact
response, phase, zero-effect ledger, and empty stderr. The optional
`--evidence-dir` writes the exact stdin bytes, stdout report, stderr bytes, and
contract execution receipt for each case; the receipt binds exit status, exact
stdout byte count/digest, parsed response count, and stderr digest. A
non-`store.current-not-found` `head_current` matrix is also exercised through
two disposable copies of a real one-task SQLite genesis Store: one flips a
persisted payload byte and the other tampers with its payload checksum. The
installed response must preserve `sdk-error`, `head_current`,
`current-selector`, the exact publication ID, and the zero-publication effect
ledger as `store.current-corrupt`. The DB and its source-private incremental
sidecar remain outside the installed prefix, and no fault-injection seam or
native/hosted qualification is implied. Each negative case also emits a
test-only `evidence-manifest.json` beside the raw input/report/receipt. Its
validator cross-binds request-bound compact cases and the detailed baseline to
the exact source revision/tree, package configuration, occurrence-manifest
digest, materializer/worker digests, report projection, and external execution
receipt/artifact digests. A raw-input-only case retains no request identity; a
compact response without a binding is recorded as `unbound` with null source
identity rather than inventing attribution. Every manifest is explicitly
marked negative-only and cannot be used as positive or release qualification
evidence.

`check_ng_clang22_materialization_scale.py` is an independent checker. The report
is intentionally marked `release_qualification: false` and `semantic_status:
partial`, with `resource_qualification: false`: ingress scale evidence and
selected installed positives do not qualify all-task semantic materialization,
resource residency, native providers, or the release gate. The
Nightly job passes `--preserve-inputs`, so the exact request files for every
boundary scenario are retained beside the evidence report for checker replay.
