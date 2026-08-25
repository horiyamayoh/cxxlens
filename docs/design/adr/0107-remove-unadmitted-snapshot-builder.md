# ADR 0107: Remove the unadmitted snapshot builder compatibility path

- Status: Accepted
- Date: 2026-08-26

## Context

The installed SDK header directory exposed `cxxlens::sdk::snapshot_builder`, but
the public C++ API catalog never admitted that class. Its implementation
constructed a one-snapshot compatibility object directly and therefore did not
provide the selector, claim-basis, coverage, closure, publication-CAS, or
memory/SQLite parity guarantees of the normative `snapshot_store` writer path.

The installed package remains distribution `1.0.0`; this change does not alter
the distribution, protocol, schema, SQLite physical-format, or provider version
axes. The public API catalog is the authority for admitted C++ symbols. A
physically shipped but unadmitted declaration is not a supported compatibility
surface.

## Decision

- Remove `snapshot_builder` and its implementation.
- Keep `snapshot_store`, `snapshot_draft`, `partition_draft`,
  `snapshot_writer`, and the closure certificate APIs as the sole snapshot
  construction path.
- Keep the existing distribution and support-matrix version at `1.0.0`; a
  distribution major bump is not used to legitimize an unadmitted symbol.
- Add an installed-header negative compile probe so the removed declaration
  cannot silently reappear through the broad SDK header install rule.
- Do not add a compatibility shim. Any future migration surface must be
  separately admitted in the catalog with its own invariants and tests and must
  delegate to the normative writer rather than fabricate a snapshot.

## Consequences

Callers using the accidental, unadmitted declaration must migrate to the
normative writer API. This is an intentional product-surface cleanup; no
semantic fallback or silent translation is provided. Existing supported
snapshot identities and persistence behavior remain unchanged.

The direct SDK regression now exercises the writer lifecycle, and the relocated
install test compiles a legacy probe expecting failure against the installed
headers.
