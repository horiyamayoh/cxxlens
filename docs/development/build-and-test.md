# ビルドとテスト

## Presets

| Preset | 用途 |
|---|---|
| `dev-clang` | 開発用 Debug build、tests、compile commands |
| `ci-quick` | CI の warnings-as-errors build |
| `docs` | Doxygen HTML/XML と契約検査 |
| `asan-ubsan` | Address/UndefinedBehavior Sanitizer |
| `install-check` | Release build と install consumer test |

```sh
python3 -m pip install --requirement tools/quality/requirements.txt
npm install --global markdownlint-cli2@0.18.1
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang
```

Clang 22 が導入された CI では `CXX=clang++-22` を使用する。M0 の library 自体は LLVM library を
リンクしないため、ローカルでは C++23 対応 Clang/GCC でも基盤を検証できる。

## Test labels

- `unit`: value semantics と契約
- `public-api`: public header 単体コンパイルと依存境界
- `documentation`: Doxygen 契約と example syntax
- `install`: install/export と downstream consumer

source tree 内 build は CMake configure 時に拒否する。
