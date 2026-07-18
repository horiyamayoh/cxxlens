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
python3 -m pip install --require-hashes --only-binary=:all: \
  --requirement tools/quality/requirements.lock
CXX=clang++ python3 tools/quality/run_gate.py check --preset dev-clang \
  --configure --report build/dev-clang/check-report.json
```

`run_gate.py` は CPU 数と利用可能 memory から build/test parallel level を決め、sanitizer preset では上限を
下げます。`CMAKE_BUILD_PARALLEL_LEVEL` と `CTEST_PARALLEL_LEVEL` を明示して同じ並列度を再現できます。report は
logical check ID/実行回数、configure/build/test/checker ごとの wall time、CTest JUnit、`.ninja_log` 集計、上位 critical
path、revision/tree/worktree digest、toolchain/checker/configuration digest、parallel level、cache 使用有無を記録します。
cache hit は operational data であり pass evidence ではありません。
同じ mode/configuration の前回 report を `--baseline <report.json>` で渡すと、phase ごとの wall time delta を検証付きで
出力します。各 phase は wall time に加えて user/system CPU time を保持します。

## Gate mode

| Mode | Scope | Final SHA qualification |
|---|---|---|
| `fast` | slow/process、quality、install を除く unit/integration/public-header smoke | no |
| `check` | 全 CTest（install を除く）と production quality checker | no |
| `full` | configured lane の runtime、quality、install。CI aggregate は static/shared と GCC も必須 | aggregate only |
| `stress` | configured clean lane の full と deterministic repeat。nightly aggregate は sanitizer も必須 | aggregate only |

`run_gate.py full|stress` の単一 report は一つの configured lane の証拠であり、それだけで final SHA を認定しません。final
判定は ownership manifest の required configuration 全件を workflow aggregator が受理した場合だけ成立します。

変更選択は `python3 tools/quality/check_quality_ownership.py select -- <paths...>` で説明可能です。public header、schema、
CMake、workflow、selector 自身、unknown file、dependency graph failure は必ず `full` へ fallback します。Ready PR と main
の最終 SHA は changed-file selection を使いません。

## Local/CI correspondence

| CI owner | Local reproduction |
|---|---|
| `build-test` static/shared | <code>ctest --preset ci-quick -j N -LE 'quality&#124;install'</code> |
| `quality-contracts` | `ctest --preset ci-quick -j N -L quality` and `cmake --build --preset ci-quick --target cxxlens-quality` |
| `install-consumer` static/shared | `ctest --preset install-check -j N -L install` |
| `gcc-public-headers` | the two `g++ -std=c++23 -fsyntax-only` commands in `quality.yml` |
| nightly sanitizer | `ctest --preset asan-ubsan -j N -LE quality` or `ctest --preset tsan -j 1 -LE quality` |
| nightly clean full | `run_gate.py stress --preset ci-quick --configure` with `CCACHE_DISABLE=1` |

install は `install.prepare` と `install.relocation` fixture が exact relocated immutable prefix を一度だけ作り、core、SDK、Clang 22 SDK、examples、
runtime layout、legacy-zero consumer を独立 build directory で並列実行します。個別再実行は、例えば
`ctest --preset install-check -R '^install.sdk-consumer$'` です。fixture は自動的に選択されます。SDK unit test も
`unit.sdk.<case>` 単位で選択・`--rerun-failed` できます。
prepare は revision/tree、toolchain、configuration、prefix 全 file digest を install artifact manifest に固定し、各 consumer は
実行前に同じ manifest を再検証します。別 SHA/configuration の prefix、file 差替え、manifest drift は consumer effect 前に失敗します。

logical check の owner、required dimension、input set は `schemas/cxxlens_ng_quality_ownership.yaml` が authority です。
evidence は logical check/version、revision/tree、toolchain/configuration、checker/input/output digest へ bind されます。
mismatch、required evidence 欠落、同一 evidence ID の重複は fail closed です。CI failure artifact の JUnit、Ninja log、
report に記録された exact preset、parallel level、toolchain、command を使って再現してください。

`CXXLENS_CLANG_ADAPTER=AUTO` は exact LLVM/Clang 22 だけを受理し、隣接 major へ fallback しません。
`ON` は exact package がなければ configure error、`OFF` は structured unavailable build です。

主要 test label は `unit`、`public-api`、`provider`、`quality`、`install` です。quality unit test は CTest だけが所有し、
`cxxlens-quality` は production checker だけを実行します。

`cxxlens-ng-foundation-completion-check` は authority/schema/version、G0–G4、support/catalog、asset ledger、
legacy-zero を静的に検証します。main への push では build/test、install consumer、GCC public header の成功後に
`foundation-completion` job が同一 `GITHUB_SHA`、tree、clean checkout と、completion manifest に宣言した
`required_closed_issues`、gate issue、tracking issue の状態を結合した JSON report を artifact として生成します。
宣言 issue の取得失敗や未 close は fail closed です。G5、GR、roadmap など宣言集合外の issue は各 gate が所有し、
Foundation 完了を遡及的に失敗させません。tracked manifest 自身に tree hash を埋め込む自己参照は行いません。
