# ADR 0098: generated relation static projection is explicit

- Status: Accepted
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
At the independently reviewed proposal checkpoint, the seven newly classified
descriptors remained proposal-only and their public artifacts were deliberately
absent. With this ADR accepted, implementation may add those artifacts, but the
catalog and registry installed-static header sets must become exactly equal, and
ADR 0092's generated-header/callable inventory gates apply in the same change.
Until that implementation is complete, production scope closure remains blocked;
classification alone is not installed-package qualification.

## Consequences

- Static/dynamic classification is explicit and fail-closed for every relation.
- The accepted classification has a machine-checkable target state without treating
  missing public artifacts as dynamic-only.
- Independent review of exact commit
  `332fdca68df26ef316a9f675ce6ae84f7e468710` found no P0/P1 and authorized the
  catalog/header/inventory implementation step.

## Verification

The relation contract rejects missing/invalid projection or tag combinations and
rejects dynamic-only descriptors in the static projection. The accepted authority
checkpoint preserves all published descriptor digests and keeps scope closure
blocked until the seven catalog headers, generated files, callable inventory rows,
examples, and installed-package evidence are complete.
