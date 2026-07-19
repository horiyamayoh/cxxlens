---
id: DF-0174
title: Clarify generated relation tags versus installed static API admission
status: investigating
kind: missing-assumption
impact: irreversible
confidence: high
implementation_disposition: blocked
scope:
  - public-api.generated-relation-admission
  - relation-registry.static-api-projection
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0089-high-level-author-sdk-and-wave0-readiness.md
  - docs/design/adr/0092-exact-public-callable-inventory.md
  - schemas/cxxlens_ng_api_development_readiness.yaml
  - schemas/cxxlens_ng_public_api_catalog.yaml
  - schemas/cxxlens_ng_relation_registry.yaml
  - schemas/cxxlens_ng_relation_registry.schema.yaml
tracking_issue: '#174'
implementation_issues:
  - '#173'
resolution_refs: []
review:
  mode: independent
  status: pending
  author: codex-agent-root
  reviewer: null
  refs: []
created: '2026-07-19'
---

# Clarify generated relation tags versus installed static API admission

## Observation

All 18 accepted NG0 Relation Registry descriptors carry a `generated_cpp_tag`, and the
registry's static API projection names `relations[].descriptor_id` and
`relations[].columns[].id` as its sources. The registry schema requires a generated tag for
every relation. Public API Catalog admission, installed relation headers, and the exact public
callable inventory expose generated C++ tags for only 11 relations.

The seven descriptors without an admitted generated header are `source.origin.v1`,
`cc.declaration.v1`, `cc.type_component.v1`, `core.provider_execution.v1`,
`core.unresolved.v1`, `core.claim_conflict.v1`, and
`core.differential_disagreement.v1`.

## Working mental model

The Public API Catalog intentionally owns installed header admission, while the Relation
Registry owns derivation and identity for an admitted generated header. This record does not
change the dynamic or system semantics of accepted descriptors; it isolates only their installed
static-header admission. The unresolved point is whether a `generated_cpp_tag` on a non-admitted
descriptor is a reserved future name, a required installed projection, or metadata that should
not exist until admission.

## Mismatch or opportunity

ADR 0089 explicitly makes the Public API Catalog the admission authority, so catalog omission
cannot be repaired by silently installing seven additional public headers. At the same time,
the Relation Registry and its schema have no per-relation visibility/admission state that
authorizes those seven tags as deferred, dynamic-only, or system-only. The current checker proves
only that the catalog-admitted subset is generated reproducibly; it does not classify the
remaining tags.

Adding the headers would irreversibly expand the public source API. Removing the tags would change
the registry/schema contract, while merely ignoring them would leave the metadata unclassified.
The affected static-admission unit is therefore blocked until authority makes the classification
explicit.

## Evidence

- `schemas/cxxlens_ng_relation_registry.yaml` defines the static projection and all 18 relation
  descriptors with generated C++ tags.
- `schemas/cxxlens_ng_relation_registry.schema.yaml` requires `generated_cpp_tag` on relation
  entries.
- `schemas/cxxlens_ng_public_api_catalog.yaml` admits 11 generated relation headers under
  `public.relation-static`.
- `schemas/cxxlens_ng_public_callable_inventory.yaml` contains callable rows only for those
  admitted generated headers.
- `docs/design/adr/0089-high-level-author-sdk-and-wave0-readiness.md` assigns header admission to
  the Public API Catalog and derivability checking to the Relation Registry.
- `tools/quality/check_ng_sdk_contract.py::admitted_generated_relations` intentionally selects
  catalog-admitted relation headers before binding them to registry entries.

## Alternatives and trade-offs

1. Admit and generate all seven headers. This maximizes static/dynamic symmetry but makes an
   irreversible API expansion and may expose system relations that were intended to remain a
   dynamic or side-channel surface.
2. Add an explicit per-relation static projection/admission field and classify each descriptor.
   This is the most precise contract and preserves fail-closed auditing, at the cost of schema,
   catalog, checker, documentation, and compatibility-policy changes.
3. Remove `generated_cpp_tag` from non-admitted relations. This minimizes the installed API but
   changes the registry model and requires a clear rule for future admission and stable tag
   reservation.
4. Treat catalog omission alone as permanent authorization. This makes no source change but leaves
   non-admitted tags unclassified and cannot prove the goal's exactly-once scope closure.

## Recommendation

Prefer an explicit registry projection/admission classification owned by an accepted authority,
then decide each of the seven relations individually. Preserve the current catalog-admitted 11
headers until that authority change has independent adversarial review. Do not add or remove the
seven public headers based on this record alone.

## Disposition

2026-07-19: Investigation opened from production-completion audit issue #173. The static public
admission unit is blocked. Dynamic/system relation implementation and unrelated units may proceed.
