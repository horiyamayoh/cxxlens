# cxxlens

`cxxlens` は、実際の C/C++ build context に基づく versioned semantic claim を provider から収集し、
condition、provenance、partiality とともに immutable snapshotへ公開する C++23 Semantic Relation
Platformです。Recipeに加え、typed/dynamic query、portable provider、Clang major-specific native
providerを第一級の開発経路として提供します。

> [!WARNING]
> 現在は pre-alphaで、Issue #56 の NG Foundation Migration中です。旧M0/M1/M2実装は移行
> baselineとして保持されていますが、旧124 API freezeとIssue #55は新規実装を認可しません。
> また、ライセンスは未決定です。

## 要件

- CMake 3.25 以上
- C++23 対応コンパイラ
- 主検証環境: Linux / Clang 22
- Doxygen 1.9.8 以上（ドキュメント生成時のみ）
- Python 3.10 以上（品質検査時のみ）

LLVM/Clang 22.1.8 は設計上の初期 baseline です。exact major が見つかれば adapter をリンクし、
見つからなければ通常 API を削除せず structured unavailable を返します。

## ビルドとテスト

```sh
cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang
```

環境に `clang-22` がない場合は、`CXX` を利用可能な C++23 コンパイラへ指定できます。

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang
```

品質検査とドキュメント生成は次の target から実行します。

```sh
python3 -m pip install --requirement tools/quality/requirements.txt
npm install --global markdownlint-cli2@0.18.1
cmake --build --preset dev-clang --target cxxlens-quality
cmake --preset docs
cmake --build --preset docs --target docs
```

M0 Semantic Kernel の全 acceptance criteria は、依存ツールを導入した clean checkout で
次の一つの build targetから検証できます。

```sh
CXX=clang++ cmake --preset m0-acceptance
cmake --build --preset m0-acceptance --target cxxlens-m0-acceptance
```

## インストール後の利用

```cmake
find_package(cxxlens 0.1 CONFIG REQUIRED)
target_link_libraries(my_tool PRIVATE cxxlens::cxxlens)
target_compile_features(my_tool PRIVATE cxx_std_23)
```

```cpp
#include <cxxlens/cxxlens.hpp>

const auto product_versions = cxxlens::versions();
```

## 文書

- [次世代統合設計書](docs/design/cxxlens_next_generation_integrated_design_ja.md) — 最上位の規範文書
- [設計パッケージ](docs/design/README.md)
- [アーキテクチャ](docs/development/architecture.md)
- [ビルドとテスト](docs/development/build-and-test.md)
- [コーディング規約](docs/development/coding-style.md)
- [Doxygen 規約](docs/development/doxygen-style.md)
- [M0 acceptance gate](docs/development/m0-acceptance.md)
- [Workspace catalog contract](docs/development/workspace-catalog.md)
- [Observation and immutable fact contract](docs/development/fact-contract.md)
- [Clang 22 frontend adapter contract](docs/development/clang-adapter.md)
- [コントリビューション](CONTRIBUTING.md)

旧統合設計、旧124 API catalog/freeze、旧二層構想は移行 provenanceです。新規設計・実装判断には
使用せず、[authority transition](docs/design/adr/0004-legacy-contract-reset.md)に従います。

## 非目標

- LLVM/Clang の全 API を別名でラップすること
- AST pointer を長期間またはスレッドを跨いで保持すること
- ビルド構成や未解決事項を隠して成功として報告すること
- 安全性検証を省略した直接的なソース書き換え
