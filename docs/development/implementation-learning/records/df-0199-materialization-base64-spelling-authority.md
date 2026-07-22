---
id: DF-0199
title: Reconcile materialization Base64 spelling authority
status: accepted
kind: contract-contradiction
impact: invariant
confidence: high
implementation_disposition: may-proceed
scope:
  - provider.clang22-materialization-request-streaming
  - provider.clang22-task-v3-input-identity
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_request.schema.yaml
tracking_issue: '#199'
implementation_issues:
  - '#181'
resolution_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml
  - schemas/cxxlens_ng_clang22_materialization_request.schema.yaml
  - tools/quality/check_ng_clang22_materialization.py
  - tests/quality/test_ng_clang22_materialization.py
  - src/llvm/clang22/materialization_request.cpp
  - tests/adapter/clang22/materialization_request_test.py
  - tests/adapter/clang22/materialization_request_stream_test.cpp
  - tests/adapter/clang22/materialization_request_v2_1_test.cpp
  - src/llvm/clang22/materialization_request_identity.hpp
  - src/llvm/clang22/materialization_request_v2_1.hpp
  - src/llvm/clang22/provider_task_v3.hpp
  - src/llvm/clang22/provider_task_v3.cpp
  - tests/adapter/clang22/provider_normalizer_test.cpp
review:
  mode: independent
  status: complete
  author: codex-agent-df0199-authority-resolution
  reviewer: codex-agent-df0199-independent-review
  refs:
    - https://github.com/horiyamayoh/cxxlens/issues/199#issuecomment-5023053569
created: '2026-07-20'
---

# Reconcile materialization Base64 spelling authority

## Observation

The accepted v2.1 request schema admits standard-alphabet, correctly padded Base64 strings without
requiring zero discarded padding bits. For example, `YQ==` and `YR==` are both schema-valid and both
decode to the single byte `a`. These are distinct decoded JSON string values; they are not merely two
JSON token spellings for the same string.

The exact `cxxlens.clang22.task.v3` projection includes the complete per-translation-unit request
payload, including `source.content_base64`. Existing independent request/task validation therefore
treats the two schema-valid strings as different task.v3 byte streams and different
`task_input_digest` values even though the decoded source receipt is identical.

The spool-backed streaming implementation instead retains decoded source bytes/count/digest and
re-encodes those bytes with canonical Base64 while constructing task.v3. Its source decoder also
rejects non-zero discarded padding bits. It consequently either rejects an accepted request or
changes the accepted task.v3 identity.

## Working mental model

Three values must remain separate:

1. The raw JSON token spelling, such as whether `=` is written directly or as `\u003d`, is lexical
   transport occurrence and is not task authority.
2. The decoded JSON string is the Base64 spelling consumed by the selected request schema.
3. The bytes decoded from that Base64 string, with their byte count, content digest, and line-index
   receipt, are source authority.

The current authority clearly rejects (1) as semantic authority and clearly authenticates (3), but
is internally inconsistent about whether (2) is also an exact task.v3 input field. A streaming
implementation cannot choose between those meanings silently because the choice changes request
admission and `task_input_digest`.

## Mismatch or opportunity

ADR 0096 says that only decoded source bytes/count/digest are task authority. In the same accepted
v2.1 rollout, however, the request schema admits noncanonical pad bits, the machine contract binds
`content_base64` into the per-TU source projection and exact task.v3 bytes, and the independent
oracle/conformance fixtures require schema-valid decoded Base64 spelling changes to change task
identity.

This is a contradiction inside the accepted v2.1 authority set, not an implementation detail that
can be resolved by selecting whichever existing test is easiest to satisfy. DF-0197 is
non-normative, but its accepted recommendation and review specifically called for raw-spool slices
for the exact decoded Base64 spelling; its resolution references point to the same ADR, contract,
schema, checker, and conformance tests now in tension.

## Evidence

