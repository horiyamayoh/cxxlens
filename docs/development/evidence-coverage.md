# Evidence and coverage accounting

Structured evidence rows are authoritative. Their typed kind, normalized source, full supporting fact
IDs, and typed attributes define semantic order and identity; summary wording is only a display
projection. `evidence::add` and `merge` sort, deduplicate, and make union idempotent. Validation checks
fact membership against the acquired snapshot and requires version/factory attributes for model,
build-context, approximation, exclusion, and custom rows.

A coverage report stores two authoritative collections: the requested universe and one terminal row
for every request. The validator enforces:

```text
requested - covered - excluded - failed - unresolved - not-applicable = 0
```

Duplicate requests, missing rows, multiple terminal states, and unrequested rows are rejected. A
report is complete only when accounting is valid and neither failed nor unresolved rows remain.
Counts and `complete` are derived from rows during projection; there is no mutable summary state.

An exact guarantee requires complete coverage, achieved precision at least as strong as requested,
and structured evidence for every non-empty requested universe. Sound over/under approximations
require an approximation evidence row. Best-effort and heuristic results may be partial, but their
failed and unresolved coverage remains explicit.
