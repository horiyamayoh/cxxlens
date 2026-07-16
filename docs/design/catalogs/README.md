# Next-generation catalog and registry index

以下は次世代設計の machine-readable entry point です。Issue #58 が配置した bootstrap catalog は、各
exact-contract issue が個別の versioned schema、validator、positive/negative vector へ昇格させます。

| Contract | Path | Owner | State |
|---|---|---|---|
| Relation Registry | `schemas/cxxlens_ng_relation_registry.yaml` | #60 | accepted exact contract |
| Logical Query Contract | `schemas/cxxlens_ng_logical_query_contract.yaml` | #61 | accepted exact contract |
| Query Runtime Contract | `schemas/cxxlens_ng_query_runtime_contract.yaml` | #69 | implemented reference runtime |
| Semantic Guarantee Contract | `schemas/cxxlens_ng_semantic_guarantee_contract.yaml` | #62 | accepted exact contract |
| Snapshot / Store Contract | `schemas/cxxlens_ng_snapshot_store_contract.yaml` | #63 | accepted exact contract |
| NG SQLite Physical Store | `schemas/cxxlens_ng_sqlite_store_contract.yaml` | #68 | implemented physical contract |
| Provider Protocol | `schemas/cxxlens_ng_provider_protocol.yaml` | #64 | accepted exact contract |
| Public C++ API Catalog | `schemas/cxxlens_ng_public_api_catalog.yaml` | #66 → #67 | implemented exact contract + relation/claim kernel |
| Acceptance Manifest | `schemas/cxxlens_ng_acceptance_manifest.yaml` | #58 → #71 | bootstrap |
| Security Profile | `schemas/cxxlens_ng_security_profile.yaml` | #65 | accepted exact contract |

## Cross-catalog release authority

| Contract | Path | Owner | State |
|---|---|---|---|
| Product Boundary / Release Compatibility Bundle | `schemas/cxxlens_ng_release_bundle.yaml` | #59 | accepted contract |

release bundle は5 catalog の version axis を統合するが、それらの exact relation/protocol/API/
acceptance/security contract を所有しない。`inspect` / `doctor` の request/report schema と fail-closed
compatibility decision は #59 が所有し、各 axis の意味と実装 qualification は対応する owner issue が
昇格させる。

Relation Registry は Issue #60 により `cxxlens.relation-registry.v1` へ昇格し、18 descriptor、system claim
envelope、validator、positive/negative vector を固定しています。これは schema contract の acceptance であり、
store/query/provider/public C++ implementation の completion を意味しません。

Logical Query Contract は Issue #61 により `cxxlens.logical-query-contract.v1` へ昇格し、annotated multiset、
11 NG0 operator、ordering/partial/continuation、normalized IR digest、memory/SQLite reference evaluator を固定して
います。これは logical semantics の executable oracle であり、production query backend や Issue #66 の公開
C++ signature の completion を意味しません。

Semantic Guarantee Contract は Issue #62 により support-pair truth、soundness/completeness approximation、verification
modality set、exact precondition、lossless provenance/summary drill-down を固定しています。filtering policy や
confidence が truth/guarantee を格上げすることはありません。

Snapshot / Store Contract は Issue #63 により full SHA-256 と versioned binary tuple、claim/partition/closure/
snapshot identity DAG、tagged producer input basis、exact publication series selector、copy-on-write compaction、
format migration、corruption recovery を固定しています。catalog-only `current` と prior publication への silent
fallback は禁止されます。

Provider Protocol は Issue #64 により 104 byte fixed header、deterministic CBOR control、binary column
chunk、credit/ACK/resume、batch / atomic output group / dependency group の不可分性、deterministic task DAG、
structured terminal failure を固定しています。全 output の memory vector 化、cycle/fixed point の silent
ordering、undeclared partial adoption、adjacent version/provider への silent fallback は禁止されます。

Security Profile は Issue #65 により namespace owner、trusted certification/revocation、Ed25519 subject binding、
discovery precedence、shadowing/downgrade rejection、sandbox assurance、product execution audit、untrusted validation
boundary、provider/relation/interpretation/toolchain/platform support tuple を固定しています。manifest の自己認証と
`schema-conformant` は standard canonical authority を付与しません。

Public C++ API Catalog は Issue #66 により `cxxlens.ng-public-api-catalog.v1` へ昇格し、generated typed query、
runtime dynamic query、portable provider、Clang 22 native provider、high-level recipe foundation の5経路を、
signature、error、lifetime、threading、versioning、実装/正例/負例/harness とともに固定しています。通常の
`cxxlens::provider_sdk` は LLVM/Clang を link/fetch せず、native surface は `cxxlens::clang22_provider_sdk` に
明示分離されます。flagship recipe の completion は引き続き Issue #73 が所有します。

Issue #67 は同 catalog を 1.1.0 へ進め、accepted relation IDL から生成した `cc.entity`、`cc.call_site`、
`cc.call_direct_target`、external exemplar `company.lock.acquire`、immutable registry/engine、typed/runtime row、
claim-envelope v2 の assertion/canonical/derived validator、hard/soft reference staging と明示 merge law を
production SDK に追加しました。generated source の再現性と external IDL 追加時の core source diff 0 は
`check_ng_sdk_contract.py` が fail closed で検査します。

Issue #68 は同 catalog を 1.2.0 へ進め、canonical tuple identity、immutable snapshot writer/handle/cursor、exact
publication series、in-memory/SQLite backend、CAS publication、reader generation pin、copy-on-write compaction、
corrupt-current fail-closed recovery、旧 v1 fact store の一方向 mapper boundary を実装しました。SQLite の物理 schema
は ADR 0013 に隔離され、path、backend、page/order、publication sequence は semantic snapshot ID に入りません。

Issue #69 は同 catalog を 1.3.0 へ進め、exact IR argument decoder、snapshot claim annotation cursor、
deterministic reference planner/executor、annotated query result cursor、budget/cancellation、coverage/unresolved/
conflict/guarantee と logical/physical explain を production SDK に追加しました。memory/SQLite は同じ Logical
Query IR と semantic rows を返し、physical backend 情報は logical authority へ入りません。

Issue #70 は同 catalog を 1.4.0 へ進め、exact provider selection、Linux process/sandbox port、binary digest
preflight、bounded provider transcript validator、`cxxlens-clang-worker-22`、Clang observation から
`cc.entity` / `cc.call_site` / `cc.call_direct_target` への canonical normalizer を追加しました。GCC 固有または
無視された semantic option、fatal/error、source normalization failure は exact とせず、provider-local
observation、coverage、unresolved evidence を保持します。

残る `maturity: bootstrap` catalog は acceptance manifest であり、名前空間、version axis、owner issue、依存関係の入口だけを固定します。
entry の `status: contract-pending` は実装、stable API、release support を意味しません。各 owner issue が exact
contract、validator、negative vector、conformance report を追加した時点でのみ maturity を進めます。