- `docs/design/adr/0096-clang22-installed-materialization-boundary.md:66-70` distinguishes raw JSON
  token spelling from the decoded Base64 value, then says source bytes/count/digest alone are task
  authority.
- `schemas/cxxlens_ng_clang22_materialization_request.schema.yaml` accepts padded Base64 shape but
  does not constrain the discarded padding bits to zero.
- `schemas/cxxlens_ng_clang22_materialization_contract.yaml::project_and_tasks.per_translation_unit_binding`
  includes `content_base64`, while `worker_task_v3` makes exact canonical task.v3 bytes and their
  digest authoritative.
- `tools/quality/check_ng_clang22_materialization.py::worker_task_v3_projection` copies the complete
  task payload, apart from the five expressly excluded derived fields, into task.v3. It therefore
  preserves `source.content_base64` as an exact canonical string field.
- Commit `7200dce` in `tests/adapter/clang22/materialization_request_test.py` proves that the earlier
  conformance expected changing only schema-valid discarded padding bits to invalidate the old
  task-input binding and then pass after rebinding the alternate string spelling.
- Commit `7200dce` in `tests/adapter/clang22/provider_normalizer_test.cpp` required task.v3 to
  round-trip the exact alternate Base64 string, produce a different task-input digest, and
  stream-decode it to the same source receipt.
- `docs/development/implementation-learning/records/df-0197-materialization-task-transport-streaming-bounds.md`
  records the independently reviewed working model that retains raw-spool slices for exact decoded
  Base64 spelling. The record is evidence of review history, not normative authority.
- Commit history does not provide a precedence escape: the explicit task-spelling fixtures were
  added in `7200dce`, while the accepted ADR streaming text, bounded schema form, machine-contract
  streaming amendments, and DF-0197 acceptance were committed together in `6dad619`.

## Alternatives and trade-offs

1. Restrict v2.1 request Base64 to its canonical RFC 4648 spelling by requiring zero discarded pad
   bits. Raw JSON escaping remains non-authoritative, and decoded source bytes/receipt become the
   only source-bearing task authority; task.v3 emits the unique canonical Base64 string derived from
   those bytes. This matches ADR 0096's bytes-only sentence and requires no second spelling replay,
   but narrows the accepted schema and reverses existing exact task.v3 conformance. Because v2.1 is
   not yet merged or qualified, it could be an unreleased correction only after an explicit
   authority amendment and independent compatibility review.
2. Preserve the exact decoded JSON Base64 string in task.v3 while independently streaming its
   decoded bytes to the source spool and validating size/digest/line-index receipts. Raw JSON token
   escapes remain non-authoritative: replay must pass through the same strict JSON string decoder.
   This preserves the current request schema, exact task.v3 projection, and conformance fixtures,
   but requires a bounded spelling replay (a raw-spool slice/re-decoding strategy or a separate
   private spelling spool) and an explicit retained-memory/lifetime statement. ADR 0096's
   bytes-only sentence must be corrected or narrowed.

Both alternatives are reversible inside the unqualified v2.1 active unit, but both change accepted
contract semantics. Work that does not decide source spelling identity may proceed; request source
admission, source-to-task.v3 encoding, and their production qualification remain blocked.

## Proposed authority resolution

Adopt Alternative 1. The selected-v2 request schema accepts exactly the RFC 4648 standard alphabet
with required padding and zero discarded padding bits. `YQ==` and `YWI=` are canonical; `YR==` and
`YWJ=` are rejected before derived identity or task binding. Raw JSON token differences such as `=`
versus `\u003d` decode to the same string and therefore retain the same request and task identity.

The decoded source bytes, byte count, content digest, and line index are the source authority. The
request/task validator decodes and re-encodes `source.content_base64` and requires exact equality,
while task.v3 emits the unique canonical spelling regenerated from the independently sealed source
bytes. Consequently each accepted source byte sequence has one task.v3 source spelling, and neither
an alternate decoded spelling nor raw JSON escaping creates a second task identity.

