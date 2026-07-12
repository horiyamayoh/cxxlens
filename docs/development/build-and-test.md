# ビルドとテスト

## Presets

| Preset | 用途 |
|---|---|
| `dev-clang` | 開発用 Debug build、tests、compile commands |
| `ci-quick` | CI の warnings-as-errors build |
| `docs` | Doxygen HTML/XML と契約検査 |
| `asan-ubsan` | Address/UndefinedBehavior Sanitizer |
| `tsan` | Thread Sanitizer と borrowed lifetime test |
| `install-check` | Release build と install consumer test |
| `m0-acceptance` | M0 semantic kernel completion gate |
| `m1-acceptance` | M1 exact Clang 22 production-path gate |
| `m2-acceptance` | M2 installed flagship search gate |

```sh
python3 -m pip install --requirement tools/quality/requirements.txt
npm install --global markdownlint-cli2@0.18.1
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang
```

Clang 22 が導入された CI では `CXX=clang++-22` を使用する。`CXXLENS_CLANG_ADAPTER=AUTO` は
exact LLVM/Clang 22 development package だけを受理し、隣接 major へ fallback しない。
`ON` は exact package がなければ configure error、`OFF` は明示的 unavailable build になる。

## Test labels

- `unit`: value semantics と契約
- `public-api`: public header 単体コンパイルと依存境界
- `documentation`: Doxygen 契約と example syntax
- `install`: install/export と downstream consumer

source tree 内 build は CMake configure 時に拒否する。
