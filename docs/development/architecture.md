# 開発アーキテクチャ

`cxxlens` は利用者へ単一 target `cxxlens::cxxlens` を公開する。内部は package ごとに分割するが、
利用者に LLVM wrapper と上位ライブラリの選択を要求しない。

```text
public use-case API
  -> domain models / immutable plans
    -> internal ports
      -> LLVM-major-specific adapters
```

Clang-free port は `src/llvm/common/`、native adapter は `src/llvm/clang22/` に隔離する。
通常 public header は LLVM-free で、raw access は明示的な `interop/clang.hpp` callback 内だけに
限定する。

## Dependency rules

- `include/cxxlens/interop/clang.hpp` 以外の public header は Clang/LLVM 型を公開しない。
- stable API は LLVM major や patch version の型レイアウトに依存しない。
- service handle は将来 pImpl とし、semantic result/plan は immutable value とする。
- public expected failure は `std::expected<T, error>` で表す。
- `src` の domain logic を `utils.cpp` や `helpers.cpp` に集約しない。

正確な package 一覧、data flow、禁止事項は[統合設計書](../design/cxxlens_integrated_design_ja.md)を参照する。
