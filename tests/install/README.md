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
