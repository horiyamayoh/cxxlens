# Build and test

| Preset | Purpose |
|---|---|
| `dev-clang` | Debug build and complete test suite |
| `ci-quick` | warnings-as-errors CI build |
| `docs` | Doxygen and documentation contracts |
| `asan-ubsan` | Address/UndefinedBehavior Sanitizer |
| `tsan` | ThreadSanitizer build |
| `install-check` | Release install and downstream consumers |

```sh
python3 -m pip install --requirement tools/quality/requirements.txt
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang
cmake --build --preset dev-clang --target cxxlens-quality
```

`CXXLENS_CLANG_ADAPTER=AUTO` は exact LLVM/Clang 22 だけを受理し、隣接 major へ fallback しません。
`ON` は exact package がなければ configure error、`OFF` は structured unavailable build です。

主要 test label は `unit`、`public-api`、`provider`、`quality`、`install` です。install test は
3 package の downstream consumer を build/run し、旧 header/schema/worker が install されないことも検査します。

`cxxlens-ng-foundation-completion-check` は authority/schema/version、G0–G4、support/catalog、asset ledger、
legacy-zero を静的に検証します。main への push では build/test、install consumer、GCC public header の成功後に
`foundation-completion` job が同一 `GITHUB_SHA`、tree、clean checkout、child issue 状態を結合した JSON report を
artifact として生成します。tracked manifest 自身に tree hash を埋め込む自己参照は行いません。
