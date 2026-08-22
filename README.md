# cxxlens

> **cxxlens は、コンパイラの観測結果を、再現可能・問い合わせ可能・証拠付きの意味知識へ変換し、分からない場合には何が足りないかまで返す基盤である。**

`cxxlens` は、静的解析器・言語解析器・意味検索・移行・リファクタリング支援を構築するための
C++23 Semantic Relation Platform です。versioned relation、semantic claim、immutable snapshot、
typed/dynamic query、provider protocol を共通基盤とし、recipe 利用者から portable/native provider
開発者まで同じ identity・validation・partiality 契約を利用できます。

cxxlens が返すのは単なる row や finding ではありません。結果は、適用できる範囲で
`proved`、`disproved`、`unknown`、`partial`、`conflicting` を区別し、coverage、closure、
unresolved、conflict、differential disagreement、guarantee、provenance、logical/physical
explain を保持します。分からない場合は、足りない source/build input、capability、model、
provider evidence と、結果を強化するための dependency-ordered completion plan を返せることを
製品方向とします。

通常の public API は LLVM/Clang に依存しません。Clang 22 native API は専用 package に分離され、
AST object は同期 callback の外へ保存・所有・thread 移送できません。cxxlens は Clang AST API を
網羅的に薄く包むことや、公開 API 数を増やすこと自体を目的にしません。compiler-native observation を
detached semantic knowledge へ変換し、独立 consumer が再利用できる versioned capability として提供します。

## Development direction

完成は二方向から判定します。

- **Supply-side closure:** admitted public surface が contract、implementation、validation、evidence を持つ。
- **Demand-side closure:** admitted use case から必要 capability、provider、relation、analysis、evidence まで
  実行可能な経路があるか、明示された tracked gap がある。
- **Complete:** orphan public surface と orphan admitted use case の双方が存在しない。

今後の優先順は、real-project source closure/capture/replay、semantic graph、CFG/control exit、
use-def/value flow、alias/effect/invalidation、interprocedural summary/model pack、
proof-carrying rewrite/artifact、cross-provider semantic consensus です。個別 capability は kernel を
肥大化させず、versioned relation/provider/analysis/model/recipe として追加します。

machine-readable な製品 contract は `schemas/cxxlens_ng_*` と
`schemas/cxxlens_support_matrix.yaml` にあります。開発・release の運用方針は
[ADR 0106](docs/design/adr/0106-test-only-development-and-release-policy.md) にあります。

## Build

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang
cmake --build --preset dev-clang --target cxxlens-quality
```

Clang 22 development package がない環境では native provider は structured unavailable 実装になります。
exact Clang 22 を必須にする場合は `-DCXXLENS_CLANG_ADAPTER=ON` を指定します。

## Packages and targets

| Package | Targets |
| --- | --- |
| `cxxlens` | `cxxlens::base`, `cxxlens::kernel`, `cxxlens::query`, `cxxlens::cpp`, `cxxlens::recipes`, `cxxlens::cxxlens` |
| `cxxlensProviderSDK` | `cxxlens::provider_sdk` |
| `cxxlensClang22ProviderSDK` | `cxxlens::clang22_provider_sdk` |

```cmake
find_package(cxxlensProviderSDK 1.0 CONFIG REQUIRED)
target_link_libraries(my_analyzer PRIVATE cxxlens::provider_sdk)
target_compile_features(my_analyzer PRIVATE cxx_std_23)
```

## Authority

- [次世代統合設計](docs/design/cxxlens_next_generation_integrated_design_ja.md)
- [Public API catalog](schemas/cxxlens_ng_public_api_catalog.yaml)
- [Relation registry](schemas/cxxlens_ng_relation_registry.yaml)
- [開発アーキテクチャ](docs/development/architecture.md)
- [Extending the platform](docs/development/extending-platform.md)
- [Tutorials](docs/tutorials/README.md)
- [Support matrix](docs/support-matrix.md)

旧アーキテクチャは production tree から削除済みです。履歴資料だけを
[archive](docs/archive/README.md) に非規範資料として保存しています。
