# ビルドとテスト

## Presets

| Preset | 用途 |
|---|---|
| `dev-clang` | 開発用 Debug build、tests、compile commands |
| `ci-quick` | CI の warnings-as-errors build |
| `docs` | Doxygen HTML/XML と契約検査 |
| `asan-ubsan` | Address/UndefinedBehavior Sanitizer |
| `tsan` | LLVM-independent foundation の ThreadSanitizer gate |
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
- `concurrency`: scheduler、store、provisioning、process runtime の並列回帰

## ThreadSanitizer nightly gate

`tsan` preset は `CXXLENS_CLANG_ADAPTER=OFF` を固定し、LLVM-independent foundation を検査する。
nightly は clean configure/build 後、意図的 data race fixture で TSan runtime が exit code 66 と
race diagnostic を返すことを先に確認する。その後、全 foundation suite を一度実行し、
`concurrency` label を jobs 1/2/8 と固定 seed を含む test seam で5回反復する。これには #34 の
scheduler identity/coalescing、#36 の concurrent process launch、#37 の timeout/cancel/crash
isolation、fact store の snapshot read/single writer、provisioning publication が含まれる。
`handle_segv=0` は #37 の real worker signal を TSan 自身の exit code に置換させないための runtime
設定であり、race report は `halt_on_error=1:exitcode=66` で fail-closed にする。

```sh
CXX=clang++-22 cmake --fresh --preset tsan
cmake --build --preset tsan
ctest --preset tsan -R '^sanitizer\.tsan-detection$'
ctest --preset tsan -E '^sanitizer\.tsan-detection$'
ctest --preset tsan -L concurrency --repeat until-fail:5
```

TSan log は成功・失敗を問わず nightly artifact として14日保持する。既知の false positive は現在
0件である。suppression は `tests/tsan/tsan.supp` と `tests/tsan/suppressions.yaml` の双方に登録し、
対象 symbol/library、狭い suppression type、理由、期限、再検証条件を必須とする。blanket
suppression や metadata のない entry は quality gate が拒否する。Clang 22 linked adapter はこの
必須 gate には含めず、upstream runtime を含む安定した allowlist が確立した後に別 matrix で追加する。

source tree 内 build は CMake configure 時に拒否する。
