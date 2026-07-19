---
id: DF-0192
title: Bind materialization to complete Store admission and publication identities
status: observed
kind: missing-assumption
impact: invariant
confidence: high
implementation_disposition: blocked
scope:
  - provider.clang22-installed-materialization-publication
  - relation.registry-engine-admission
  - store.snapshot-series-identity
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0009-snapshot-identity-publication-series.md
  - docs/design/adr/0017-relation-descriptor-binding.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_relation_registry.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_request.schema.yaml
  - schemas/cxxlens_ng_clang22_materialization_report.schema.yaml
tracking_issue: '#192'
implementation_issues:
  - '#181'
resolution_refs: []
review:
  mode: independent
  status: pending
  author: codex-agent-root
  reviewer: null
  refs: []
created: '2026-07-19'
---

# Bind materialization to complete Store admission and publication identities

## Observation

The materialization request names `registry.registry_digest` as a content digest over the complete Relation Registry semantic projection, and the report copies that value. The Store does not accept that identity as its registry authority. `relation_registry::build()` derives `relation_engine::registry_digest()` in the `cxxlens.relation-registry.v1` semantic-v2 domain from the exact admitted descriptor-name/runtime-descriptor-digest inventory, and `snapshot_store::begin()` requires the draft series selector to equal that engine digest.

The two digests have different domains, projections, and values. The current materialization request/report does not state the exact engine inventory, derive its digest, bind that digest to the authority Registry descriptors, or preserve it in publication evidence. Passing the request digest to Store fails `store.draft-authority-mismatch`; using an internal engine digest can pass Store but leaves the published registry identity unauthenticated by the machine request/report.

The current engine implementation has a second mismatch at this boundary. ADR 0017 requires the registry digest inventory to use each canonical descriptor ID and bound descriptor digest. `relation_registry::build()` currently serializes the relation map key (`descriptor.name`) instead of `descriptor.id`. A new machine binding must not freeze that implementation drift as authority; the SDK digest must first be brought back to the already accepted descriptor-ID projection.

The publication request also carries an opaque `series_id` rather than the complete `snapshot_series_selector` projection. Store publication requires catalog, channel, engine generation, condition universe, engine registry digest, interpretation policy digest, and trust policy digest before it independently derives the series ID. Some of those values may be derivable elsewhere in the request, but the current materialization checker neither reconstructs the full selector nor proves that the caller-supplied series ID is its identity.

The gap continues after selector construction. The machine contract does not define the exact Store partition draft projection: relation/condition grouping, scope, producer semantics, producer input basis, precision profile, assumption set, per-partition coverage, or unresolved mapping. The report carries aggregate claim-stage digests and a boolean `snapshot_identity_recomputed`, but no exact partition manifests or committed publication record projection from which an external validator can recompute the SDK snapshot and publication IDs. Its fixture uses opaque `snapshot:fixture` and `publication:<configuration>:<backend>` values.

Finally, the memory backend is process-local to one CLI invocation. A non-genesis memory request with an expected prior publication has no authority or input from which the new process can reconstruct that prior head, yet the current schema permits it and the successful fixture uses it. SQLite can open an existing database; memory cannot silently synthesize prior state.

## Working mental model

The authority Registry projection digest and the executable engine inventory digest are distinct and both are useful. The first authenticates the complete accepted Registry semantics from which descriptors were selected. The second authenticates the exact closed set of runtime descriptors admitted to the immutable relation engine and is the identity persisted in Store snapshots. A materializer must derive the second from exact descriptor bindings selected from the first and report both without aliasing them.

Likewise, a Store series ID, partition manifest, snapshot ID, and publication ID are derived identities, not caller assertions. The machine request must carry or deterministically derive every selector and partition-policy component. The tool and independent checker must use SDK-parity codecs to reconstruct the series, partitions, candidate snapshot, and committed publication record. The report must preserve those exact projections rather than replace them with booleans or opaque IDs.

## Mismatch or opportunity

ADR 0096 requires one whole-request memory/SQLite publication transaction, but its current machine schema jumps from authority descriptor bindings to an opaque publication series ID without specifying the engine build/admission projection between them. Integrated design, ADR 0017, ADR 0009, and the Store implementation make this intermediate identity mandatory. Treating the Registry content digest, engine registry digest, and snapshot series ID as interchangeable would weaken snapshot identity and make backend parity evidence unverifiable.

## Evidence

