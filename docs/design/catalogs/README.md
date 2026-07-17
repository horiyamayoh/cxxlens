# Catalog and registry index

| Contract | Path | Owner | State |
|---|---|---|---|
| Relation Registry | `schemas/cxxlens_ng_relation_registry.yaml` | #114 | accepted exact scalar-value contract |
| Logical Query Contract | `schemas/cxxlens_ng_logical_query_contract.yaml` | #61 | accepted exact contract |
| Query Runtime Contract | `schemas/cxxlens_ng_query_runtime_contract.yaml` | #69 | implemented |
| Semantic Guarantee Contract | `schemas/cxxlens_ng_semantic_guarantee_contract.yaml` | #62 | accepted exact contract |
| Snapshot / Store Contract | `schemas/cxxlens_ng_snapshot_store_contract.yaml` | #148 | accepted exact contract |
| SQLite Physical Store | `schemas/cxxlens_ng_sqlite_store_contract.yaml` | #68 | implemented |
| Provider Protocol | `schemas/cxxlens_ng_provider_protocol.yaml` | #64 | accepted exact contract |
| Provider Runtime | `schemas/cxxlens_ng_provider_runtime_contract.yaml` | #70 | implemented |
| Public C++ API Catalog | `schemas/cxxlens_ng_public_api_catalog.yaml` | #66–#70 | implemented |
| Acceptance Manifest | `schemas/cxxlens_ng_acceptance_manifest.yaml` | #71 | implemented foundation gates |
| Foundation Completion | `schemas/cxxlens_ng_foundation_completion_manifest.yaml` | #113 | implemented, CI commit/finding-bound measured-audit report |
| Quality Evidence Ownership | `schemas/cxxlens_ng_quality_ownership.yaml` | #156 | implemented single-owner and revision/toolchain/input-bound evidence contract |
| Quality Evidence Instance | `schemas/cxxlens_ng_quality_evidence.schema.yaml` | #156 | implemented exact configuration evidence and workflow aggregation schema |
| Install Artifact Manifest | `schemas/cxxlens_ng_install_artifact_manifest.schema.yaml` | #156 | implemented revision/tree/toolchain/configuration/all-file binding |
| Security Profile | `schemas/cxxlens_ng_security_profile.yaml` | #65 | accepted exact contract |
| Release Bundle | `schemas/cxxlens_ng_release_bundle.yaml` | #59 | accepted exact contract |

各 contract の schema、positive/negative conformance vector、report schema は同じ prefix で配置します。
G5 と distribution release gate は foundation completion とは別に deferred であり、production-supported を
意味しません。catalog entry が `contract-pending` の場合、その surface は実装済み・stable・production-supported
ではありません。
