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
| `m0-acceptance` | 移行元 M0 behavior baseline |
| `m1-acceptance` | 移行元 exact Clang 22 production-path baseline |
| `m2-acceptance` | 移行元 installed flagship baseline |

```sh
python3 -m pip install --requirement tools/quality/requirements.txt
npm install --global markdownlint-cli2@0.18.1
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang
cmake --build --preset dev-clang --target cxxlens-quality
```

documentation/authority のみを短時間で検査する場合は次を実行します。

```sh
cmake --preset docs
cmake --build --preset docs --target cxxlens-documentation-consistency-check
cmake --build --preset docs --target cxxlens-ng-authority-check
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

## NG acceptance direction

次世代 gate は [Acceptance Manifest](../../schemas/cxxlens_ng_acceptance_manifest.yaml) の G0〜GR です。既存
M0/M1/M2 target が成功しても NG0 completion にはなりません。各 NG gate は relation/query/store/provider
contract の owner issue で production path へ接続し、最終的に #71 の commit-bound report へ統合します。

## Legacy ThreadSanitizer baseline

`tsan` preset は `CXXLENS_CLANG_ADAPTER=OFF` を固定し、LLVM-independent foundation を検査する。
nightly は clean configure/build 後、意図的 data race fixture で TSan runtime が exit code 66 と
race diagnostic を返すことを先に確認する。その後、全 foundation suite を一度実行し、
`concurrency` label を jobs 1/2/8 と固定 seed を含む test seam で5回反復する。これには #34 の
scheduler identity/coalescing、#36 の concurrent process launch、#37 の timeout/cancel/crash
isolation、fact store の snapshot read/single writer、provisioning publication が含まれる。
`handle_segv=0` は legacy worker signal を TSan 自身の exit code に置換させないための runtime
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
