---
cxxlens_document_status: archived
cxxlens_authority: non-normative
cxxlens_replacement: docs/design/cxxlens_next_generation_integrated_design_ja.md
cxxlens_removal_issue: "#72"
---
# Phase B public contract integration

Issue #53 は #43〜#51 の Contract Candidate を再設計せず、installed public surface へ統合する。
権威は引き続き `schemas/cxxlens_public_api_contract.yaml`、package semantics は
`schemas/cxxlens_package_contract_candidates.yaml`、高リスク判断は
`schemas/cxxlens_high_risk_contract_validation.yaml` にある。最終 `frozen` transition は #54 の責務である。

## Header topology

`schemas/cxxlens_phase_b_contract_integration.yaml` が `include/cxxlens/` の完全な public header
inventory を所有する。各 header は C++23、warnings-as-errors、GCC/Clang で one-header-per-TU
compile される。forward/reverse/repeated include の permutation も同じ gate で検査する。

`<cxxlens/cxxlens.hpp>` は 21 個の LLVM-free package surface を aggregate する。
`<cxxlens/interop/clang.hpp>` は live Clang object の callback-scoped borrow を露出するため、umbrella
には含めない。これは optional Clang adapter の availability/link contract を暗黙に全 consumer へ
伝播させない explicit opt-in boundary である。通常 header での `clang::*`、`llvm::*`、private include、
macro、using-directive は gate が fail-closed に拒否する。

## API, Doxygen, schema, and traceability

124 API / 124 atomic unit の exact signature、signature fingerprint、installed declaration source、family
member の header presence を双方向に検査する。Doxygen の callable-level XML checker と compile-checked
header example に加え、`implementation_state: unimplemented` の planned API に限り candidate manifest の
API ごとの `doxygen_obligation` を意味契約の補完 authority として使用する。Doxygen XML に exact family
member declaration が存在し、manifest obligation と一意に結べない場合は失敗する。実装済み API は従来どおり
XML member 自体の brief/pre/post/note/param/retval/code を全て要求する。各 obligation は
ownership/lifetime、pre/post、error/unresolved/partial、ordering、
versioning と package design section を結ぶ。package document が欠落する場合は integration gate が失敗する。

同じ gate は API から requirement/use case、owner Issue、declaration、schema owner、Doxygen authority、
package example、compile matrix、install test、atomic unit まで 124 行の traceability report を生成する。
error、capability、fact、public type、shared component、provider、schema registry の duplicate/dangling owner
も検査する。task packet、ownership、ready report、authorization、catalog inventory、#52 evidence は全て
input digest に含まれ、stale artifact は既存 generator check と統合 gate の双方で拒否される。

## Package and installed consumption

`examples/packages/` は 22 package を exactly once で覆う。各 source は public installed header のみを
include し、positive、negative、ambiguous、partial/unresolved の契約 scenario を明記する。mutation / generation
surface は plan/dry-run の観測だけを示し、apply を実行しない。source tree と relocatable installed prefix の
双方から GCC/Clang で compile/link する。既存 install consumer は exported CMake target、C++23 feature、
static/shared link closure と optional interop availability を実行検査する。

## Reproducible acceptance

次が Phase B integration 専用 command である。

```sh
cmake --build --preset dev-clang --target cxxlens-phase-b-contract-integration
```

report は `build/<preset>/phase-b-contract-integration-report.json` に出力され、実行時の Git HEAD/tree、
cleanliness、compiler path/version、全 evidence input digest、#52 semantic digest、124 行の traceability、
実行済み compile/link count を記録する。issue close evidence では commit/push 後に `--require-clean` で
同じ checker を再実行し、current commit に bind する。command の宣言だけを green evidence としない。
