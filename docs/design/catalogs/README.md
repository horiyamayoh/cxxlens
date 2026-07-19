# Catalog and registry index

| Contract | Path | Owner | State |
| --- | --- | --- | --- |
| Relation Registry | `schemas/cxxlens_ng_relation_registry.yaml` | #152 | accepted exact scalar-value and cross-TU entity identity contract |
| Logical Query Contract | `schemas/cxxlens_ng_logical_query_contract.yaml` | #61 / #166 | accepted NG0/NG1 exact contract |
| Query Runtime Contract | `schemas/cxxlens_ng_query_runtime_contract.yaml` | #69 / #166 | implemented closure-bound anti-join |
| Semantic Guarantee Contract | `schemas/cxxlens_ng_semantic_guarantee_contract.yaml` | #62 | accepted exact contract |
| Snapshot / Store Contract | `schemas/cxxlens_ng_snapshot_store_contract.yaml` | #148 | accepted exact contract |
| SQLite Physical Store | `schemas/cxxlens_ng_sqlite_store_contract.yaml` | #68 | implemented |
| Provider Protocol | `schemas/cxxlens_ng_provider_protocol.yaml` | #149 | accepted exact contract |
| Provider Runtime | `schemas/cxxlens_ng_provider_runtime_contract.yaml` | #151 | implemented exact contract |
| Clang 22 Installed Materialization | `schemas/cxxlens_ng_clang22_materialization_contract.yaml` | #181 | accepted option-2 boundary with DF-0191/DF-0192 machine-v2 amendment under independent review; implementation and production evidence tracked by #181 |
| Public C++ API Catalog | `schemas/cxxlens_ng_public_api_catalog.yaml` | #66–#70 | implemented |
| Public Callable Inventory | `schemas/cxxlens_ng_public_callable_inventory.yaml` | #169 | implemented exact Clang 22 AST and Doxygen bidirectional correspondence |
| Acceptance Manifest | `schemas/cxxlens_ng_acceptance_manifest.yaml` | #71 | implemented foundation gates |
| Foundation Completion | `schemas/cxxlens_ng_foundation_completion_manifest.yaml` | #113 | implemented, CI commit/finding-bound measured-audit report |
| API Development Readiness | `schemas/cxxlens_ng_api_development_readiness.yaml` | #168 | implemented clean-main Wave 0 baseline |
| Design Feedback Record | `schemas/cxxlens_ng_design_feedback_record.schema.yaml` | #171 | implemented non-normative implementation-learning lifecycle |
| G5 Qualification | `schemas/cxxlens_ng_g5_qualification.yaml` | #166 | implemented exact-SHA closure, incrementality, bounded recursion, and R4 performance gate |
| Quality Evidence Ownership | `schemas/cxxlens_ng_quality_ownership.yaml` | #156 | implemented single-owner and revision/toolchain/input-bound evidence contract |
| Quality Evidence Instance | `schemas/cxxlens_ng_quality_evidence.schema.yaml` | #156 | implemented exact configuration evidence and workflow aggregation schema |
| Install Artifact Manifest | `schemas/cxxlens_ng_install_artifact_manifest.schema.yaml` | #156 | implemented revision/tree/toolchain/configuration/all-file binding |
| Security Profile | `schemas/cxxlens_ng_security_profile.yaml` | #151 | accepted exact contract |
| Release Bundle | `schemas/cxxlens_ng_release_bundle.yaml` | #59 | accepted exact contract |

各 contract の schema、positive/negative conformance vector、report schema は同じ prefix で配置します。
G5 は foundation completion とは独立に認定され、distribution release gate は #167 の完了まで deferred です。
G5 の実装はそれ自体で production-supported を意味しません。catalog entry が `contract-pending` の場合、その surface は実装済み・stable・production-supported
ではありません。
