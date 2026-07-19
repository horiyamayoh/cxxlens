---
id: DF-0187
title: Break the project-catalog and build-relation identity cycle
status: accepted
kind: contract-contradiction
impact: invariant
confidence: high
implementation_disposition: may-proceed
scope:
  - kernel.project-catalog-identity
  - relation.build-identity-dag
  - provider.materialization-base-claims
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0063-project-catalog-bottom-up-identity.md
  - docs/design/adr/0064-portable-provider-task-session-binding.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_project_catalog_contract.yaml
  - schemas/cxxlens_ng_relation_registry.yaml
  - schemas/cxxlens_ng_public_api_catalog.yaml
tracking_issue: '#187'
implementation_issues:
  - '#182'
  - '#181'
resolution_refs:
  - include/cxxlens/sdk/relation.hpp
  - docs/design/adr/0063-project-catalog-bottom-up-identity.md
  - docs/design/adr/0064-portable-provider-task-session-binding.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_project_catalog_contract.yaml
  - schemas/cxxlens_ng_project_catalog_contract.schema.yaml
  - schemas/cxxlens_ng_portable_provider_task_contract.yaml
  - schemas/cxxlens_ng_portable_provider_task_contract.schema.yaml
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml
  - schemas/cxxlens_ng_clang22_materialization_request.schema.yaml
  - schemas/cxxlens_ng_clang22_materialization_report.schema.yaml
  - tools/quality/check_ng_clang22_materialization.py
  - tests/quality/test_ng_clang22_materialization.py
review:
  mode: independent
  status: complete
  author: codex-agent-root
  reviewer: codex-agent-df0182-final-contract-reviewer
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/187#issuecomment-5016861230
created: '2026-07-19'
---

# Break the project-catalog and build-relation identity cycle

## Observation

The accepted project-catalog and Relation Registry identity projections form a cryptographic cycle:

```text
catalog_digest
  = H(compile_unit_id, effective_invocation_digest, source_digest, environment_digest)
project_id
  = H(catalog_digest, logical_root, environment_digest)
build_variant_id
  = H(project_id, toolchain_id, language/standard/target/macro/include/flags)
compile_unit_id
  = H(source_snapshot_id, build_variant_id, toolchain_id, effective_invocation_digest)
```

The cycle appears only if the catalog entry's input ID is implicitly aliased to the final relation ID. That equality was an undocumented assumption in the first DF-0182 fixture, not an accepted mapping. If imposed, a valid value requires an infeasible SHA-256 fixed point. This became executable evidence when DF-0182 stopped treating base IDs as opaque and reconstructed all request-owned base claims bottom-up.

## Working mental model

Catalog input identity and published relation identity form an acyclic authority graph. The accepted public header already describes `catalog_compile_unit::compile_unit_id` as stable within the catalog, whereas the Relation Registry independently derives the final `build.compile_unit` ID from source, variant, toolchain, and invocation. A materializer must retain both identities and an exact mapping; a shared name is not equality authority. Task identity and execution occurrence identity are likewise distinct: identical semantic task projections may share a provider task ID while input digest and provider execution ID distinguish executions.

## Mismatch or opportunity

ADR 0063 correctly requires the catalog digest and ID to bind every exact compile-unit entry. The Relation Registry correctly requires each base relation identity to be derived from authoritative payload. The contradiction came from silently treating the catalog-local entry ID as the final relation ID. Accepted API text, runtime validation, tests, and the catalog consumer list do not establish that alias. The missing contract was an explicit catalog-entry-to-relation mapping and execution correlation rule; accepting caller-supplied placeholders or weakening relation identities would still hide the defect.

## Evidence

- `project_catalog::canonical_projection()` includes each entry's `compile_unit_id`, invocation, source, and environment digests.
- `include/cxxlens/sdk/relation.hpp` describes that ID as stable within the catalog; `catalog_compile_unit::validate()` only validates detached control-free input identity.
- Accepted SDK tests use `unit:a` and `unit:b`, not relation-derived `compile-unit:sha256:...` values.
- ADR 0063 maps the validated catalog into `build.project` and never declares equality with `build.compile_unit`.
- `build.project.v1.project` derives from catalog digest, logical root, and environment digest.
- `build.variant.v1.variant` derives in part from `project`.
- `build.compile_unit.v1.compile_unit` derives in part from `variant`.
- ADR 0064 and `core.provider_execution.v1` distinguish semantic task ID from input-bound execution identity, so multiple TU executions may share one global-catalog task ID without aliasing results.
- The first DF-0182 checker could derive all non-cyclic base identities but could not construct a fixed point because it reused one field for the two identities and keyed results only by task ID.
- Issue #187 records the exact cycle and alternatives without treating the issue as authority.

## Alternatives and trade-offs

1. **Selected:** make the existing catalog-scoped input identity and the final `build.compile_unit` relation ID explicit, preserve the full validated global catalog for every generic task, and bind each selected entry to its final row by exact digests. Correlate results by `(provider_task_id, task_input_digest, provider_execution_id)`. This preserves public signatures, the project-catalog projection, relation identities, and generic task identity; it clarifies previously implicit semantics and adds the missing machine mapping.
2. Remove `project` from `build.variant` identity. This is locally small, but identical variants from different projects would retain conflicting project payload under one functional key unless the relation is redesigned further.
3. Remove `catalog_digest` from `build.project` identity or introduce a stable pre-catalog project seed. This breaks the cycle and may make project identity stable across catalog revisions, but changes public relation identity semantics and must prove cross-project uniqueness and conflict behavior.
4. Require all base claims in a pre-existing parent snapshot. This contradicts ADR 0096's same-transaction genesis semantics, adds parent-ingestion evidence, and leaves the underlying accepted identity cycle unresolved.

## Recommendation

Adopt alternative 1 without changing public signatures or any accepted relation projection. Make the catalog-local/final identity boundary explicit in ADR 0063, the project-catalog contract, Doxygen, and ADR 0096. The request must carry the full canonical catalog entry array plus a selected catalog ID and final relation ID per task; the checker must reconstruct the global catalog before project/variant/final-unit IDs and exact-compare every entry digest. Because all TU executions use the same global semantic task, allow repeated provider task IDs and correlate request/report records by the exact task/input/execution tuple. Keep #182 and #181 blocked until an independent adversarial review accepts the full authority and machine-contract resolution.

## Disposition

2026-07-19: Opened from the final DF-0182 base-claim closure audit. The finding affects public identity and compatibility semantics, so affected implementation remains blocked. No alternative is accepted authority yet.

2026-07-19: Focused authority/API audit found that the fixed point was caused by an undocumented alias in the new fixture. Existing Doxygen scopes the catalog ID within the catalog, the validator accepts detached catalog input IDs, accepted tests use opaque catalog-local values, and ADR 0063 has no `build.compile_unit` equality mapping. Alternative 1 is now the proposed resolution: preserve the global catalog, explicitly map its selected input ID to the independently derived final relation ID, and use task/input/execution composite correlation. Authority and machine changes remain `proposed/blocked` pending independent review.

2026-07-19: Independent whole-contract review accepted alternative 1. ADR 0063/0064/0096, Doxygen, the project-catalog and materialization machine contracts, and fail-closed tests now distinguish the catalog-local input identity from the independently derived final relation ID, recompute the exact ordered effective invocation, and correlate physical results by task/input/execution without contaminating semantic aggregates. The review reference records the final `ACCEPT`. This record is `accepted / may-proceed`; the identity-cycle blocker is resolved without changing public signatures or accepted relation identity projections.
