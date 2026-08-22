# Build and test

開発完了は変更固有の試験と `main` の全決定的回帰試験の成功で判定します。試験用
JUnit、timing JSON、toolchain provenance、checksum、qualification report、集約 report は
生成・保存しません。GitHub Actions の job log は通常の platform 機能として残ります。

## Local deterministic suite

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang --output-on-failure
cmake --build --preset dev-clang --target cxxlens-quality
```

`cxxlens-quality` は product contract、security、public-header、documentation の assertion
を実行し、失敗時は非ゼロ終了するだけです。`ctest` は label 選択なしで登録された決定的
試験を全件実行します。変更時は affected test を先に実行し、最後に全件を実行します。

## Exact LLVM/Clang 22 native adapter

この環境の exact LLVM/Clang 22.1.0 development installation は
`/home/dhuru/.local/opt/LLVM-22.1.0-Linux-X64` にある。`llvm-22` の system package が
見つからない場合でも、この配布物を先に確認する。CMake config と library が同梱されて
いるため、`LLVM_DIR` と `Clang_DIR` を明示すれば native adapter を有効化できる。

```sh
LLVM22_ROOT=/home/dhuru/.local/opt/LLVM-22.1.0-Linux-X64
CXXLENS_SOURCE_REVISION="$(git rev-parse HEAD)"
CXXLENS_SOURCE_TREE="$(git rev-parse HEAD^{tree})"
cmake -S . -B build/dev-clang-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCXXLENS_BUILD_QUALITY_TOOLS=ON -DCXXLENS_CLANG_ADAPTER=ON \
  -DCMAKE_CXX_COMPILER="$LLVM22_ROOT/bin/clang++" \
  -DCXXLENS_CLANG_FORMAT="$LLVM22_ROOT/bin/clang-format" \
  -DLLVM_DIR="$LLVM22_ROOT/lib/cmake/llvm" \
  -DClang_DIR="$LLVM22_ROOT/lib/cmake/clang" \
  -DCXXLENS_SOURCE_REVISION="$CXXLENS_SOURCE_REVISION" \
  -DCXXLENS_SOURCE_TREE="$CXXLENS_SOURCE_TREE"
cmake --build build/dev-clang-native -j2
ctest --test-dir build/dev-clang-native --output-on-failure
```

構成ログに `Enabled exact LLVM/Clang 22.1.0 adapter` が必要である。`CXXLENS_CLANG_ADAPTER=ON`
で package 未検出になる場合は、まずこの install root と二つの CMake directory を指定する。
install/package 試験では source revision/tree も明示する。これはインストール成果物の
source identity を安全に固定するための製品条件であり、開発・release の運用証跡ではない。

## Main workflow

`.github/workflows/quality.yml` は次を実行します。

- Clang 22 static/shared の build と全決定的 CTest
- contract/security/documentation checks
- static/shared の installed consumer と relocated prefix
- GCC による LLVM-independent public header compile

path selection、fast gate、report/baseline、artifact upload/download、結果集約 job はありません。

## Release workflow

`.github/workflows/release.yml` は手動実行または `v*` tag でのみ動き、main 全件に加えて
ASan/UBSan、TSan、clang-tidy、stress/repeat、最大 materialization scale、real-project、
relocated-install を直接実行します。重検査が一件でも失敗した場合、package job は実行されません。
試験 artifact を保存せず、tag の package だけを公開します。

```sh
cmake --preset asan-ubsan && cmake --build --preset asan-ubsan
ctest --preset asan-ubsan --output-on-failure
cmake --preset tsan && cmake --build --preset tsan
ctest --preset tsan --output-on-failure
```

## Compatibility

Compatibility request/report は v2 です。`os`、`architecture`、`toolchain`、`linkage` を
`schemas/cxxlens_support_matrix.yaml` と照合し、未掲載環境と Windows/MSVC は `unsupported`
とします。`runtime_qualified`、`evidence_refs`、`qualification_state`、
`compat.release-not-qualified` は使いません。

## Product runtime data

claim/provenance、coverage、unknown、materialization report、SQLite/source-closure の安全
receipt、provider の署名・binary identity・失効・sandbox・canonical semantic certification
は製品の実行結果です。開発・release の運用証跡廃止によって削除・簡略化してはいけません。
