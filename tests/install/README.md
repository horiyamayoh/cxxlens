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

## Scale and resource evidence

`clang22_materializer_scale_test.py` is the bounded Linux scale runner for the
Protocol 1.1 ingress boundary. It uses the request driver for the complete
limit-adjacent census (one task, 4,096 tasks, exact 16 MiB source, exact 512 MiB
aggregate source, 1 GiB raw input, limit-plus-one, and pipe-fragmented input) and
records `wait4` peak RSS plus exact stdout/stderr digests. An explicitly selected
subset may also run through the installed materializer.

`clang22_materializer_negative_test.py` runs the installed executable through two
authority-bound negative paths: a raw-input-only request-schema rejection, and a
fresh SQLite non-genesis request whose `head_current` observation is the exact
`store.current-not-found`/`absent` case. It verifies the compact response, exact
operation/path, discarded logical draft, zero publication, and empty stderr. A
non-`store.current-not-found` `head_current` SDK error still requires an injected
Store failure seam and is not claimed by this install test.

`check_ng_clang22_materialization_scale.py` is an independent checker. The report
is intentionally marked `release_qualification: false` and `semantic_status:
partial`, with `resource_qualification: false`: ingress scale evidence and
selected installed positives do not qualify all-task semantic materialization,
resource residency, native providers, or the release gate. The
Nightly job passes `--preserve-inputs`, so the exact request files for every
boundary scenario are retained beside the evidence report for checker replay.
