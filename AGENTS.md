# cxxlens agent contract

## Authority and product boundary

判断が衝突する場合は、次の順序を優先する。

1. `docs/design/cxxlens_next_generation_integrated_design_ja.md` の原則と invariant
2. relation registry、Provider Protocol、Public C++ API Catalog、Security Profile
3. accepted ADR と担当 issue の contract
4. acceptance fixture と実装
5. `docs/archive/` の履歴資料

製品の方向は、コンパイラの観測結果を再現可能・問い合わせ可能な意味知識へ変換し、
分からない場合は不足理由を返すことである。claim/provenance、coverage、closure、
unresolved、conflict、differential disagreement、guarantee、unknown、materialization
report、SQLite/source-closure の安全 receipt、provider の署名・binary identity・失効・
sandbox・canonical semantic certification は製品機能として維持する。

開発・release の運用証跡は [ADR 0106](docs/design/adr/0106-test-only-development-and-release-policy.md)
に従い、生成・保存・再検証しない。Acceptance Manifest、work-unit、review receipt、
exact-SHA メモ、checksum、qualification JSON、集約 report、Learning checkpoint は
現行 authority ではない。v1 compatibility shim も持たない。

## Required product design

新しい public surface、relation、provider、analysis、model、recipe では、実装前に次を
宣言する。

- 独立 consumer と use case、既存 capability と不足 capability
- `proved`、`disproved`、`unknown`、`partial`、`conflicting` の結果
- coverage、closure、unresolved、conflict、guarantee、provenance の保持方法
- unknown の原因と、依存順の completion plan
- exact contract ID、authority、write scope、support/stability disposition

public API 数、relation 数、green test 数だけで製品完成を主張しない。追加 surface は
consumer path または明示した tracked gap を持つこと。高リスクの public semantics、
identity、protocol、persistence、不可逆 effect、resource bound は、仕様/ADR と
executable state machine、phase-authentic outcome、bounded resource、crash/effect の
positive・negative・fault test で反証可能にする。独立 review は任意であり、試験を代替しない。

## Development and issue policy

通常の経路は direct-to-main である。履歴の rewrite、force update、reset、rebase はしない。
変更は小さくし、影響する試験を先に実行する。開発完了の必須条件は次の二つだけである。

1. 変更固有の positive・negative・fault・determinism/resource/error test が成功する。
2. `main` workflow が全決定的 CTest と契約/security/docs/install/header checks を成功させる。

issue は main workflow が green になった時点で、追加コメント・receipt・checkpoint を作らず
close する。release qualification を implementation issue の完了条件へ遡及させない。

## CI and release

- `.github/workflows/quality.yml` は Clang 22 static/shared の全 build と全決定的 CTest、
  contract/security/docs、static/shared installed consumer、GCC public header を実行する。
- path selection、fast report、JUnit保存、timing JSON、toolchain provenance、artifact
  upload/download、結果集約 job は使用しない。
- `.github/workflows/release.yml` は手動または `v*` tag で main 全件に加え ASan/UBSan、
  TSan、static analysis、stress/repeat、最大 scale、real-project、relocated-install を
  実行する。一件でも失敗したら package job を実行しない。
- 試験用 artifact は保存しない。GitHub の job log と Git の履歴は通常機能として残す。
- package install へ運用専用 schema を含めない。対応環境は
  `schemas/cxxlens_support_matrix.yaml` の `{release_version, surface, os, architecture,
  compiler_provider_major, linkage}` で照合し、未掲載環境と Windows/MSVC は unsupported。

Compatibility request/report は v2 とし、`os`、`architecture`、`toolchain`、`linkage` を
使う。`runtime_qualified`、`evidence_refs`、`qualification_state`、
`compat.release-not-qualified` は削除済みである。

## Implementation rules

- C++23、public namespace/type/function は設計書の lower snake case。
- public header に `clang::*`、`llvm::*`、LLVM/Clang header を露出しない。
- schema-first: semantics/invariants、identity、value types、schema、validator、tests、service。
- filesystem、process、time、hash は port 越し。AST pointer を保存・所有・移送しない。
- unordered iteration order を serialization/ID に使わない。
- empty と unresolved を区別し、read result の evidence/coverage/guarantee を落とさない。
- unknown は actionable な不足理由と completion plan を含む。
- mutation/generation は plan、独立 validator、dry-run、transaction の順。
- public API/relation/provider の変更は catalog/registry、Doxygen、acceptance test、設計 traceability を整合させる。
- provider trust、claim provenance、runtime receipt の安全条件を generic な運用証跡削除と混同しない。

## Forbidden shortcuts

- name/pretty type string だけの semantic identity
- compile command/variant の silent fallback/first-wins
- macro expansion range への直接 edit
- conflict、stale digest、variant、reparse failure の無視
- unsupported surface、consumer gap、unknown reason の omission
- test に合わせた上位 contract の縮小
- diagnostic prose substring による制御
- shell command の文字列連結
- 運用証跡 namespace、artifact upload、qualification report、issue checkpoint の再導入

## Commands

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang --target <affected-targets>
ctest --preset dev-clang --output-on-failure
cmake --build --preset dev-clang --target cxxlens-quality
```

## LLVM/Clang 22 development installation

この開発環境には exact LLVM/Clang 22.1.0 の release 配布物がユーザー領域へ導入済みで
ある。標準の場所は `/home/dhuru/.local/opt/LLVM-22.1.0-Linux-X64`、実行ファイルの
versioned wrapper は `/home/dhuru/.local/bin/clang++-22` と
`/home/dhuru/.local/bin/clang-format-22` である。`apt` の `llvm-*` package 一覧だけを
見て development package がないと判断してはならない。まず次を確認する。

```sh
LLVM22_ROOT=/home/dhuru/.local/opt/LLVM-22.1.0-Linux-X64
CXXLENS_SOURCE_REVISION="$(git rev-parse HEAD)"
CXXLENS_SOURCE_TREE="$(git rev-parse HEAD^{tree})"
"$LLVM22_ROOT/bin/llvm-config" --version
test -f "$LLVM22_ROOT/lib/cmake/llvm/LLVMConfig.cmake"
test -f "$LLVM22_ROOT/lib/cmake/clang/ClangConfig.cmake"
```

exact adapter を構成するときは CMake に両方の package directory を明示する。

```sh
cmake -S . -B build/dev-clang-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCXXLENS_BUILD_QUALITY_TOOLS=ON -DCXXLENS_CLANG_ADAPTER=ON \
  -DCMAKE_CXX_COMPILER="$LLVM22_ROOT/bin/clang++" \
  -DCXXLENS_CLANG_FORMAT="$LLVM22_ROOT/bin/clang-format" \
  -DLLVM_DIR="$LLVM22_ROOT/lib/cmake/llvm" \
  -DClang_DIR="$LLVM22_ROOT/lib/cmake/clang" \
  -DCXXLENS_SOURCE_REVISION="$CXXLENS_SOURCE_REVISION" \
  -DCXXLENS_SOURCE_TREE="$CXXLENS_SOURCE_TREE"
```

`Enabled exact LLVM/Clang 22.1.0 adapter` が構成出力に現れることを確認する。上記の
場所が存在しない場合だけ、同じ version の release 配布物を導入してから再試行する。
source identity の二つの値は install/package 試験で成果物へ埋め込む値であり、運用証跡
や report ではない。

`cxxlens-quality` は契約を直接 assert し、終了コードだけを返す。変更固有試験と main
全件が green であることが実装完了であり、別の evidence/report/checkpoint は作らない。
