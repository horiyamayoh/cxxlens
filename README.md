# cxxlens

`cxxlens` は、実際の C/C++ build context に基づく versioned semantic claim を provider から収集し、
condition、provenance、partiality とともに immutable snapshot へ公開する C++23 Semantic Relation
Platform です。Recipe 利用者だけでなく、typed/dynamic query、portable provider、Clang
major-specific native provider の作者にも共通の identity、schema、validation、source mapping、
failure isolation、conformance harness を提供します。

> [!WARNING]
> 現在は pre-alpha で、Issue #56 の NG Foundation Migration 中です。既存実装と M0/M1/M2 gate は
> 移行 baseline として保持されていますが、旧 124 API freeze と Issue #55 は新規実装を認可しません。
> また、ライセンスは未決定です。

## 要件

- CMake 3.25 以上
- C++23 対応コンパイラ
- 主検証環境: Linux / Clang 22
- Doxygen 1.9.8 以上（ドキュメント生成時のみ）
- Python 3.10 以上（品質検査時のみ）

Clang 22 は最初の公式 source provider baseline です。次世代 kernel は compiler library を link せず、
major-specific worker を process 境界の外側に置きます。現在の linked adapter は移行元実装であり、
Issue #70 で worker/provider protocol へ置き換えます。

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

既存挙動を固定する legacy M0 baseline は、依存ツールを導入した clean checkout で次の target から
検証できます。これは次世代 NG0 completion の宣言ではありません。

```sh
CXX=clang++ cmake --preset m0-acceptance
cmake --build --preset m0-acceptance --target cxxlens-m0-acceptance
```

## Author SDK

generated/dynamic query、detached relation/snapshot、portable provider、conformance harness は LLVM/Clang に
依存しない独立 package から利用できます。

```cmake
find_package(cxxlensProviderSDK 0.1 CONFIG REQUIRED)
target_link_libraries(my_tool PRIVATE cxxlens::provider_sdk)
target_compile_features(my_tool PRIVATE cxx_std_23)
```

```cpp
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/sdk.hpp>

auto query = cxxlens::sdk::query::from<cxxlens::cc::relations::call_site>();
```

Clang 22 native helper は `find_package(cxxlensClang22ProviderSDK)` と
`cxxlens::clang22_provider_sdk` へ明示 opt-in します。5経路の runnable example は
[SDK tutorials](docs/tutorials/README.md)、exact contract は
[Public C++ API Catalog](schemas/cxxlens_ng_public_api_catalog.yaml) を参照してください。

## 文書と machine-readable contract

- [文書ポータル](docs/README.md) — current / migration / archive の境界
- [次世代統合設計書](docs/design/cxxlens_next_generation_integrated_design_ja.md) — 最上位規範
- [次世代 catalog/registry](docs/design/catalogs/README.md)
- [アーキテクチャ](docs/development/architecture.md)
- [ビルドとテスト](docs/development/build-and-test.md)
- [Support matrix](docs/support-matrix.md)
- [Security profile](schemas/cxxlens_ng_security_profile.yaml)
- [Asset migration ledger](schemas/cxxlens_asset_migration_ledger.json)
- [コントリビューション](CONTRIBUTING.md)

旧統合設計、旧 124 API catalog/freeze、旧二層構想は [archive](docs/archive/README.md) と legacy
baseline に隔離されています。新規判断へ復活させず、
[authority transition](docs/design/adr/0004-legacy-contract-reset.md) と各 NG catalog を使用してください。

## 非目標

- LLVM/Clang の全 API を別名でラップすること
- AST pointer を長期間またはスレッドを跨いで保持すること
- ビルド構成や未解決事項を隠して成功として報告すること
- 安全性検証を省略した直接的なソース書き換え
