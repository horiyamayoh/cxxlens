# ADR 0098: generated relation static projection is explicit

- Status: Proposed
- Date: 2026-08-12
- Issue: #174
- Depends on: ADR 0089, ADR 0092

## Context

The Relation Registry previously inferred a generated C++ surface from a non-null
`generated_cpp_tag`. The Public API Catalog admitted only eleven generated headers,
while seven standard NG0 descriptors carried an unclassified generated tag. That
one-way relation let the catalog prove that an admitted header was derivable, but did
not prove whether a tagged descriptor was installed, dynamic-only, or deferred.

## Decision

Each registry relation SHALL declare `cpp_projection` as exactly one of
`installed-static` and `dynamic-only`. `installed-static` requires one non-null
`generated_cpp_tag`; `dynamic-only` requires a null tag. The registry static
projection derives descriptor and column IDs only from `installed-static` relations.

The target classification is all eighteen currently tag-bearing NG0 descriptors as
`installed-static`, including `source.origin`, `cc.declaration`,
`cc.type_component`, `core.provider_execution`, `core.unresolved`,
`core.claim_conflict`, and `core.differential_disagreement`. The three
provider-owned `frontend.clang22.*_observation.v2` descriptors remain
`dynamic-only` with null tags.

Catalog admission remains the authority for installed public headers under ADR 0089.
Before this ADR is accepted, the seven newly classified descriptors are proposal-only:
their catalog headers, generated files, callable inventory rows, examples, and
installed-package claims SHALL NOT be added. Production scope closure SHALL retain
them as blocked-pending-review rather than treating omission as a dynamic-only
disposition. On acceptance, the catalog and registry installed-static header sets
must be exactly equal, and ADR 0092's generated-header/callable inventory gates
apply in the same change.

## Consequences

- Static/dynamic classification is explicit and fail-closed for every relation.
- The proposal has a machine-checkable target state without prematurely expanding
  the public source API.
- Acceptance requires independent review of DF-0174 before public header admission
  and generated callable inventory activation.

## Verification

The relation contract rejects missing/invalid projection or tag combinations and
rejects dynamic-only descriptors in the static projection. Scope closure reports
the seven proposal-only installed-static descriptors as blocked pending review;
catalog-generated header freshness remains limited to the eleven already admitted
headers until acceptance.
