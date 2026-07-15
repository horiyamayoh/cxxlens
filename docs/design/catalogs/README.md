# Next-generation catalog and registry index

以下は次世代設計の machine-readable entry point です。全ファイルは
`cxxlens.ng-catalog-bootstrap.v1` schema で version、owner、authority、replacement、contract issue を
相互参照します。

| Contract | Path | Bootstrap owner | Exact-contract issue |
|---|---|---|---|
| Relation Registry | `schemas/cxxlens_ng_relation_registry.yaml` | #58 | #60 |
| Provider Protocol | `schemas/cxxlens_ng_provider_protocol.yaml` | #58 | #64 |
| Public C++ API Catalog | `schemas/cxxlens_ng_public_api_catalog.yaml` | #58 | #66 |
| Acceptance Manifest | `schemas/cxxlens_ng_acceptance_manifest.yaml` | #58 | #71 |
| Security Profile | `schemas/cxxlens_ng_security_profile.yaml` | #58 | #65 |

`maturity: bootstrap` は名前空間、version axis、owner issue、依存関係の入口だけを固定します。entry の
`status: contract-pending` は実装、stable API、release support を意味しません。各 owner issue が exact IDL、
validator、negative vector、conformance report を追加した時点でのみ maturity を進めます。
