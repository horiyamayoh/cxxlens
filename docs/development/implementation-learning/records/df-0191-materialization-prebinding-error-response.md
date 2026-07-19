---
id: DF-0191
title: Define phase-authentic materialization failure responses
status: observed
kind: contract-contradiction
impact: contract
confidence: high
implementation_disposition: blocked
scope:
  - provider.clang22-installed-materialization-transport
  - provider.materialization-request-validation
  - release.materialization-report-ingestion
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_request.schema.yaml
  - schemas/cxxlens_ng_clang22_materialization_report.schema.yaml
tracking_issue: '#191'
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

# Define phase-authentic materialization failure responses

## Observation

ADR 0096 and the materialization machine contract require exactly one JSON request on stdin and exactly one JSON report on stdout. The same authority requires lexical, schema, version, identity, catalog, task, and descriptor failures to produce stable typed materialization errors before effects or publication. The only report schema nevertheless requires fully bound request, source, installation, provider, project, registry, task, claim, provenance, publication, and semantic-verification evidence for every failed report.

A request rejected before semantic binding has no authoritative materialization request ID, project catalog, task set, registry mapping, publication target, or execution evidence. Invalid UTF-8 and duplicate-member inputs may not even have an authoritative decoded JSON object. The current contract therefore cannot produce a schema-valid response for all failures it promises to report without omitting stdout, violating the report schema, or fabricating identities and evidence.

The same problem continues after request binding when failure precedes a complete sealed materialization. An installed-binary mismatch cannot truthfully fill fields that assert the requested binary was measured; worker launch and early transcript failures do not have complete groups, batches, side channels, claims, provenance, or guarantees; and a Store failure before head lookup must distinguish an unobserved head from an observed absent head. The current failed-report branch has no progress boundary for those states. Its checker returns before bottom-up report validation, so a stale-parent fixture can retain success-derived digests while zeroing unrelated counts and still pass.

## Working mental model

A machine response may bind only facts established before its failure phase. A pre-binding failure can always bind the exact raw stdin byte occurrence, a stable failure phase, a typed error code, and a no-effect verdict. It may bind decoded request fields only after the strict decoder and the relevant validation stage have authenticated those fields. The detailed materialization report remains appropriate only after the complete request has crossed the semantic binding boundary.

The stdout transport should therefore be a closed versioned response union with phase-authentic branches: a detailed report only after the materialization evidence DAG is complete, and a compact failure response for earlier failures. The compact response binds raw input observation, the last authenticated request boundary, failure phase, typed error, and exact no-publication effects. A detailed stale-parent failure remains valid only when every pre-publication materialization leaf is complete and independently revalidated. Release qualification accepts only a passed detailed report. No branch may use stderr prose, sentinel identities, or optional detailed fields as control authority.

## Mismatch or opportunity

The report schema's unconditional required set and the checker's unconditional `validate_request()` call make request-level error reporting unsatisfiable. Its current failed fixture covers a valid request that later fails stale-parent CAS; it does not exercise any failure before request binding. ADR 0096 also says that required fields, identities, publication, or error semantics of JSON contract v1 must not be changed as a patch, so an in-place reinterpretation of the accepted v1 report is not an allowed correction.

## Evidence

- ADR 0096 fixes the transport to one stdin request and one stdout report, rejects permissive JSON, and registers request/version/identity/catalog/task/descriptor error families.
- `schemas/cxxlens_ng_clang22_materialization_report.schema.yaml` unconditionally requires all request-derived and execution-derived sections even when `result` is `failed`.
- The report schema's failed branch only requires discarded adoption and zero publication; it does not relax unavailable request or execution bindings.
- `tools/quality/check_ng_clang22_materialization.py::validate_report()` calls `validate_request()` first and exact-compares request-derived bindings before its failed-result return.
- Existing lexical negative vectors prove that BOM, invalid UTF-8, nested duplicate members, a non-object document, and a trailing or second value can fail before an authoritative request object exists.
- The current stale-parent failed fixture begins with a completely valid bound request and therefore does not cover this contradiction.
- ADR 0096 Compatibility and rollback explicitly requires a new contract/version for a change to v1 error semantics.

## Alternatives and trade-offs

1. Introduce an explicit new machine/response version whose stdout schema is a closed union of a complete detailed report and a phase-aware compact failure response. The compact branch supports both raw-input-only and request-bound states, while the detailed failed form is reserved for a complete materialization that fails publication CAS. This preserves exactly one typed JSON stdout value, keeps evidence strict, and avoids fabricated identities. It requires exact version migration and release/checker updates.
2. Add a distinct error envelope without a closed response-union schema. The payload can remain small, but callers would need out-of-band schema selection and could accidentally treat an error envelope as qualification evidence.
3. Permit no stdout value for pre-binding failure and map typed errors to exit codes. This avoids invented report fields but weakens the accepted one-response transport and makes structured recovery dependent on a second channel.
4. Fill the detailed report with sentinels or caller-derived guesses. This is rejected because syntactically valid placeholders are not semantic authority and may collide with real identities.

Alternative 1 is the current recommendation. Because the executable has not yet been implemented or qualified, a new major version can replace the unimplemented v1 rollout without reinterpreting any published report. The exact response fields, failure boundary, exit semantics, and version transition remain non-normative until the authority change and independent review are complete.

## Recommendation

Define a new closed response schema and machine-contract major version. The compact failure branch should contain only the response schema/version, response kind, failed result, generation time, bounded raw-input observation, optional exact request binding when validation completed, failure phase, stable error code, diagnostic text, and an exact effect ledger showing zero committed transactions and preserved prior head. Input-limit rejection must describe the exact consumed prefix rather than claim a digest for unread bytes. A complete materialization that later fails stale-parent CAS must use the detailed branch, retain every materialization binding, and undergo the same bottom-up validation as a passed report.

The checker should validate raw bytes and an unbound compact response without invoking full request validation; validate a request-bound compact response only after full request validation; and validate all detailed materialization evidence before branching on publication outcome. Add one positive response fixture per failure phase and negative fixtures for digest/size/completeness drift, fabricated detailed sections, request-binding drift, phase/code mismatch, unobserved-versus-absent Store head confusion, effect drift, partially validated detailed failures, and attempts to use any failure response as release evidence. State an authoritative stdin byte ceiling and consumed-prefix rule before the implementation allocates or decodes the request. Bind process exit status to result (`0` only for passed detailed report, one exact nonzero status for a schema-valid failure response) without making exit status or stderr the error-kind authority.

## Disposition

2026-07-19: Opened during #181 implementation preflight. The contradiction affects typed machine error semantics and fail-closed behavior, so materializer runtime implementation is blocked. No sentinel or no-report fallback is authorized.

2026-07-19: A second audit showed that the defect is not limited to pre-binding failures. Installation mismatch, worker launch, partial transcript, and pre-observation Store failures also lack the complete evidence required by the detailed schema, while the checker skips bottom-up validation for every failed detailed report. The proposed v2 resolution is therefore phase-aware: compact raw-input/request-bound failure responses for incomplete pipelines, and a detailed failed report only after a complete materialization reaches publication CAS.
