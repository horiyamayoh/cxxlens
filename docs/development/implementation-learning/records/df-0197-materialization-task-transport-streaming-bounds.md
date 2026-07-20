---
id: DF-0197
title: Close materialization task transport and streaming bounds
status: observed
kind: contract-contradiction
impact: invariant
confidence: high
implementation_disposition: blocked
scope:
  - provider.clang22-materialization-request-streaming
  - provider.clang22-task-v3-input-transfer
  - provider.host-transcript-payload-limits
  - release.materialization-scale-evidence
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0010-provider-wire-streaming-atomicity.md
  - docs/design/adr/0044-shared-provider-transcript-validation.md
  - docs/design/adr/0064-portable-provider-task-session-binding.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_provider_protocol.yaml
  - schemas/cxxlens_ng_provider_runtime_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_request.schema.yaml
tracking_issue: '#197'
implementation_issues:
  - '#181'
resolution_refs: []
review:
  mode: independent
  status: pending
  author: codex-agent-streaming-request-architecture
  reviewer: null
  refs: []
created: '2026-07-20'
---

# Close materialization task transport and streaming bounds

## Observation

The installed materialization request accepts a decoded source of up to 16,777,216 bytes per task
and a corresponding `content_base64` string of up to 22,369,624 characters. The exact
`cxxlens.clang22.task.v3` projection embeds that base64 string, the complete global project catalog,
effective argv, and all other task authority. Provider Protocol v1 permits payload only on the one
`open_task` frame in its exact five-frame host transcript and fixes that frame's payload limit at
16,777,216 bytes. A schema-valid maximum source therefore exceeds the transport limit before
canonical tuple framing or any catalog and task metadata is added.

The current request implementation also contradicts ADR 0096's bounded streaming requirement. The
test driver reads stdin into one string, `json_document` retains both the raw occurrence and a
recursive DOM, and every validated task owns a copy of the global catalog, base64 spelling, decoded
source, and encoded worker payload. The default JSON limits are only 16 MiB input and 8 MiB per
string, so even a single schema-valid maximum source cannot pass the current parser. Retaining all
task catalogs and task payloads additionally creates an `O(task_count * catalog_count)` expansion.

At the 512 MiB aggregate decoded-source ceiling, let `D` be decoded source bytes and `E` their
base64 character count. The retained source-bearing lower bound is `raw request + DOM base64 + task
base64 + decoded source + task-payload base64 >= R + 3E + D`, where `E` is approximately `4D/3`
and `R >= E`. This is already approximately 3.17 GiB before DOM/container overhead, transient
canonical trees, repeated catalog bytes, argv, descriptors, and provider-runtime copies.

## Working mental model

Raw request occurrence authority, decoded task semantics, and worker transport are separate
lifetimes. The raw occurrence should be captured once into an immutable private spool with its exact
bounded prefix digest. A duplicate-aware lexical pass can authenticate the JSON document and
envelope without a DOM; a selected-version pass can validate exact v2 shape, stream each base64
value into a bounded source spool, and retain only replay slices and bounded task metadata. The
global catalog should have one immutable owner, and tasks should be validated and executed in
canonical order without retaining all source or encoded worker payloads in memory.

Fragmentation cannot weaken task identity. If task input crosses multiple frames or an equivalent
content-addressed transfer, one shared host encoder/worker decoder/transcript validator must bind
the exact ordered bytes, total length, and final task-input digest before `task_accepted`. Raw frames
remain diagnostic occurrences; only the shared validator's immutable sealed value can authorize
claim construction or Store adoption.

## Mismatch or opportunity

No implementation can simultaneously accept the current maximum request, encode exact task.v3,
and obey the accepted Provider Protocol v1 payload limit and state machine. Raising a runtime
`protocol_limits` value locally would silently contradict the machine authority; lowering the source
limit would silently shrink the accepted request schema. A private filesystem side channel would
introduce unauthenticated input authority.

The streaming request refactor is therefore blocked at its host-to-worker boundary until normative
authority selects an input-transfer contract. Work on generic bounded spool and parser mechanics may
continue, but it cannot be described as a production-complete installed request path.

## Evidence

