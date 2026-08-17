# Build and test

| Preset | Purpose |
| --- | --- |
| `dev-clang` | Debug build and complete test suite |
| `ci-quick` | warnings-as-errors CI build |
| `docs` | Doxygen and documentation contracts |
| `asan-ubsan` | Address/UndefinedBehavior Sanitizer |
| `tsan` | ThreadSanitizer build |
| `install-check` | Release install and downstream consumers |

通常の implementation issue は **bounded implementation completion** を既定とします。初回 configure の後は、
issue に宣言した対象と直接 dependency closure を build/test します。

```sh
python3 -m pip install --require-hashes --only-binary=:all: \
  --requirement tools/quality/requirements.lock
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang --target <affected-targets>
ctest --preset dev-clang -R '<affected-tests>' --output-on-failure
CXX=clang++ python3 tools/quality/run_gate.py fast --preset dev-clang \
  --report build/dev-clang/fast-report.json
```

public contract、schema、documentation、生成 inventory を変更した場合は、影響する validator/checker を追加します。
`check`、`full`、`stress`、install/native matrix、Nightly/release は、その surface を issue が明示的に所有する場合だけ
issue の完了条件に含めます。

`run_gate.py` は CPU 数と利用可能 memory から build/test parallel level を決め、sanitizer preset では上限を
下げます。`CMAKE_BUILD_PARALLEL_LEVEL` と `CTEST_PARALLEL_LEVEL` を明示して同じ並列度を再現できます。report は
logical check ID/実行回数、configure/build/test/checker ごとの wall time、CTest JUnit、`.ninja_log` 集計、上位 critical
path、revision/tree/worktree digest、toolchain/checker/configuration digest、parallel level、cache 使用有無を記録します。
cache hit は operational data であり pass evidence ではありません。
同じ mode/configuration の前回 report を `--baseline <report.json>` で渡すと、phase ごとの wall time delta を検証付きで
出力します。各 phase は wall time に加えて user/system CPU time を保持します。

## Gate mode

| Mode | Scope | Default owner | Final SHA qualification |
| --- | --- | --- | --- |
| `fast` | slow/process、quality、install を除く unit/integration/public-header smoke | ordinary implementation issue | no |
| `check` | 全 CTest（install を除く）と production quality checker | contract/checker owner or repository guard | no |
| `full` | configured lane の runtime、quality、install。CI aggregate は static/shared と GCC も必須 | merged `main` or explicit integration gate | aggregate only |
| `stress` | configured clean lane の full と deterministic repeat。nightly aggregate は sanitizer も必須 | Nightly/release qualification | aggregate only |

`run_gate.py full|stress` の単一 report は一つの configured lane の証拠であり、それだけで final SHA を認定しません。final
判定は ownership manifest の required configuration 全件を workflow aggregator が受理した場合だけ成立します。
通常の issue close はこの final 判定を再所有せず、bounded scope の実装・直接 evidence・残余 gap の追跡で判定します。

変更選択は `python3 tools/quality/check_quality_ownership.py select -- <paths...>` で説明可能です。bounded implementation
completion では、説明可能な changed-file selection と affected target/test を使用できます。public header、schema、CMake、workflow、
selector 自身、unknown file、dependency graph failure を production qualification の入力にする場合は `full` へ fallback します。
merged `main`、Nightly、release の最終 evidence は changed-file selection を correctness evidence にしません。

## Local/CI correspondence

| CI owner | Local reproduction |
| --- | --- |
| `build-test` static/shared | <code>ctest --preset ci-quick -j N -LE 'quality&#124;install'</code> |
| `quality-contracts` | `ctest --preset ci-quick -j N -L quality` and `cmake --build --preset ci-quick --target cxxlens-quality` |
| `install-consumer` static/shared | `ctest --preset install-check -j N -L install` |
| `gcc-public-headers` | the two `g++ -std=c++23 -fsyntax-only` commands in `quality.yml` |
| nightly sanitizer | `ctest --preset asan-ubsan -j N -LE quality` or `ctest --preset tsan -j 1 -LE quality` |
| nightly clean full | `run_gate.py stress --preset ci-quick --configure` with `CCACHE_DISABLE=1` |

hosted workflow が移行期間中に PR で bounded acceptance より広い repository guard を実行しても、その成功を
`production-qualified` という issue claim に読み替えません。PR の acceptance authority は担当 issue の exact scope と
bounded evidence です。CI workload の tier 移行は integration/readiness gate が所有します。

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