Retain exact materialization contract version `2.1.0`. This is an active unreleased correction:
protected `main` has not merged the materializer, the production materializer remains incomplete,
and no exact production qualification has run. If review finds any canonical or qualified external
2.1 producer/consumer, this version decision is rejected rather than waived; the resolution must
return to a successor-version and migration-boundary proposal.

## Recommendation

Independently falsify the Alternative 1 proposal before accepting it. The reviewer must distinguish
raw JSON escaping from decoded Base64 spelling; verify positive and negative one- and two-padding
vectors at request schema, independent checker, materialization request codec, task.v3 codec, and
streaming task.v3 decoder; prove source bytes regenerate one unique task.v3 spelling; and confirm
that no canonical or qualified external v2.1 consumer exists. The review must also verify that the
schema-derived maximum projection proof changes only through its authority digest, not its byte
bound or saturated vector.

No source-backed task should be qualified until the independent review is recorded on Issue #199
and this record transitions from `proposed` to `accepted` with a reviewer distinct from the author.

## Disposition

2026-07-20: Investigation opened from Issue #181 after the streaming decoder exposed the conflict
between bytes-only source receipts and schema-valid exact Base64 spelling in task.v3. No authority,
schema, checker, conformance expectation, or production codec has been changed by this record. The
affected source-to-task.v3 binding is blocked pending an authority-first proposal and independent
review on Issue #199.

2026-07-20: Alternative 1 was materialized as an authority-first proposal across the integrated
design, accepted ADR 0096, machine contract/schema, selected request schema, independent checker,
request/task codecs, and positive/negative conformance vectors. Exact version `2.1.0` is retained
only under the documented unmerged, unimplemented, unqualified premise. Status is `proposed`, the
source-backed qualification remains blocked, and no acceptance is claimed pending independent
falsification review on Issue #199.

2026-07-20: The focused v2.1 source replay, request streaming, and provider task.v3 conformance now
passes for canonical one-/two-padding vectors, rejects `YWJ=` and `YR==`, and proves `=` versus
`\u003d` retains the same decoded source/request identity. The legacy
`adapter.clang22-materialization-request` test remains a tracked integration gap: its driver accepts
only v2.0 while the canonical fixture/schema are v2.1. It is not hidden and no v2.0 fallback is
restored; the driver must be retargeted when the source-dependent v2.1 validator is completed.
The Alternative 1 proposal and independent falsification checklist were posted in the
[Issue #199 proposal](https://github.com/horiyamayoh/cxxlens/issues/199#issuecomment-5022890215);
this author proposal is not independent review evidence and is intentionally absent from
`review.refs`.

2026-07-20: Independent falsification found that the initially proposed request-schema regex ended
with `$` and therefore allowed a terminal LF under Python/jsonschema matching semantics. The
reviewer added the ECMAScript-compatible exact-end guard `$(?![\s\S])`, added schema/checker and C++
request/task codec vectors for the terminal-LF case, and recomputed the schema-derived projection
authority digest. Exhaustive enumeration of all 266,240 one-/two-padding terminal quanta then
matched the zero-discarded-bit rule; the 41,530,256-byte maximum, transfer margin, and saturated
vector digest remained unchanged.

2026-07-20: Alternative 1 was accepted after
[independent review](https://github.com/horiyamayoh/cxxlens/issues/199#issuecomment-5023053569). The
accepted disposition permits the source-dependent v2.1 implementation to proceed, but does not
qualify production. That stage must still enforce selected full-schema validation before derived
identity/binding, seal and cross-bind each decoded source to its exact request string and canonical
task.v3 projection, and authorize no launch/publication earlier. The legacy v2.0-only request driver
remains a visible gap and must be retargeted without fallback before production qualification. Exact
version `2.1.0` remains conditional on the reviewed unmerged/unqualified premise; discovery of a
canonical or qualified external 2.1 consumer requires a successor version and explicit migration
boundary.