- `schemas/cxxlens_ng_clang22_materialization_request.schema.yaml` permits
  `source.size_bytes <= 16777216` and `content_base64.maxLength == 22369624`.
- `schemas/cxxlens_ng_clang22_materialization_contract.yaml` repeats the per-task and aggregate
  source limits and requires streaming base64 to private spool.
- `schemas/cxxlens_ng_provider_protocol.yaml` fixes `wire.limits.payload_bytes` at 16,777,216 and
  permits a host payload only on the single `open_task` frame.
- `src/llvm/clang22/provider_task_v3.cpp::task_payload_projection()` embeds the exact base64 string,
  full catalog, argv, and task authority in task.v3.
- `tests/adapter/clang22/materialization_request_driver.cpp` reads all stdin bytes into a string.
- `src/llvm/clang22/materialization_json.hpp::json_document` owns both raw bytes and a recursive
  value tree; default input/string limits are below the request schema limits.
- `src/llvm/clang22/materialization_request.cpp::parse_task()` copies the project catalog, retains
  base64 and decoded source, encodes and retains a complete worker payload, and checks the aggregate
  source limit only after that work.
- `src/llvm/clang22/materialization_io.hpp` has bounded capture, append, and seal ports but no
  immutable replay/slice/reservation operations required by a streaming parser and task builder.
- Issue #181 requires the installed actual-source path, 4,096-task/512-MiB bounds, shared transcript
  validation, raw-frame non-authority, and static/shared memory/SQLite qualification.

## Alternatives and trade-offs

1. Add a bounded authenticated task-input chunk sequence, or an equivalent content-addressed input
   transfer, to the Provider Protocol. Preserve exact task.v3 logical bytes and digest across chunks,
   and validate the same state machine in the host, worker, and shared runtime. This preserves the
   accepted source limit and avoids a single large frame, but changes protocol/session authority and
   requires versioned conformance evidence.
2. Increase the one-frame payload limit to a proved finite maximum for full task.v3. This is smaller
   mechanically, but the current schema does not give every variable task string a finite maximum,
   it retains a large contiguous payload requirement, and it changes protocol resource/security
   qualification.
3. Reduce request source and field limits so every task.v3 fits 16 MiB. This preserves Protocol v1
   but weakens the accepted materialization contract; the safe source maximum varies with catalog,
   argv, and other task fields.
4. Transfer source by an ambient path or descriptor outside the authenticated transcript. This is
   rejected unless a new exact content-addressed and sandboxed input-transfer contract binds the
   occurrence; otherwise the worker gains hidden input authority.

## Recommendation

Prefer alternative 1 as the working hypothesis. Define a bounded ordered input-transfer sequence
whose terminal authority is the exact task.v3 byte length and content digest. Decide explicitly
whether logical task.v3 remains the codec across physical fragmentation or an exact successor codec
is required. The shared host encoder, worker decoder, transcript validator, budget accounting, and
fault-injection corpus must use one state machine; missing, duplicate, reordered, extra, truncated,
or digest-mismatched chunks fail before task acceptance.

After that authority is accepted, replace the request path with two strict spool-backed JSON passes:
lexical/envelope dispatch first, then selected-v2 schema and bottom-up binding. Retain a single
immutable catalog, raw-spool slices for exact decoded base64 spelling, bounded per-task decoded
source spools, and a compact task index. Validate the complete request before effects, then build,
launch, seal, and destroy each task payload/source occurrence sequentially. The raw request spool
remains until response write completion, while provider frames are destroyed as soon as the shared
immutable seal and diagnostic digest receipt are established.

Independent review should require limit-adjacent and maximum source/task/catalog tests, 1 GiB raw
input and 512 MiB aggregate source tests, arbitrary short reads, duplicate JSON members at arbitrary
depth, all task-input chunk perturbations, spool failures, deterministic task-order evidence, and
measured peak retained memory independent of task/source aggregate size.

## Disposition

2026-07-20: Opened as a blocking invariant contradiction during Issue #181's production request
architecture audit. Normative authority is intentionally unchanged and `resolution_refs` and review
refs remain empty. Issue #181 may continue work outside production request ingestion and
host-to-worker launch, but must not complete or qualify the installed streaming boundary until this
record is resolved by accepted authority and independent review.