- `descriptor_bindings()` in the materialization checker computes a `sha256:` content digest over the full 21-relation Registry semantic projection.
- `relation_registry::build()` computes a `semantic-v2:sha256:` registry digest over sorted admitted descriptor names and bound runtime descriptor digests.
- ADR 0017 requires canonical descriptor IDs rather than unversioned relation names in that inventory; current source and accepted prose differ.
- On the current authority, the materialization request digest, the selected 12-descriptor engine digest, and the full 21-descriptor engine digest are three distinct values.
- `snapshot_store::begin()` rejects a draft unless selector engine generation and registry digest exactly equal its immutable engine.
- The request/report registry objects contain the authority digest plus six base and six output descriptor bindings, but no engine registry digest or explicit admitted-inventory digest.
- The request publication object contains `series_id` but not the full selector fields required by `snapshot_series_selector`.
- The materialization checker copies `series_id` into the report and compares it as an opaque value; it does not reconstruct the SDK series identity.
- Only `project.catalog_id` is directly available for the seven-field selector. Channel, engine generation, interpretation-policy digest, and trust-policy digest are absent; condition universe is task-local and not required to be uniform; the registry field is the wrong digest axis.
- The checker accepts multiple tasks from different condition universes even though one Store draft and every staged partition must use the selector's single universe.
- The report does not contain Store partition identity fields, exact per-partition claim-content/coverage sets, a snapshot manifest projection, or a publication record sequence/parent projection. Its snapshot/publication IDs are not independently recomputed.
- A new memory Store has no prior head. The current non-genesis memory success fixture therefore cannot be produced by the installed one-request CLI without fabricating external state.

## Alternatives and trade-offs

1. Restore the SDK engine digest to ADR 0017's canonical descriptor-ID projection, then define a complete materialization-to-Store mapping. Bind the authority and engine registry identities, the seven-field selector and derived series ID, a deterministic claim-to-partition policy, exact partition manifests, candidate snapshot, and committed publication record. Restrict memory to fresh genesis; allow non-genesis only for SQLite with an existing checked head. This keeps the engine narrow and makes every publication identity reproducible.
2. Admit all accepted Registry descriptors into the materialization engine and still bind both digest domains and the complete selector. This gives broader query readiness but expands the executable descriptor surface beyond the relations the tool constructs and validates.
3. Redefine Store to accept the authority content digest. This is not local to the materializer and would change established snapshot identity semantics for every Store client.
4. Keep the engine digest and selector components tool-private. This can make a local transaction pass but cannot prove that the resulting snapshot is bound to the request authority or that memory/SQLite used the same series.

Alternative 1 is the current recommendation. The exact 12-descriptor inventory is already closed by the accepted base/output descriptor bindings and avoids silently admitting unrelated relations. Channel remains an explicit caller-selected series dimension with no default; engine generation is contract-owned; all tasks share one request-level condition universe; interpretation and trust policy digests use named canonical projections rather than aliases. Claims are grouped into Store partitions by descriptor, exact condition, interpretation, and common direct semantic-request input basis. Store closure certificates remain exact-zero for this direct materialization unit unless separately authorized.

## Recommendation

Correct `relation_registry` to reject duplicate canonical descriptor IDs and construct its accepted registry inventory from canonical descriptor IDs and bound descriptor digests, with focused compatibility evidence proving name/major-version separation. Amend the materialization contract and versioned request/report schemas to name an `authority_registry_digest` and an `engine_registry_digest`, define the exact sorted 12-descriptor engine inventory and SDK-parity digest algorithm, and reject missing/extra/reordered or mutated descriptor admission.

Replace opaque publication-series authority with the complete selector projection and derived ID. Define the exact SDK claim and partition mapping, including common direct semantic-request basis, condition/interpretation grouping, producer semantics, scope, precision, assumptions, coverage, and unresolved placement. A detailed report must contain enough exact Store projections to recompute every partition manifest, the snapshot manifest/ID, and the publication record/ID, then cross-bind them to reopened Store state and canonical query evidence. Memory requests must be genesis with no expected parent; SQLite may be genesis or may compare against an existing checked head.

Add positive parity tests against `relation_registry::build()`, `snapshot_series_selector::id()`, `make_partition_manifest()`, snapshot identity, and publication identity. Add negative tests for duplicate descriptor IDs, substituting the authority digest as the engine digest, selecting the full or partial wrong inventory, mixed task universes, mutating any selector or partition component under an old ID, constructing memory and SQLite with different series/engine/registry values, opaque fixture IDs, cross-series Store records, non-genesis memory requests, and report/manifest/reopened-store drift.

## Disposition

2026-07-19: Opened during #181 implementation preflight after comparing the accepted machine request/report to the actual Store begin invariant. Publication implementation remains blocked until the registry and complete series-selector mapping are accepted and independently reviewed.

2026-07-19: Independent source audit found that the current engine digest serializes relation names although ADR 0017 requires canonical descriptor IDs. The proposed resolution must restore the SDK to the accepted projection before cross-binding the new materializer and Store identities; current implementation digest values are evidence of the mismatch, not values to preserve as authority.

2026-07-19: Complete Store mapping audit found that only catalog ID is directly available for the seven-field selector, task universes need not agree, and aggregate report assertions cannot reconstruct partition, snapshot, or publication identities. It also showed that non-genesis memory success is unreachable for a one-request process. DF-0192 now covers the complete admission/publication chain and backend genesis policy rather than registry digest alone.
