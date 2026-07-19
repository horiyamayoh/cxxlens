---
id: DF-0182
title: Define the installed Clang 22 task and validated output adoption boundary
status: accepted
kind: missing-assumption
impact: compatibility
confidence: high
implementation_disposition: may-proceed
scope:
  - provider.clang22-installed-task
  - provider.validated-output-adoption
  - release.clang22-vertical-evidence
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0015-process-provider-runtime-clang22-normalizer.md
  - docs/design/adr/0038-provider-runtime-protocol-state-validation.md
  - docs/design/adr/0043-provider-columnar-wire-batches.md
  - docs/design/adr/0044-shared-provider-transcript-validation.md
  - docs/design/adr/0064-portable-provider-task-session-binding.md
  - docs/design/adr/0091-distribution-1-production-qualification.md
  - docs/design/adr/0095-production-scope-closure.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_relation_registry.yaml
  - schemas/cxxlens_ng_provider_protocol.yaml
  - schemas/cxxlens_ng_provider_runtime_contract.yaml
  - schemas/cxxlens_ng_portable_provider_task_contract.yaml
  - schemas/cxxlens_ng_release_qualification.yaml
  - schemas/cxxlens_ng_public_api_catalog.yaml
tracking_issue: '#182'
implementation_issues:
  - '#181'
resolution_refs:
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml
  - schemas/cxxlens_ng_clang22_materialization_request.schema.yaml
  - schemas/cxxlens_ng_clang22_materialization_report.schema.yaml
  - schemas/cxxlens_ng_relation_registry.yaml
  - schemas/cxxlens_ng_portable_provider_task_contract.yaml
  - schemas/cxxlens_ng_provider_protocol.yaml
  - schemas/cxxlens_ng_provider_runtime_contract.yaml
  - schemas/cxxlens_ng_release_qualification.yaml
  - schemas/cxxlens_ng_release_qualification_evaluation_report.schema.yaml
  - schemas/cxxlens_ng_production_scope_closure.yaml
  - schemas/cxxlens_ng_acceptance_manifest.yaml
  - tools/quality/check_ng_clang22_materialization.py
  - tools/quality/check_ng_release_qualification.py
  - tests/quality/test_ng_clang22_materialization.py
  - tests/quality/test_ng_release_qualification.py
review:
  mode: independent
  status: complete
  author: codex-agent-root
  reviewer: codex-agent-df0182-final-contract-reviewer
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/182#issuecomment-5016085557
    - https://github.com/horiyamayoh/cxxlens/issues/182#issuecomment-5016125729
    - https://github.com/horiyamayoh/cxxlens/issues/182#issuecomment-5016187656
    - https://github.com/horiyamayoh/cxxlens/issues/182#issuecomment-5016197410
    - https://github.com/horiyamayoh/cxxlens/issues/182#issuecomment-5016859805
created: '2026-07-19'
---

# Define the installed Clang 22 task and validated output adoption boundary

## Observation

The Clang 22 worker already parses actual source and emits provider-owned observations plus canonical `cc.entity`, `cc.call_site`, and `cc.call_direct_target` batches. Its task input codec and three observation descriptors are private, however, so an installed host cannot authoritatively construct the worker's exact six-descriptor task. The process runtime validates column chunks but returns raw frames rather than an adoption-capable sealed dependency-group value. The path also lacks an owner for the `build.compile_unit` and `source.span` claims hard-referenced by `cc.call_site`.

## Working mental model

Provider-specific task input should stay detached and Clang-native-free, but installed execution needs one supported authority for constructing it. Successful transcript validation should yield a bounded adoption boundary for sealed dependency groups rather than requiring downstream code to reinterpret raw frames. Source/build ingestion should own the base claims, and canonical Clang output should resolve hard references against that base before staging.

The source ownership boundary requires more than retaining a `source_span_id`. The host can construct project, toolchain, variant, source-file, and compile-unit claims from the request/catalog before worker execution, but cannot invert a span digest to recover primary begin/end, semantic role, or read-only state. The provider-owned entity/call observation must therefore retain an optional all-or-none full primary-span bundle. When absent, the observation remains with typed unresolved/non-exact accounting and no source-dependent canonical row; when present, the installed tool can independently construct the standard `source.span` claim in the unpublished transaction.

## Mismatch or opportunity

Integrated design §§17.5-17.6 describe adopted dependency groups and snapshot-draft staging. The current installed public surface ends at validated frames and private provider-specific task construction. A test-only decoder using private headers would make a fixture pass without creating a production path and would leave ADR 0091 unjustified.

## Evidence

- `src/llvm/clang22/provider_worker.cpp` performs actual Clang parsing and emits six batches.
- `src/llvm/clang22/provider_worker.hpp` privately owns `clang22_task_input`, its codec, and observation descriptors.
- `tests/adapter/clang22/provider_worker_protocol_test.cpp` exercises malformed input, not a successful installed invocation.
- `process_execution_report` exposes raw frames; decoded values are validated but not retained for adoption.
- `tests/install/real_project_consumer` publishes an unrelated custom relation and never launches the worker.
- GR production tuple evidence does not include a worker-to-store artifact digest.

