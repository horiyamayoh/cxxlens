# Architecture Decision Records

| ADR | Decision |
|---|---|
| [0002](0002-semantic-relation-platform.md) | semantic relation platform |
| [0003](0003-versioned-relation-kernel.md) | versioned relation kernel |
| [0005](0005-product-boundary-release-compatibility.md) | product boundary and release tuple |
| [0006](0006-ng0-relation-and-claim-envelope.md) | NG0 relation and claim envelope |
| [0007](0007-logical-query-algebra.md) | Logical Query IR algebra |
| [0008](0008-truth-guarantee-provenance-algebra.md) | truth, guarantee and provenance |
| [0009](0009-snapshot-identity-publication-series.md) | snapshot identity/publication |
| [0010](0010-provider-wire-streaming-atomicity.md) | provider protocol and atomicity |
| [0011](0011-provider-trust-certification-discovery.md) | trust, discovery and sandbox |
| [0012](0012-author-sdk-surface.md) | author SDK surface |
| [0013](0013-ng-sqlite-physical-store.md) | SQLite physical store |
| [0014](0014-deterministic-reference-query-runtime.md) | reference query runtime |
| [0015](0015-process-provider-runtime-clang22-normalizer.md) | process runtime and Clang 22 normalizer |
| [0016](0016-semantic-digest-v2.md) | prefix-free semantic digest v2 |
| [0017](0017-relation-descriptor-binding.md) | relation authority/runtime descriptor binding |
| [0018](0018-runtime-relation-schema-parity.md) | runtime relation schema parity |
| [0019](0019-incremental-claim-conflict-classification.md) | incremental claim conflict classification |
| [0020](0020-query-functional-payload-parity.md) | query functional payload classification parity |
| [0021](0021-query-closure-applicability.md) | query closure certificate applicability |
| [0022](0022-logical-query-output-schema-binding.md) | Logical Query IR typed output schema binding |
| [0023](0023-query-terminal-projection-parity.md) | canonical implicit terminal projection parity |
| [0024](0024-query-snapshot-schema-compatibility.md) | query snapshot schema compatibility bind |
| [0025](0025-query-preallocation-budget-accounting.md) | pre-allocation query budget accounting |
| [0026](0026-query-rooted-dag-closure.md) | Logical Query IR rooted DAG closure |
| [0027](0027-query-typed-argument-canonicalization.md) | typed node argument canonicalization |
| [0028](0028-query-canonical-union-execution-order.md) | canonical union execution order |
| [0029](0029-query-conflict-canonical-projection.md) | lossless query conflict canonical projection |
| [0030](0030-shared-canonical-json-and-utf8-validation.md) | shared canonical JSON and strict UTF-8 |
| [0031](0031-query-json-lexical-and-unicode-validation.md) | query JSON lexical and Unicode validation |
| [0032](0032-set-literal-byte-codec-parity.md) | set literal byte codec parity |
| [0033](0033-sqlite-semantic-object-graph-revalidation.md) | SQLite semantic object graph revalidation |
| [0034](0034-complete-partition-closure-subject.md) | complete partition closure subject |
| [0035](0035-derived-basis-partition-membership.md) | derived basis partition membership |
| [0036](0036-claim-stage-input-revalidation.md) | claim stage input revalidation |
| [0037](0037-lossless-claim-evidence-occurrences.md) | lossless claim evidence occurrences |
| [0038](0038-provider-runtime-protocol-state-validation.md) | typed process-provider protocol state validation |
| [0039](0039-exact-provider-fallback-policy.md) | exact provider fallback policy |
| [0040](0040-provider-frame-version-and-flags.md) | provider frame version and flag semantics |
| [0041](0041-provider-control-text-utf8.md) | strict UTF-8 provider control text |
| [0042](0042-immutable-provider-selection-authority.md) | immutable provider selection authority token |
| [0056](0056-measured-foundation-zero-audits.md) | measured foundation zero-audit subreports |
| [0057](0057-detached-scalar-value-validation.md) | shared detached scalar value validation |
| [0058](0058-query-scan-occurrence-column-identity.md) | scan occurrence-qualified query column identity |
| [0059](0059-query-row-guarantee-canonical-projection.md) | lossless row-level guarantee canonical projection |
| [0060](0060-query-summary-guarantee-fragment-algebra.md) | result-contributing guarantee fragment algebra |
| [0061](0061-relocatable-static-shared-install-package.md) | relocatable static/shared installed package qualification |
| [0062](0062-first-party-sanitizer-closure.md) | first-party sanitizer compile and runtime closure |
| [0063](0063-project-catalog-bottom-up-identity.md) | project catalog bottom-up exact-input identity |
| [0064](0064-portable-provider-task-session-binding.md) | portable provider task and session exact binding |
| [0077](0077-persisted-publication-identity-binding.md) | persisted publication identity binding |
| [0078](0078-snapshot-version-wire-u32-canonicality.md) | snapshot version wire u32 canonicality |
| [0079](0079-checked-publication-counters.md) | checked publication counters |
| [0080](0080-provider-columnar-typed-digests.md) | provider columnar typed digests |
| [0081](0081-provider-candidate-canonical-identity.md) | provider candidate canonical identity |
| [0082](0082-verified-executable-fd-binding.md) | verified executable FD binding |
| [0083](0083-cross-tu-semantic-entity-identity.md) | cross-TU semantic entity identity |
| [0084](0084-macro-call-occurrence-discriminator.md) | macro call occurrence discriminator |
| [0085](0085-static-row-view-exact-validation.md) | static row view exact validation |
| [0086](0086-self-contained-claim-evidence-occurrences.md) | self-contained claim evidence occurrences |

identity、condition、closure、protocol major、snapshot format、native lifetime、sandbox、determinism を変更する場合は
新しい ADR が必要です。
