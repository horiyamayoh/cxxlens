# Catalog and registry index

| Contract | Path | State |
| --- | --- | --- |
| Relation Registry | `schemas/cxxlens_ng_relation_registry.yaml` | accepted |
| Logical Query Contract | `schemas/cxxlens_ng_logical_query_contract.yaml` | accepted |
| Query Runtime Contract | `schemas/cxxlens_ng_query_runtime_contract.yaml` | implemented |
| Semantic Guarantee Contract | `schemas/cxxlens_ng_semantic_guarantee_contract.yaml` | accepted |
| Snapshot / Store Contract | `schemas/cxxlens_ng_snapshot_store_contract.yaml` | accepted |
| SQLite Physical Store | `schemas/cxxlens_ng_sqlite_store_contract.yaml` | implemented with safety receipts |
| Provider Protocol 2.0 | `schemas/cxxlens_ng_provider_protocol_v2.yaml` | accepted exact wire contract; provider safety tests required |
| Provider Runtime | `schemas/cxxlens_ng_provider_runtime_contract.yaml` | implemented |
| Clang 22 Installed Materialization | `schemas/cxxlens_ng_clang22_materialization_contract.yaml` | implemented with runtime reports |
| Public C++ API Catalog | `schemas/cxxlens_ng_public_api_catalog.yaml` | implemented |
| SDK Doctor Product Catalog | `schemas/cxxlens_ng_sdk_doctor_catalog.yaml` | implemented product-only capability and use-case authority |
| Security Profile | `schemas/cxxlens_ng_security_profile.yaml` | accepted |
| Compatibility v2 | `schemas/cxxlens_ng_compatibility_request.schema.yaml` / `schemas/cxxlens_ng_compatibility_report.schema.yaml` | implemented |
| Support table | `schemas/cxxlens_support_matrix.yaml` | version/environment declaration |

各 contract は schema、positive/negative/fault test、必要な product runtime receipt を同じ authority path で管理します。
Relation Registry は exact scalar-value と cross-TU entity identity の contract を含みます。
`cc.type` の原始 builtin kind と整数値表現は [ADR 0130](../adr/0130-original-scalar-type-facets.md) に従い、型 ID と対象集合の完全性から独立に保持します。
元の候補除外と実際に到達した定数評価呼び出しは [ADR 0131](../adr/0131-original-template-deduction-and-evaluation-events.md) に従い、候補・評価 root・呼び出しの対象集合と binding を独立に保持します。
開発完了は変更固有試験と deterministic CTest の成功で判定します。

Original projection resource reservations use the additive charged-usage contract in [ADR 0132](../adr/0132-projection-resource-reservation-settlement.md).
Original exceptional occurrence populations retain independent physical definitions and compiler lowering variants as specified in [ADR 0133](../adr/0133-original-exceptional-exit-occurrences.md).

Original whole-source feature observations and actual entered-file closure follow [ADR 0134](../adr/0134-original-entered-file-source-features.md); the exact schema slice is [original_source_features_127.yaml](original_source_features_127.yaml).
