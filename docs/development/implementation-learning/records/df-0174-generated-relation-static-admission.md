---
id: DF-0174
title: Clarify generated relation tags versus installed static API admission
status: accepted
kind: missing-assumption
impact: irreversible
confidence: high
implementation_disposition: may-proceed
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
  - '#174'
resolution_refs:
  - docs/design/adr/0098-explicit-static-relation-projection.md
review:
  mode: independent
  status: complete
  author: codex-agent-root
  reviewer: codex-agent-u0-static-admission-review
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/174#issuecomment-5255918605
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
Registry owns derivation and identity for an admitted generated header. Accepted ADR 0098 adds an
explicit `cpp_projection` field so this record no longer relies on tag nullability as admission
metadata. It classifies the seven standard descriptors as `installed-static`; the implementation
must now admit their public artifacts exactly, without changing descriptor semantics or digests.

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

Implement accepted ADR 0098 by making the catalog/header/callable/example sets exactly match all
eighteen `installed-static` relations. Preserve descriptor digests and keep all three
`dynamic-only` observation relations excluded from generated static API admission.

## Disposition

2026-07-19: Investigation opened from production-completion audit issue #173. The static public
admission unit is blocked. Dynamic/system relation implementation and unrelated units may proceed.

2026-08-12: ADR 0098 and the registry/checker proposal classify every relation explicitly and
identify the seven target installed-static descriptors. This record remains proposed/blocked:
independent review must accept ADR 0098 before catalog admission, generated headers, callable
inventory, examples, or installed qualification are activated.

2026-08-12: Independent adversarial review accepted exact proposal commit
`332fdca68df26ef316a9f675ce6ae84f7e468710` with no P0/P1. ADR 0098 is accepted and this record
is `may-proceed`; installed-package qualification remains blocked until the seven public artifact
families and their exact catalog/inventory/tests are implemented and reviewed.

2026-08-12: Issue #174 implementation admitted and generated all seven accepted
`installed-static` relation headers, while the three frontend observation relations remain
`dynamic-only`. The Public API Catalog and registry now bind exactly eighteen static relation
headers; the Clang 22 and Doxygen-derived callable inventory contains 604 rows. Learning
checkpoint: DF-0174 remains the governing resolved feedback record; independent implementation
review and installed-package qualification evidence remain required before terminal release
qualification.

2026-08-12: Static activation exposed a runtime/IDL parity gap: the accepted
`cc.type_component` canonical claim key includes optional projection members, whose absence is
represented as canonical null. Accepted ADR 0018 requires `key_columns` to equal the complete
claim-key role set; it does not require every key member to be non-optional. The runtime validator
now enforces that accepted rule, preserving the registry descriptor and digest. Regression evidence
is `relation-schema-parity` in `tests/unit/sdk/sdk_test.cpp`.

2026-08-12: Static activation also exposed an accepted scalar spelling outside the runtime's
generic `_id` suffix rule: `core.unresolved.scope` is `typed_id<stable_unit_key>`. The runtime
validator now recognizes that exact accepted identity spelling without admitting arbitrary
`typed_id` parameters; the registry descriptor and digest remain unchanged. Positive
`canonical-stable-unit-key-scalar` and negative non-identity conformance vectors preserve the
boundary.

2026-08-12: Independent implementation review found two further activation-only parity gaps. The
Store wire decoder still bounded `scalar_kind` at the former `set` ordinal, and the query validator
and reference executor omitted the five newly admitted scalar spellings. The decoder now accepts
through the appended `interpretation_domain_id` ordinal while preserving every legacy ordinal; the
query paths map all five exact canonical names. Store reopen and typed/dynamic query execution tests
exercise the last ordinal and every new spelling. The same review cycle exposed a quality-fixture
isolation defect: a negative scope test wrote through a symlink into the repository's DF record.
Its temporary root now owns a real docs copy, so fail-closed mutation cannot alter source evidence.

2026-08-12: Independent review P1 follow-up found that `source.origin` had accepted
`container_elements: true` relation-IDL metadata but the public runtime descriptor and generated
tag projection omitted it. ADR 0017 therefore did not bind the element-wise reference law, and
claim adoption compared the encoded set as one scalar. The runtime projection now binds the flag,
requires unary `set<T>` to exact scalar `T` targets, and resolves every canonical set element under
the existing interpretation/presence law; the all-static registry and missing-element regressions
are required evidence. The same review clarified `content_digest` as the conceptual strong type
for claim content: raw, semantic-v2, and lowercase typed-domain SHA-256 spellings are accepted
recursively for `set<content_digest>`, while generic `digest` remains narrower.
