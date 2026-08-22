# Catalog and registry index

| Contract | Path | State |
| --- | --- | --- |
| Relation Registry | `schemas/cxxlens_ng_relation_registry.yaml` | accepted |
| Logical Query Contract (Issue #61) | `schemas/cxxlens_ng_logical_query_contract.yaml` | accepted |
| Query Runtime Contract | `schemas/cxxlens_ng_query_runtime_contract.yaml` | implemented |
| Semantic Guarantee Contract (Issue #62) | `schemas/cxxlens_ng_semantic_guarantee_contract.yaml` | accepted |
| Snapshot / Store Contract (Issue #148) | `schemas/cxxlens_ng_snapshot_store_contract.yaml` | accepted |
| SQLite Physical Store | `schemas/cxxlens_ng_sqlite_store_contract.yaml` | implemented with safety receipts |
| Provider Protocol (Issue #149) | `schemas/cxxlens_ng_provider_protocol.yaml` | accepted exact contract; provider safety tests required |
| Provider Runtime | `schemas/cxxlens_ng_provider_runtime_contract.yaml` | implemented |
| Clang 22 Installed Materialization | `schemas/cxxlens_ng_clang22_materialization_contract.yaml` | implemented with runtime reports |
| Public C++ API Catalog | `schemas/cxxlens_ng_public_api_catalog.yaml` | implemented |
| Public Callable Inventory | `schemas/cxxlens_ng_public_callable_inventory.yaml` | implemented |
| Security Profile | `schemas/cxxlens_ng_security_profile.yaml` | accepted |
| Compatibility v2 | `schemas/cxxlens_ng_compatibility_request.schema.yaml` / `schemas/cxxlens_ng_compatibility_report.schema.yaml` | implemented |
| Support table | `schemas/cxxlens_support_matrix.yaml` | version/environment declaration |

各 contract は schema、positive/negative/fault test、必要な product runtime receipt を同じ authority path で管理します。
Relation Registry は Issue #152 の accepted exact scalar-value and cross-TU entity identity contract を含みます。
開発・release の運用証跡（qualification report、review receipt、work-unit、checksum、集約 JSON）は catalog に追加しません。
