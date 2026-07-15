# Next-generation catalog and registry index

以下は次世代設計の machine-readable entry point です。Issue #58 が配置した catalog は
`cxxlens.ng-catalog-bootstrap.v1` から開始し、各 exact-contract issue が個別の versioned schema と validator へ
昇格させます。

| Contract | Path | Owner | State |
|---|---|---|---|
| Relation Registry | `schemas/cxxlens_ng_relation_registry.yaml` | #60 | accepted exact contract |
| Provider Protocol | `schemas/cxxlens_ng_provider_protocol.yaml` | #58 → #64 | bootstrap |
| Public C++ API Catalog | `schemas/cxxlens_ng_public_api_catalog.yaml` | #58 → #66 | bootstrap |
| Acceptance Manifest | `schemas/cxxlens_ng_acceptance_manifest.yaml` | #58 → #71 | bootstrap |
| Security Profile | `schemas/cxxlens_ng_security_profile.yaml` | #58 → #65 | bootstrap |

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

残る `maturity: bootstrap` catalog は名前空間、version axis、owner issue、依存関係の入口だけを固定します。
entry の `status: contract-pending` は実装、stable API、release support を意味しません。各 owner issue が exact
contract、validator、negative vector、conformance report を追加した時点でのみ maturity を進めます。
