---
id: DF-0195
title: Bind sealed provider transcript evidence to materialization report leaves
status: accepted
kind: contract-contradiction
impact: invariant
confidence: high
implementation_disposition: may-proceed
scope:
  - provider.clang22-materialization-report-leaves
  - provider.transcript-coverage-plane
  - release.materialization-guarantee-authority
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0044-shared-provider-transcript-validation.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_provider_protocol.yaml
  - schemas/cxxlens_ng_provider_runtime_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_report.schema.yaml
tracking_issue: '#195'
implementation_issues:
  - '#181'
resolution_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0044-shared-provider-transcript-validation.md
  - docs/design/adr/0071-host-to-provider-transcript-validation.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - docs/design/catalogs/README.md
  - schemas/cxxlens_ng_provider_protocol.yaml
  - schemas/cxxlens_ng_provider_protocol.schema.yaml
  - schemas/cxxlens_ng_provider_runtime_contract.yaml
  - schemas/cxxlens_ng_provider_runtime_contract.schema.yaml
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml
  - schemas/cxxlens_ng_clang22_materialization_report.schema.yaml
  - schemas/cxxlens_ng_release_qualification.yaml
  - schemas/cxxlens_ng_release_qualification.schema.yaml
  - tools/quality/check_ng_provider_protocol.py
  - tests/quality/test_ng_provider_protocol.py
  - tools/quality/check_ng_provider_runtime.py
  - tests/quality/test_ng_provider_runtime.py
  - tools/quality/check_ng_clang22_materialization.py
  - tests/quality/test_ng_clang22_materialization.py
  - tools/quality/check_ng_release_qualification.py
  - tests/quality/test_ng_release_qualification.py
review:
  mode: independent
  status: complete
  author: codex-agent-runtime-authority-df
  reviewer: codex-agent-final-df195-197-review
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/195#issuecomment-5020651186
created: '2026-07-20'
---

# Bind sealed provider transcript evidence to materialization report leaves

## Observation

The generic shared transcript validator accepts a successful provider transcript only when its
coverage set contains `{kind: task, id: <exact provider task ID>, state: covered}`. The installed
Clang 22 worker instead requests and classifies exactly three specialization semantic units:
`frontend.clang22.observation`, `cc.entity`, and `cc.call-extraction`. It emits no `task` unit.
Consequently, an otherwise complete real worker transcript reaches the shared validator but cannot
produce its immutable sealed success value.

Adding the missing generic task record without an authority amendment creates a second mismatch.
The materialization checker requires the global coverage census to contain exactly three records per
task. It does not say whether the generic task record is retained in the sealed task component,
projected into the materialization global summary, or intentionally retained in a separate transport
plane. Silently dropping it would make report evidence differ from the sealed transcript; counting it
would make the accepted three-per-task checker reject the report.

The same report leaf is incomplete in two other ways. Each task result requires
`transcript_digest`, `semantic_digest`, and `raw_frame_digest`, but the materialization authority does
not define the exact runtime value, canonical projection, or byte occurrence for all three. The
checker fixture supplies unrelated placeholder values rather than deriving them through the shared
runtime. The report's guarantee also accepts arbitrary sorted assumption IDs and verification
modalities. The request carries neither set, the sealed generic transcript carries no guarantee
fragment, and the checker fixture invents `clang22-parse`, `query-parity`, and `store-reopen`.

`query-parity` and `store-reopen` are postpublication observations. Using them as prepublication
Store claim guarantee fields without defining whether a modality denotes completed evidence or only
an intended method can make the committed snapshot claim a verification that has not occurred yet.

## Working mental model

Generic transport coverage and specialization semantic coverage are two related but different
planes. The first authenticates that the provider completed the exact transport task and is a
generic validator precondition. The second accounts for the Clang 22 semantic work units used by the
materialization guarantee. A production seal should retain both planes explicitly and bind their
relationship; neither should be inferred from the other or discarded after validation.

Every task transcript digest should come from one shared runtime-owned typed value or exact byte
occurrence. The materializer may project that authenticated value into its report, but should not
implement a second delimiter/prose codec. The raw-frame occurrence remains diagnostic-only even when
its digest is retained.

Guarantee assumptions and modalities are semantic inputs, not fixture decoration. Their exact source
and meaning must be fixed before claim construction. Prepublication claim guarantee modalities and
postpublication Store/query verification receipts may need separate fields so an immutable claim does
not depend on future evidence.

## Mismatch or opportunity

The generic provider success invariant, the actual first-party worker, and the specialization report
checker cannot all be satisfied at once. This blocks the only production worker-to-sealed-result path
required by Issue #181. Placeholder digest leaves and unbound guarantee values additionally let a
schema-valid report describe evidence that cannot be reproduced from the executed process.