## Alternatives and trade-offs

1. Add a Clang-22-specific public C++ host bridge. Directly usable, but expands the stable native SDK surface.
2. Add an installed provider-owned tool/machine contract. Avoids provider-specific general C++ API, but makes tool/provenance/error semantics a production contract.
3. Add a general provider task-builder/adoption abstraction. Reusable, but broader than the proven Clang 22 need.
4. Duplicate the private codec or reconstruct claims from raw frames in tests. Small patch, but no installed authority and no legitimate publication path.

The user selected alternative 2. The accepted design authority names a provider-owned, versioned JSON materialization tool and rejects alternatives 1, 3, and 4 for this boundary. The tool-private immutable sealed result is not a new public C++ type.

## Recommendation

Adopt option 2 for exact provider-specific task authority, keep the adoption boundary inside the installed tool, and make source/build ingestion own base hard-reference claims. Reject options 1, 3, and 4 for this unit. Update the ADR, machine contracts, registry, provider/release contracts, and acceptance traceability before implementation.

Decision update: implement option 2 as `cxxlens-clang22-materialize`, with three canonical v1 plus three observation v2 descriptors, mandatory canonical/observation groups, full primary-span retention, one whole-request memory/SQLite publication transaction, and typed static/shared release reports. Raw frames remain diagnostic evidence only. Do not add a Clang-specific public C++ bridge or general public adoption API.

## Disposition

2026-07-19: Investigation opened for queued unit #181. Affected implementation is blocked; #179 may classify and report this gap but must not choose or implement the public boundary.

2026-07-19: The user chose the provider-owned installed JSON tool. ADR 0096, the integrated design, and the Public C++ API Catalog record that decision and the no-public-C++ boundary. A source audit corrected the earlier hidden assumption: the existing six outputs retain a span ID but not enough primary coordinates to construct `source.span`, so the three observation descriptors use an incompatible v2 family with an optional all-or-none entity/call span bundle while the exact task remains six descriptors. A source-less observation is retained only with typed unresolved/non-exact accounting and no corresponding source-dependent canonical row.

2026-07-19: The first independent adversarial review blocked acceptance because the portable/protocol/runtime projections did not bind the full installed-adoption semantics and release evidence activation depended on overall production-scope closure, making #181 evidence unreachable while unrelated gaps remained. The resolution exact-binds the full specialization projection, including v1 non-adoptability and output/group/source-less/call-site policies, and keys release activation to the exact materializer assignment. The current #181/DF-0182 tracked gap permits exact-zero request/report evidence and exempts only the installed tool binary; an included-and-qualified assignment requires four co-located request/report pairs and two report-set digests regardless of unrelated gaps.

2026-07-19: The same independent reviewer re-ran the two adversarial scopes and returned `ACCEPT`. Focused materialization, release, provider protocol/runtime, SDK, release-contract, and G5 checks passed. Authority, schemas, validators, tests, release/acceptance binding, and review evidence are now present. This record is `accepted`, its implementation disposition is `may-proceed`, and implementation issue #181 may implement the approved Option 2 contract without reopening the public-boundary decision.

2026-07-19: A later independent review of the entire diff found a separate P1 after that two-scope acceptance. ADR 0096 requires project, toolchain-context, variant, source-file, compile-unit, and source-span base claims to be constructed and staged in the same unpublished transaction, but the machine request omitted authoritative payload fields needed to reconstruct the first five claims and the report counted only compile-unit, source-file, and source-span claims. Caller-supplied IDs cannot replace those payloads. The review is recorded at the third review reference above. DF-0182 is reopened as `proposed`, its implementation disposition is `blocked`, and #181 remains blocked until the request, report, checker, tests, and release binding close the exact topological base-claim set and an independent re-review accepts the result.

2026-07-19: The whole-diff review also found that the accepted strict-JSON transport was not machine-bound: duplicate object members were accepted with last-key-wins semantics in release evidence, the materialization checker parsed JSON fixtures through a YAML loader, and invalid UTF-8 was not converted into the typed error boundary. The fourth review reference records this addendum. Resolution also requires one exact raw-byte JSON lexical policy, a shared strict request/report loader, release integration, and duplicate-key/encoding/trailing-value negative evidence before re-review.

2026-07-19: The final independent whole-contract re-review accepted the completed closure. The request now reconstructs and exact-compares all six base-claim families, strict JSON is shared by machine and release ingestion, native observation-v2 rows and span/row associations are exact-bound, physical execution digests are separated from semantic aggregates, release independently verifies row/census equivalence, and Provider Protocol zero-row batches have an exact empty-leaf normalization. The fifth review reference records the final `ACCEPT`. This record is `accepted / may-proceed`; #181 may implement only this Option 2 authority after the #182 authority PR is merged.
