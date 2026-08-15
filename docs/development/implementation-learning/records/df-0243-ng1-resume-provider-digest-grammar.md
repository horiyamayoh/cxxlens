---
id: DF-0243
title: Reconcile NG1 resume provider digest grammar
status: accepted
kind: contract-contradiction
impact: contract
confidence: high
implementation_disposition: may-proceed
scope:
  - provider.manifest-identity
  - provider.ng1-durable-resume
  - provider.ng1-live-transport
  - release.ng1-qualification
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - schemas/cxxlens_ng_provider_manifest.schema.yaml
  - schemas/cxxlens_ng_provider_protocol.yaml
  - schemas/cxxlens_ng_provider_protocol.schema.yaml
  - schemas/cxxlens_ng_provider_ng1_hardening.yaml
  - schemas/cxxlens_ng_provider_ng1_hardening.schema.yaml
  - schemas/cxxlens_ng_provider_ng1_conformance_vectors.yaml
  - schemas/cxxlens_ng_provider_ng1_conformance_vectors.schema.yaml
  - schemas/cxxlens_ng_provider_ng1_qualification_report.schema.yaml
  - docs/design/adr/0100-ng1-resume-provider-digest-grammar.md
tracking_issue: '#243'
implementation_issues:
  - '#183'
resolution_refs:
  - docs/design/adr/0100-ng1-resume-provider-digest-grammar.md
  - schemas/cxxlens_ng_provider_ng1_hardening.yaml
  - schemas/cxxlens_ng_provider_ng1_hardening.schema.yaml
  - schemas/cxxlens_ng_provider_protocol.yaml
  - schemas/cxxlens_ng_provider_protocol.schema.yaml
  - schemas/cxxlens_ng_provider_ng1_conformance_vectors.yaml
  - schemas/cxxlens_ng_provider_ng1_conformance_vectors.schema.yaml
  - schemas/cxxlens_ng_provider_ng1_qualification_report.schema.yaml
  - src/sdk/provider_ng1_validation.cpp
  - tests/unit/sdk/provider_ng1_hardening_test.cpp
  - tests/unit/sdk/provider_ng1_transport_test.cpp
  - tools/quality/check_ng_provider_ng1.py
  - tools/quality/check_ng_provider_ng1_qualification.py
  - tests/quality/test_ng_provider_ng1_qualification.py
review:
  mode: independent
  status: complete
  author: codex-agent-ng1-digest-grammar-review
  reviewer: Aristotle-agent-019ffd7a-398e-7161-8ea3-8a40359b531f
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/243#issuecomment-5287649392
created: '2026-08-14'
---

# Reconcile NG1 resume provider digest grammar

## Observation

The provider manifest and current runtime identity path represent both
`provider_binary_digest` and `provider_semantic_contract_digest` as
`sha256:<64 lowercase hex>`. The materializer passes the measured worker
digest through without conversion, and the runtime carries the selected
identity into its expected-provider validation.

Before this resolution, the NG1 hardening contract classified the
corresponding resume binding fields as `semantic-digest`, and the NG1 resume
validator accepted only `semantic-v2:sha256:<64 lowercase hex>`. The typed
transport codec reached that validator for encode/decode, so a resume control
populated directly from the current manifest identity was rejected before its
token projection could be validated. The candidate adds the missing
field-specific grammar and exercises both the manifest-derived path and
namespace substitution rejection.

## Working mental model

The measured provider identity and the NG1 resume token projection are
distinct authorities. The provider identity fields carry the exact manifest
content digest, while the token digest is a separate semantic-v2 digest over
the canonical tuple. The projection preserves each provider identity string
exactly; it is not a string-prefix alias or a rehashing bridge.

## Mismatch or opportunity

The manifest schema and runtime identity grammar use `sha256:`, while the
other NG1 binding digests and token projection use semantic-v2. The resolved
boundary is explicit and field-specific. The integrated design still forbids
silently reinterpreting content `sha256:` values as semantic-v2 values;
prefix conversion, silent rehashing, and unconditional dual-namespace
acceptance remain invalid.

## Evidence

- `schemas/cxxlens_ng_provider_manifest.schema.yaml` defines the manifest
  digest grammar as `sha256:`.
- `tools/clang22/materialize_main.cpp` assigns the measured worker digest to
  the manifest without conversion, and `src/llvm/clang22/materialization_occurrence.cpp`
  measures it as `sha256:`.
- `src/sdk/provider_runtime.cpp` carries manifest identity into provider
  selection and expected-provider validation without conversion.
- `schemas/cxxlens_ng_provider_ng1_hardening.yaml` now defines both resume
  provider identity fields as `manifest-content-digest` and records the
  field-specific grammar and accepted ADR.
- `src/sdk/provider_ng1_validation.cpp` validates provider identity fields
  with the exact `sha256:` validator, validates semantic binding fields with
  the semantic-v2 validator, and preserves all values as exact strings in
  `ng1_resume_token_digest()`.
- `src/sdk/provider_ng1_transport.cpp` invokes that validator through the
  resume codec bridge.
- `schemas/cxxlens_ng_provider_ng1_conformance_vectors.yaml` and the focused
  unit/quality tests cover accepted manifest digests and rejected namespace
  substitution.
- The independent review recorded on
  `https://github.com/horiyamayoh/cxxlens/issues/243#issuecomment-5287649392`
  confirms P0=0, P1=0 for the codec-only scope, and a real P2 for future NG1
  integration.

## Alternatives and trade-offs

1. Define a new provider-identity semantic projection and calculate it from
   measured bytes and selected contract authority. Rejected for this bounded
   candidate because it invents a second identity value not present in the
   manifest contract.
2. Use the exact manifest content digest for the two provider identity fields,
   while retaining semantic-v2 for the other binding fields and token digest.
   Selected with coordinated schema, validator, replay projection, vector,
   report-traceability, and focused-test changes.
3. Prefix or silently rehash existing `sha256:` strings. Rejected because it
   loses domain provenance and violates semantic digest v2 rules.
4. Accept both namespaces in the validator. Rejected unless exact field
   typing, canonical projection, and replay/security implications are amended
   together.

## Recommendation

Keep the source-private codec fail-closed and keep NG1 capability
unadvertised. The accepted amendment may proceed through bounded validator,
vector, checker, and evidence work, but does not authorize live transport,
resume recovery, or production qualification.

## Disposition

2026-08-14: The contradiction was reproduced at exact implementation head
`f6ede7f421dfcc8aa0e153df1c0d1c6e3390a7b7`. Independent read-only review by
Aristotle found no P0/P1 for the codec-only PR, but confirmed a real P2 for
NG1 live integration. At that point this record was `proposed` with
`implementation_disposition: blocked`; #183 could continue only on work that
did not cross the unresolved digest boundary. No authority change or
production activation was authorized by that prior disposition.

2026-08-15: Accepted by ADR 0100. The bounded candidate changes the NG1
hardening YAML/schema, protocol and report traceability, source-private
validator, conformance vectors, focused unit/quality tests, and checkers so
that provider identity fields require exact manifest content digests and the
overall token digest remains semantic-v2 over the canonical tuple. No prefix,
silent rehash, or dual-namespace acceptance is introduced. NG1 remains
proposed and unadvertised; the live transport/production qualification gate
for #183 remains unresolved and is intentionally outside this candidate.
