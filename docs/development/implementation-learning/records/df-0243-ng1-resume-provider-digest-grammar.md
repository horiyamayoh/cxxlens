---
id: DF-0243
title: Reconcile NG1 resume provider digest grammar
status: proposed
kind: contract-contradiction
impact: contract
confidence: high
implementation_disposition: blocked
scope:
  - provider.manifest-identity
  - provider.ng1-durable-resume
  - provider.ng1-live-transport
  - release.ng1-qualification
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - schemas/cxxlens_ng_provider_manifest.schema.yaml
  - schemas/cxxlens_ng_provider_protocol.yaml
  - schemas/cxxlens_ng_provider_ng1_hardening.yaml
  - schemas/cxxlens_ng_provider_ng1_hardening.schema.yaml
tracking_issue: '#243'
implementation_issues:
  - '#183'
resolution_refs: []
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

The NG1 hardening contract classifies the corresponding resume binding fields
as `semantic-digest`, and the NG1 resume validator accepts only
`semantic-v2:sha256:<64 lowercase hex>`. The typed transport codec reaches that
validator for encode/decode. Therefore a resume control populated directly
from the current manifest identity is rejected. The current codec vectors use
semantic-v2 values and do not exercise the manifest-derived path.

## Working mental model

The measured provider identity and the NG1 resume semantic projection are
distinct authorities that need one explicit, versioned bridge. That bridge
must preserve exact binary identity and derive any semantic digest from
canonical source bytes or a precisely defined projection. A string prefix
alias is not a semantic digest calculation.

## Mismatch or opportunity

The manifest schema, runtime identity grammar, and NG1 resume field grammar
are incompatible at the live integration boundary. The integrated design
forbids silently reinterpreting legacy/content `sha256:` values as semantic-v2
values. Prefix conversion, silent rehashing, or unconditional dual-namespace
acceptance would fix a local test while weakening identity and replay
authority.

## Evidence

- `schemas/cxxlens_ng_provider_manifest.schema.yaml` defines the manifest
  digest grammar as `sha256:`.
- `tools/clang22/materialize_main.cpp` assigns the measured worker digest to
  the manifest without conversion, and `src/llvm/clang22/materialization_occurrence.cpp`
  measures it as `sha256:`.
- `src/sdk/provider_runtime.cpp` carries manifest identity into provider
  selection and expected-provider validation without conversion.
- `schemas/cxxlens_ng_provider_ng1_hardening.yaml` defines both resume
  provider identity fields as `semantic-digest`.
- `src/sdk/provider_ng1_validation.cpp` validates both fields with the
  semantic-v2-only validator before `ng1_resume_token_digest()` computes the
  replay projection.
- `src/sdk/provider_ng1_transport.cpp` invokes that validator through the
  resume codec bridge.
- The independent review recorded on
  `https://github.com/horiyamayoh/cxxlens/issues/243#issuecomment-5287649392`
  confirms P0=0, P1=0 for the codec-only scope, and a real P2 for future NG1
  integration.

## Alternatives and trade-offs

1. Define an explicit provider-identity semantic projection and calculate it
   from measured bytes and selected contract authority at the live integration
   boundary. This preserves both identity domains and is recommended, subject
   to accepted authority.
2. Change NG1 resume fields to accept legacy/content digests. This changes the
   hardening field grammar and requires coordinated schema, validator, replay
   digest, and qualification changes.
3. Prefix or silently rehash existing `sha256:` strings. Rejected because it
   loses domain provenance and violates semantic digest v2 rules.
4. Accept both namespaces in the validator. Rejected unless exact field
   typing, canonical projection, and replay/security implications are amended
   together.

## Recommendation

Keep the source-private codec fail-closed and keep NG1 capability
advertisement, live transport, resume recovery, and qualification disabled.
Amend the authority first, then update the manifest-derived positive and
negative vectors and all replay/qualification evidence in one bounded change.

## Disposition

2026-08-14: The contradiction was reproduced at exact implementation head
`f6ede7f421dfcc8aa0e153df1c0d1c6e3390a7b7`. Independent read-only review by
Aristotle found no P0/P1 for the codec-only PR, but confirmed a real P2 for
NG1 live integration. This record is `proposed` with
`implementation_disposition: blocked`; #183 may continue only on work that
does not cross the unresolved digest boundary. No authority change or
production activation is authorized by this record.