ローカルの LLVM/Clang 22 が標準の CMake package search path にない場合は、隣接する `clang` package を
自動的に解決できる LLVM package directory を明示します。例えば次のように exact package を指定して native
provider lane を構成できます。

```sh
LLVM22_PREFIX=/opt/LLVM-22.1.0-Linux-X64
CXX=clang++ cmake --preset dev-clang \
  -DCXXLENS_CLANG_ADAPTER=ON \
  -DLLVM_DIR="${LLVM22_PREFIX}/lib/cmake/llvm"
```

`LLVM_DIR` が解決できない構成は structured unavailable adapter として扱われ、installed materializer の
positive success test を success に変換しません。native install qualification は exact package を発見した
clean build directory で `ctest --preset install-check -L install` を実行してください。通常の issue が native qualification を
明示的に所有しない場合、利用不能な native lane は production claim を行わず、別 qualification owner に残します。

主要 test label は `unit`、`public-api`、`provider`、`quality`、`install` です。quality unit test は CTest だけが所有し、
`cxxlens-quality` は production checker だけを実行します。

`cxxlens-ng-foundation-completion-check` は authority/schema/version、G0–G4、support/catalog、asset ledger、
legacy-zero を静的に検証します。main への push では build/test、install consumer、GCC public header の成功後に
`quality-contracts` は Clang 22 / Doxygen の取得前に asset ledger と API-development readiness を実行し、repository state の
決定論的な不整合を fail fast します。toolchain を必要とする production checks はこの preflight の後だけ開始します。

`foundation-completion` job が同一 `GITHUB_SHA`、tree、clean checkout と、completion manifest に宣言した
`required_closed_issues`、gate issue、tracking issue の状態を結合した JSON report を artifact として生成します。
宣言 issue の取得失敗や未 close は fail closed です。G5、GR、roadmap など宣言集合外の issue は各 gate が所有し、
Foundation 完了を遡及的に失敗させません。tracked manifest 自身に tree hash を埋め込む自己参照は行いません。

`cxxlens-ng-api-development-readiness-check` は release bundle、実 CMake の public target edge、Public API Catalog による
header admission、Relation Registry による generated header binding、gate owner、workflow job 名、最大四つの active write unit と
unit 間の contract/path 非重複を検証します。write path は同一だけでなく祖先・子孫 ownership も conflict です。
production/readiness qualification が所有する required status check は `build-test (OFF)`、`build-test (ON)`、
`gcc-public-headers`、`install-consumer (OFF)`、`install-consumer (ON)`、`quality-contracts`、`quality-evidence`、`check-tier` の exact set です。
通常の implementation issue はこの集合を自らの completion claim として再所有しません。main 保護または hosted workflow が
merge 前に同じ集合を要求する期間でも、その結果は repository integration guard であり、issue の意味を production qualification へ
拡張しません。main へ SHA が入った後に `foundation-completion` と `wave0-readiness` が実行され、後者は全 artifact、JUnit、
install manifest、toolchain provenance、Foundation report、authority/header digest を clean main revision/tree に bind した
baseline report を生成します。

G5 のローカル再現は次です。

```sh
cmake --build --preset dev-clang --target cxxlens-g5-runtime
python3 tools/quality/check_ng_g5_qualification.py check --root . \
  --runtime build/dev-clang/tests/cxxlens-g5-runtime
```

checker は correctness を再実行した後、2048 partition / 512 edge、`steady_clock`、5 回 median、明示 recursion budget の
performance report に compiler、OS、architecture を保存して envelope を検証します。main の `g5-qualification` job は同じ測定を clean exact SHA と
authority digest に bind します。G5 artifact は R4 の根拠ですが、`gate.release` の代替ではありません。

Distribution 1.0 の GR は main CI の `release-qualification` job が行います。job は同一 SHA の G5、static/shared install、
runtime、security、Doxygen evidence を集約し、次の checker で exact tuple report を生成します。

```sh
python3 tools/quality/check_ng_release_qualification.py check --root .
```

report mode は CI artifact の relocated prefix と manifest を byte digest まで再検証します。production support は report に
列挙された provider/relation/interpretation/toolchain/platform tuple に限られ、source matrix の pending 行は authority ではありません。
通常の implementation issue はこの report の生成を close 条件にせず、production support を claim しないまま bounded completion できます。