The conflict can be corrected without weakening the generic validator or exposing a new public C++
surface. A tool-private sealed task evidence value can retain exact generic and specialization
projections while reusing the shared provider codecs.

## Evidence

- `src/sdk/provider_runtime.cpp::validate_provider_transcript()` sets `task_covered` only for the
  exact `task`/task-ID/`covered` tuple and otherwise returns `provider.coverage-incomplete`.
- `src/llvm/clang22/provider_worker.cpp::execute()` emits only the three specialization coverage
  kinds and uses the task ID as each record ID.
- `tests/fixtures/provider_process_fixture.cpp` emits the generic task record on its success path,
  showing the shared runtime's existing expected shape.
- `tools/quality/check_ng_clang22_materialization.py::validate_detailed_report()` rejects any global
  coverage count other than `3 * len(request["tasks"])`.
- `tools/quality/check_ng_clang22_materialization.py::_task_report()` fills the three task transcript
  digest fields with fixture-only values rather than a shared runtime receipt.
- `include/cxxlens/sdk/provider.hpp` and `src/sdk/provider_runtime.cpp` define
  `process_execution_report::canonical_form()` and `semantic_digest()`, while the tool-private
  process outcome currently retains decoded frames but no exact materialization transcript receipt.
- `schemas/cxxlens_ng_clang22_materialization_request.schema.yaml` has no assumption or verification
  modality input. The materialization fixture creates those values in the report builder.
- `src/llvm/clang22/materialization_claims.hpp` currently accepts a caller-provided
  `materialization_guarantee_authority`; its validator proves shape and canonical order, not the
  caller's authority to choose the values.
- Issue #181 requires actual installed worker execution, exact coverage/guarantee retention, and a
  report whose task and transcript digests bind the executed process.

## Alternatives and trade-offs

1. Add exactly one generic transport `task` record to each successful worker transcript, retain all
   four records in the shared seal, and define separate exact transport and three-unit semantic
   projections in the materialization report. This preserves generic validation and prevents hidden
   record loss. It requires report/contract/checker amendments.
2. Remove the generic task precondition or allow the generic validator to recognize Clang-specific
   coverage kinds. This couples a reusable runtime to one specialization and weakens every other
   provider success verdict.
3. Treat all four records as one undifferentiated global coverage census. This is simpler, but it
   obscures whether protocol completion or semantic work is incomplete and changes the meaning of the
   existing three-unit guarantee.
4. Continue accepting arbitrary task digest and guarantee leaves from the report builder. This keeps
   the fixture green but cannot authenticate the real runtime and is not acceptable.

## Recommendation

Amend the provider/materialization authority with an exact two-plane leaf contract. The current
recommended hypothesis is:

- each successful Clang 22 transcript has exactly one generic transport task record plus the existing
  exact three specialization semantic records;
- the shared sealed transcript retains the complete four-record set;
- each materialization task result retains a recomputable transport projection and a recomputable
  semantic projection, and the global materialization coverage census counts exactly the three
  specialization records per task without pretending the transport record never existed;
- task side-channel and task-result digests state explicitly which of the two projections they bind;
- transcript/report digest fields are renamed or specified as exact projections of the shared typed
  runtime receipt, including an explicit choice between actual emitted bytes and canonical frame
  re-encoding for diagnostic raw-frame evidence;
- assumptions and prepublication claim modalities come from a closed versioned materializer
  authority, while postpublication reopen/query verification remains occurrence evidence unless the
  authority gives it a non-circular semantic meaning.

The normative resolution should update the integrated design, ADR 0096, Provider Protocol and runtime
contract where generic coverage is clarified, the materialization contract and report schema, and the
checker/negative tests. It should reuse or refactor the shared provider transcript codec rather than
copying `process_execution_report` serialization into the materializer.

Independent review should reject the resolution unless it proves all of the following:

- the actual worker produces a sealable success transcript through the same validator as other
  providers;
- missing, duplicate, renamed, non-covered, or wrong-task transport coverage is rejected;
- missing, duplicate, renamed, or non-covered specialization coverage is independently rejected;
- the report preserves all sealed records while the three-unit semantic census closes exactly;
- every transcript digest is reconstructed from executed runtime evidence, not a fixture constant;
- raw frames remain non-adoption authority;
- assumption/modality values have one exact owner and cannot be supplied by an arbitrary caller;
- no claim guarantee depends circularly on a verification that occurs only after its publication.

## Disposition

2026-07-20: Accepted after the normative two-plane receipt/guarantee amendment, raw-only shared
validation integration, and independent post-change review. The final review additionally required
an independently supplied closed provider identity projection; the accepted implementation now
binds raw hello/task acceptance to provider ID/version, measured binary and semantic-contract
digests, negotiated protocol/features, sandbox policy, and Registry-derived offers. Issue #181 may
proceed for this scope.
