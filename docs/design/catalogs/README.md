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
開発完了は変更固有試験と deterministic CTest の成功で判定します。
