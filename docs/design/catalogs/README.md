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

## Cross-catalog release authority

| Contract | Path | Owner | State |
|---|---|---|---|
| Product Boundary / Release Compatibility Bundle | `schemas/cxxlens_ng_release_bundle.yaml` | #59 | accepted contract |

release bundle は5 bootstrap catalog の version axis を統合するが、それらの exact relation/protocol/API/
acceptance/security contract を所有しない。`inspect` / `doctor` の request/report schema と fail-closed
compatibility decision は #59 が所有し、各 axis の意味と実装 qualification は対応する owner issue が
昇格させる。

`maturity: bootstrap` は名前空間、version axis、owner issue、依存関係の入口だけを固定します。entry の
`status: contract-pending` は実装、stable API、release support を意味しません。各 owner issue が exact IDL、
validator、negative vector、conformance report を追加した時点でのみ maturity を進めます。
