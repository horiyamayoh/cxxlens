# cxxlens

`cxxlens` は、静的解析器・言語解析器を構築するための C++23 Semantic Relation Platform です。
versioned relation、semantic claim、immutable snapshot、typed/dynamic query、provider protocol を共通基盤とし、
recipe 利用者から portable/native provider 開発者まで同じ identity・validation・partiality 契約を利用できます。

通常の public API は LLVM/Clang に依存しません。Clang 22 native API は専用 package に分離され、AST object は
同期 callback の外へ保存・所有・thread 移送できません。

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
|---|---|
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
- [Tutorials](docs/tutorials/README.md)
- [Support matrix](docs/support-matrix.md)

旧アーキテクチャは production tree から削除済みです。履歴資料だけを
[archive](docs/archive/README.md) に非規範資料として保存しています。
