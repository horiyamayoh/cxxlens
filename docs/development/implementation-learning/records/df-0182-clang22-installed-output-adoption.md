---
id: DF-0182
title: Define the installed Clang 22 task and validated output adoption boundary
status: investigating
kind: missing-assumption
impact: compatibility
confidence: high
implementation_disposition: blocked
scope:
  - provider.clang22-installed-task
  - provider.validated-output-adoption
  - release.clang22-vertical-evidence
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0015-process-provider-runtime-clang22-normalizer.md
  - docs/design/adr/0038-provider-runtime-protocol-state-validation.md
  - docs/design/adr/0091-distribution-1-production-qualification.md
  - schemas/cxxlens_ng_provider_protocol.yaml
  - schemas/cxxlens_ng_provider_runtime_contract.yaml
  - schemas/cxxlens_ng_portable_provider_task_contract.yaml
  - schemas/cxxlens_ng_release_qualification.yaml
  - schemas/cxxlens_ng_public_api_catalog.yaml
tracking_issue: '#182'
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

# Define the installed Clang 22 task and validated output adoption boundary

## Observation

The Clang 22 worker already parses actual source and emits provider-owned observations plus canonical `cc.entity`, `cc.call_site`, and `cc.call_direct_target` batches. Its task input codec and three observation descriptors are private, however, so an installed host cannot authoritatively construct the worker's exact six-descriptor task. The process runtime validates column chunks but returns raw frames rather than an adoption-capable sealed dependency-group value. The path also lacks an owner for the `build.compile_unit` and `source.span` claims hard-referenced by `cc.call_site`.

## Working mental model

Provider-specific task input should stay detached and Clang-native-free, but installed execution needs one supported authority for constructing it. Successful transcript validation should yield a bounded adoption boundary for sealed dependency groups rather than requiring downstream code to reinterpret raw frames. Source/build ingestion should own the base claims, and canonical Clang output should resolve hard references against that base before staging.

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

## Recommendation

Choose option 1 or 2 for exact provider-specific task authority, add a bounded host adoption boundary consistent with the provider runtime contract, and make source/build ingestion own base hard-reference claims. Reject option 4. Update ADR, machine contracts, public catalog if needed, and installed static/shared evidence before implementation.

## Disposition

2026-07-19: Investigation opened for queued unit #181. Affected implementation is blocked; #179 may classify and report this gap but must not choose or implement the public boundary.
