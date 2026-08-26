# Authority Retirement Matrix

この matrix は手書き authority、generated projection、explanatory document、test fixture、
historical archive、retire candidate を区別する。削除条件は semantic consumer と直接試験の
成立を要求し、行数削減やファイル数を完了条件にしない。

| Artifact / family | Classification | Replacement / primary owner | Retirement condition |
| --- | --- | --- | --- |
| `schemas/cxxlens_ng_relation_registry.yaml` | Normative | relation registry schema と registry 自身 | accepted relation authority が別途承認されるまで保持 |
| `include/cxxlens/relations/*.hpp` | Generated | `tools/sdk/relation_idl_compiler.py` | registry projection を別 generator へ移行し、`--check` と installed consumer parity が成立した時だけ再判断 |
| `tools/quality/check_ng_provider_protocol.py` の旧 wire codec/state simulation | Retired | C++ protocol adapter/direct tests; schema/ownership/limit checks は retained | 既に C++ vectors を primary として維持。旧 simulation を再導入しない |
| `tests/quality/test_relation_idl_compiler.py` | Test fixture / reproducibility check | registry generator の byte-for-byte check | generated projection authority が変更される時だけ更新 |
| Query checker の schema/ownership 部分 | Test / boundary check | query contract、C++ query direct tests | schema/cross-document boundary を別検査で完全に覆うまで保持 |
| Provider runtime / source-closure checker の safety/provenance 部分 | Product-boundary verification | runtime/schema/C++ tests | provenance、closure、structured terminal、fault/resource vectors が C++ primary になった後に重複部分だけ再分類 |
| Store/SQLite checker の recovery/integrity 部分 | Product-boundary verification | Store/SQLite C++ direct tests | immutable publication、failed isolation、migration、WAL/SHM safety の direct matrix 完成後に重複部分だけ再分類 |
| Clang materialization checker の identity/closure 部分 | Product-boundary verification | native materialization/source-closure C++ tests | exact detached identity、source closure、unsupported outcome の direct matrix 完成後に重複部分だけ再分類 |
| `docs/design/cxxlens_next_generation_integrated_design_ja.md` | Product/explanatory authority | domain machine contracts、catalog、security profile | 後継 integrated design が principles/layers を明示して supersede する時だけ再分類 |
| `docs/archive/` | Historical | 現行 design、schemas、ADR、tests | archive は新実装・生成・CI の入力に戻さない |
| duplicate ADR IDs formerly at 0058/0107 | Retired naming collision | ADR index の active/superseded listing | 0108/0109 の current IDs と index を維持 |
| old `cxxlensProviderSDK` downstream references | Declared consumer gap | separately authorized downstream migration | consumer migration と installed package regression が完了するまで Phase 1 gate を未達とする |

Phase 1 で完了した retirement は wire simulation と duplicate ADR naming に限定する。query、
Store、source-closure、native materialization の safety checker は、accepted product semantics
を守るため無条件には削除しない。dual authority を一時的に置く場合も、primary、divergence
fail policy、replacement、retirement 条件をこの matrix と対応する設計に記載する。
