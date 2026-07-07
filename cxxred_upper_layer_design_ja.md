# C/C++ ユースケース駆動アプリケーションライブラリ設計書

**仮称:** `cxxred`  
**位置づけ:** LLVM/Clang 公式 API を直接包む下位ラッパ `cxxlens` のさらに上に載る、ユースケース駆動の C/C++ コード解析・品質保証・変換・テスト支援アプリケーションフレームワーク  
**対象実装言語:** C++23  
**解析対象:** C/C++、C++03 以降、C++20/23/26 系機能、C 言語、GNU/MSVC/Clang 拡張、プリプロセッサ、テンプレート、モジュール、複数ビルド構成  
**作成日:** 2026-07-06  
**状態:** 実装前の要求仕様、外部設計、内部詳細設計、公開 API 設計

---

## 0. エグゼクティブサマリー

前回設計した `cxxlens` は、Clang/LLVM の LibTooling、ASTMatchers、Transformer、clang-tidy、Static Analyzer、dataflow、clangd index、Sanitizer、LLVM IR などを **安全で一貫した低水準プリミティブ**として扱うための下位ラッパである。

今回設計する `cxxred` は、そのさらに上に位置する。`cxxred` の入力は「Clang の API」ではなく、**人間や AI コーディングエージェントが達成したいユースケース**である。

たとえば、次のような要求をそのまま API として表現できるようにする。

```cpp
auto report = cxxred::grep::calls_to_method()
  .receiver_type("my::Service")
  .include_derived_types(true)
  .include_virtual_overrides(true)
  .method_name("start")
  .confidence_at_least(cxxred::confidence::probable)
  .run(workspace);
```

これは正規表現検索ではない。内部では `cxxlens::query`、`cxxlens::ast`、`cxxlens::index`、必要に応じて dataflow や call graph を組み合わせる。結果には「なぜマッチしたか」「どの型解決・継承関係・仮想ディスパッチ候補に基づくか」「どこまで確実か」という evidence を含める。

`cxxred` の本質は、万能解析器そのものではなく、**万能解析器を作りやすくするためのユースケース別ライブラリ群**である。設計目標は次の通りである。

1. **ユースケースを第一級概念にする。** 「メソッド呼び出しを探す」「危険 API から安全 API へ置換する」「ある公開クラスの fake を生成する」「diff 上でレビュー gate をかける」などを API 名と型にする。
2. **正規表現を標準経路にしない。** 構文・型・名前解決・継承・オーバーロード・プリプロセッサ・ビルド構成を使う。文字列検索は補助的な narrowing に限る。
3. **精度と限界を隠さない。** C++ の完全解析は困難な領域があるため、`confidence`、`evidence`、`approximation`、`unresolved_reason` を結果 schema に含める。
4. **下位層に降りられる。** `cxxred` が未対応でも、`cxxlens`、さらに Clang raw API へ降りる経路を公式 API として持つ。
5. **AI にとって使いやすい。** API は意図が読み取れる名前にし、すべての recipe は `explain()`、`schema()`、`examples()`、`why_matched()`、`why_not_matched()`、`dry_run()` を持つ。
6. **品質保証のスケーリングを前提にする。** 解析、静的検査、動的検査、変換、mock/fake 生成、CI gate、SARIF/JSON 出力を同じ workflow model で扱う。

`cxxred` は `cxxlens` を置き換えない。むしろ、`cxxlens` の上に「よくある困難な作業」を recipe、selector、policy、workflow として大量に載せることで、初心者にも AI にも「最初から正しい足場」を与える。

---

## 1. 二階層構造の整理

### 1.1 レイヤ構造

```text
┌──────────────────────────────────────────────────────────────┐
│ Layer 2: cxxred                                              │
│ ユースケース駆動アプリケーションフレームワーク                  │
│ semantic grep / rule packs / codemods / mock / dynamic QA / CI │
└──────────────────────────────────────────────────────────────┘
                              │ uses
┌──────────────────────────────────────────────────────────────┐
│ Layer 1: cxxlens                                              │
│ LLVM/Clang の安全・一貫・型付き下位ラッパ                       │
│ project/session/ast/query/analysis/dataflow/transform/index    │
└──────────────────────────────────────────────────────────────┘
                              │ wraps
┌──────────────────────────────────────────────────────────────┐
│ Layer 0: LLVM/Clang official APIs                             │
│ LibTooling / ASTMatchers / Transformer / clang-tidy / CSA      │
│ clangd index / LibFormat / Sanitizers / LLVM IR / Pass Manager │
└──────────────────────────────────────────────────────────────┘
```

### 1.2 下位層 `cxxlens` と上位層 `cxxred` の責務分担

| 観点 | `cxxlens` | `cxxred` |
|---|---|---|
| 入力 | Clang/LLVM の概念。TU、AST、matcher、edit、diagnostic、index。 | 利用者の意図。grep、調査、規則、修正、生成、CI gate、レビュー。 |
| 抽象度 | 低〜中。AST/query/edit/dataflow などのプリミティブ。 | 中〜高。ユースケース別 recipe、workflow、policy。 |
| 主な利用者 | Clang 経験者、解析ツール開発者、下位機能を直接制御したい人。 | アプリ開発者、QA 担当、AI エージェント、clang 初心者。 |
| 失敗時 | Clang エラーや解析限界を整形して返す。 | 「何が不足したか」「どう精度が落ちたか」「次に何を指定すべきか」を説明する。 |
| 未対応回避 | `raw()` / `with_clang()` で公式 API へ降りる。 | `with_cxxlens()` / `custom_recipe()` / `custom_fact()` で下位層へ降りる。 |
| 出力 | match、diagnostic、edit_plan、index、dynamic_result。 | findings、impact_report、review_report、policy_report、mock_plan、codemod_plan。 |
| API の形 | 構文・意味 primitive。 | use case / recipe / selector / policy / workflow。 |

### 1.3 `cxxred` の非目標

`cxxred` は次を非目標とする。

- C++ の完全な意味論を LLVM/Clang と別に再実装すること。
- 未検証の推測結果を「確実」として返すこと。
- 全ての解析を単一 API で完璧に行うこと。
- 下位層や Clang raw API への脱出口を塞ぐこと。
- 正規表現検索や grep を禁止すること。ただし、C/C++ の意味解析が必要な場面で正規表現を標準手段にしない。

---

## 2. 設計の前提となる公式機能

この設計は、Clang/LLVM が公式に提供している次の機能を前提にする。

- Clang は LibTooling、Libclang、LibFormat、Plugins、RecursiveASTVisitor、LibASTMatchers、Clang Transformer、ASTImporter、JSON Compilation Database、Refactoring Engine などを「ライブラリとして使う」経路として文書化している。[^clang-docs]
- LibTooling は Clang ベースの standalone tool を書くための基盤で、`FrontendAction` をコードに対して実行するモデルを持つ。[^libtooling]
- AST Matcher はノード matcher、narrowing matcher、traversal matcher に分類され、AST 上の構造的検索を構成するための DSL である。[^ast-matcher-reference]
- Clang Transformer は AST matcher に基づく source-to-source transformation と diagnostics を書くための framework で、rewrite rule を中核概念にする。[^transformer]
- clang-tidy は LibTooling ベースの C++ linter framework で、style、interface misuse、静的に推論できる bug などの check と fix を扱う。[^clang-tidy]
- Clang Static Analyzer は C/C++/Objective-C に対する path-sensitive、inter-procedural、symbolic execution ベースの解析を提供する。[^csa]
- Clang dataflow 文書は、CFG 上で事実を伝播し fixpoint に到達させる静的解析を、refactoring や bug finding に適用できる技術として説明している。[^dataflow]
- JSON Compilation Database は、translation unit ごとのコンパイル方法を再現するための形式で、同一ファイルに複数 command が存在し得る。[^json-cdb]
- Include Cleaner は transitive include に依存しがちな C++ で、使用 symbol と定義 header を対応付け、未使用/不足 include の診断を目指す。[^include-cleaner]
- Clang source-based coverage は AST と preprocessor 情報に基づく精密な coverage を提供する。[^source-coverage]
- AddressSanitizer は compiler instrumentation と runtime で構成される memory error detector である。[^asan]
- libFuzzer は in-process、coverage-guided、evolutionary fuzzing engine で、LLVM SanitizerCoverage と組み合わせて coverage を増やす。[^libfuzzer]
- DataFlowSanitizer は特定バグ検出器ではなく、利用者が tag を付けて propagation を追跡できる汎用動的 data flow analysis framework である。[^dfsan]
- SARIF は静的解析結果を相互運用するための標準形式であり、複数ツールの結果を統合しやすくする。[^sarif]
- CodeQL の C/C++ data flow ライブラリは local data flow、global data flow、taint tracking を区別する。この区別は `cxxred` の dataflow/taint API 設計にも採用する。[^codeql-dataflow]

`cxxred` はこれらを直接包むのではなく、下位層 `cxxlens` が包んだ機能を、ユースケース別に再構成して利用する。

---

## 3. `cxxred` の最重要設計原則

### 3.1 Use-case first

利用者が書くべきコードは、Clang の内部構造ではなく「やりたいこと」に近いべきである。

悪い例:

```cpp
// 利用者が最初から matcher の組み合わせを考える必要がある。
q::member_call_expr()
  .has(q::callee().has(q::declaration().qualified_name("ns::Service::start")))
  .has(q::implicit_object_argument().has_type(...));
```

良い例:

```cpp
grep::calls_to_method()
  .receiver_type("ns::Service")
  .method_name("start")
  .include_derived_types(true)
  .include_virtual_overrides(true);
```

内部では前者に展開されてもよいが、上位 API はユースケースを表す。

### 3.2 Evidence first

C++ 解析では、完全に確実な結果と近似結果が混在する。上位 API は結果だけでなく、根拠を返す。

```cpp
for (auto f : report.findings()) {
  std::cout << f.primary_location() << "\n";
  std::cout << f.confidence() << "\n";
  std::cout << f.evidence().to_markdown() << "\n";
}
```

Evidence には次を含める。

- AST match の binding。
- canonical declaration。
- receiver expression の static type。
- overload resolution の結果。
- virtual override 候補。
- include/header 由来。
- dataflow path。
- macro spelling/expansion 情報。
- 近似を行った場合の approximation reason。

### 3.3 Precision ladder

単一 API の中で、解析精度を段階的に上げられるようにする。

| レベル | 名称 | 内容 | 速度 | 精度 |
|---|---|---|---|---|
| 0 | lexical assist | 名前・ファイル・範囲などの軽量 narrowing。正規表現ではなく補助 filter。 | 最速 | 低〜中 |
| 1 | AST structural | ASTMatcher/RAV による構造検索。 | 高速 | 中〜高 |
| 2 | semantic local | 型、名前解決、overload、canonical decl、CFG。 | 中 | 高 |
| 3 | workspace fact | index、継承、参照、call graph、include graph。 | 中〜遅 | 高 |
| 4 | dataflow | local/global dataflow、taint、guard/barrier。 | 遅 | 高。ただし model 依存 |
| 5 | path-sensitive | Clang Static Analyzer、symbolic execution。 | 遅 | 特定領域で高 |
| 6 | dynamic evidence | Sanitizer、coverage、fuzzing、runtime trace。 | ビルド/実行依存 | 実行された経路で高 |

各 recipe は既定の precision を持つが、利用者は `precision()` で制御できる。

### 3.4 Failure is data

「解析できなかった」は単なる `false` ではない。結果として返す。

```cpp
struct unresolved_reason {
  unresolved_kind kind;
  std::string message;
  std::vector<cxxlens::source_range> related_ranges;
  std::vector<std::string> suggested_actions;
};
```

代表例:

- compile command が不足している。
- header の TU が推定できない。
- macro expansion のため安全編集できない。
- dependent type のため template instantiation が必要。
- virtual dispatch の動的型が不明。
- function pointer / callback の解決に points-to 近似が必要。
- generated code の出力元が不明。
- module BMI がない。
- sanitizer build が link できない。

### 3.5 No dead end

`cxxred` が対応していないことは、上位 API の終端ではない。次の 4 つの脱出口を必ず提供する。

```cpp
recipe.with_cxxlens([](cxxlens::analysis_session& s) { ... });
selector.from_cxxlens_query(cxxlens::query q);
facts.add_custom_extractor(custom_fact_extractor{});
workflow.add_step(custom_step{});
```

下位層 `cxxlens` 自体も `raw()` / `with_clang()` を持つため、最終的には LLVM/Clang 公式 API へ降りられる。

### 3.6 Dry-run by default

ファイル変更、mock 生成、codemod、include cleanup、test harness 生成は、既定では dry-run で edit plan を返す。

```cpp
auto plan = refactor::replace_api(...).plan(workspace);
plan.preview_unified_diff();
plan.apply(apply_mode::write_files); // 明示が必要
```

### 3.7 AI-compatible API

AI エージェントが失敗しにくい API にするため、すべての主要オブジェクトは次を持つ。

- `explain()`
- `schema()`
- `examples()`
- `to_json()`
- `to_markdown()`
- `validate()`
- `dry_run()`
- `confidence()`
- `evidence()`

---

## 4. 要求仕様

### 4.1 上位目標

`cxxred` は以下を実現する。

1. C/C++ の意味を理解した semantic grep を簡単に書ける。
2. 静的解析 check を、AST matcher 直書きよりもユースケースに近い形で書ける。
3. dataflow/taint/call graph/class hierarchy/include graph を組み合わせた調査を簡単に書ける。
4. refactoring/codemod を、編集安全性、macro policy、reparse verification、format を含めて計画できる。
5. mock/fake/test seam/fuzz harness を、安全な semantic copy と public surface extraction に基づいて生成できる。
6. sanitizer/coverage/fuzzer を QA workflow として宣言的に実行できる。
7. CI/PR/diff 上で、baseline、suppression、severity gate、SARIF 出力を統一的に扱える。
8. AI コーディングエージェントが、安定した schema と explainable API に基づいて解析ツールを生成できる。

### 4.2 ハード要件

| ID | 要件 | 詳細 |
|---|---|---|
| H-01 | 下位層非劣化 | `cxxred` の利用により `cxxlens` で可能なことが不可能になってはならない。 |
| H-02 | 下位層脱出口 | すべての workflow/recipe/selector は `cxxlens` へ降りる拡張点を持つ。 |
| H-03 | 正規表現非依存 | C/C++ 解析の標準 API は AST/型/名前解決/index/dataflow を用いる。 |
| H-04 | compile DB 第一級 | project 解析は compile_commands.json または等価な build context を前提にする。 |
| H-05 | evidence 必須 | 重要な finding は根拠・confidence・approximation を保持する。 |
| H-06 | 安全編集 | 編集は `cxxlens::edit_plan` を経由し、既定で macro edit reject、dry-run、format、reparse verification。 |
| H-07 | 多構成対応 | 同一ファイルの複数 compile command、target、標準、feature macro を区別できる。 |
| H-08 | C と C++ | C の関数/typedef/struct/macro/function pointer と C++ の class/template/overload/module を同じ世界で扱う。 |
| H-09 | 出力標準 | JSON、Markdown、SARIF、unified diff、Graphviz DOT を標準出力形式にする。 |
| H-10 | AI 互換 | schema/explain/examples/why_no_match を標準装備する。 |

### 4.3 優先度別要件

#### P0: MVP

- workspace open、fact extraction、semantic selector。
- symbol/reference/call/member-call/class-hierarchy/include semantic grep。
- rule DSL と diagnostic report。
- simple codemod plan。
- mock/fake の public surface extraction。
- JSON/SARIF/Markdown 出力。
- AI 向け explain/schema。

#### P1: 実用化

- virtual dispatch 近似、overload evidence、template instantiation 方針。
- local dataflow、taint source/sink/barrier DSL。
- include cleanup workflow。
- API migration recipe。
- PR diff/baseline/suppression/gate。
- sanitizer/coverage runner。
- fuzz harness generator。

#### P2: 高度化

- global dataflow、cross-TU summary、function pointer/callback 解析。
- CSA checker adapter。
- dynamic evidence と static finding の結合。
- ABI/public surface diff。
- architecture rule packs。
- C++ modules/BMI-aware indexing。
- knowledge-base backed API modeling。

---

## 5. ユースケースの網羅的分類

この章では C/C++ ソースコード解析で想定すべきユースケースを分類する。完全に有限な列挙ではないが、`cxxred` が公開 API として支援すべき主要空間を網羅する。

### 5.1 Semantic grep / コード調査

| UC | ユースケース | 従来 grep の問題 | 必要な意味情報 |
|---|---|---|---|
| SG-01 | 関数呼び出しを探す | overload、namespace、using、macro で誤検出/漏れ。 | callee canonical decl、overload resolution。 |
| SG-02 | メソッド呼び出しを receiver type 指定で探す | `foo.bar()` だけでは型が不明。 | receiver static type、implicit object argument。 |
| SG-03 | 基底クラス/派生クラスを含むメソッド呼び出しを探す | symbol 名だけでは多態性を扱えない。 | class hierarchy、override set、virtual dispatch candidate。 |
| SG-04 | constructor/destructor 呼び出しを探す | 暗黙呼び出しや temporary がある。 | CXXConstructExpr、CXXTemporaryObjectExpr、implicit node policy。 |
| SG-05 | operator 呼び出しを探す | `a + b` と `operator+` が混在。 | operator call expr、overload resolution。 |
| SG-06 | conversion を探す | implicit conversion は文字列に現れない。 | implicit cast、constructor conversion、user-defined conversion。 |
| SG-07 | template instantiation を探す | pattern と instantiation が分かれる。 | template decl、specialization、dependent type。 |
| SG-08 | macro expansion を探す | 展開後 AST では元 macro が見えにくい。 | preprocessor callback、spelling/expansion range。 |
| SG-09 | include 使用元を探す | transitive include による誤解。 | include graph、symbol-to-header mapping。 |
| SG-10 | global variable write を探す | alias、reference、macro 経由がある。 | reference kind、write/read classification、dataflow。 |
| SG-11 | enum value 使用を探す | scoped/unscoped、using、macro がある。 | enum constant canonical decl。 |
| SG-12 | attribute/annotation 使用を探す | compiler-specific attribute がある。 | attr AST node、spelling。 |
| SG-13 | `extern "C"` boundary を探す | linkage は単純文字列では不十分。 | linkage spec decl、mangled name。 |
| SG-14 | C function pointer call を探す | callee 名が直接出ない。 | function pointer type、assignment/callback flow。 |
| SG-15 | lambda/coroutine 使用を探す | generated closure/coroutine state が関与。 | lambda expr、coroutine AST node、implicit calls。 |

### 5.2 静的検査 / Lint / コーディング標準

| UC | ユースケース | 例 |
|---|---|---|
| ST-01 | 禁止 API 検出 | `strcpy`、`sprintf`、project-specific unsafe API。 |
| ST-02 | API 誤用検出 | init 前 use、lock/unlock 対応、open/close 対応。 |
| ST-03 | error handling 検査 | 戻り値未確認、`errno`、`std::expected` 無視、status 無視。 |
| ST-04 | exception policy 検査 | noexcept 境界、catch-all、throw 禁止領域。 |
| ST-05 | const correctness | const にできる method/variable、mutable 誤用。 |
| ST-06 | ownership/lifetime | raw pointer 所有権、dangling `string_view`、return local address。 |
| ST-07 | move/copy | 不要 copy、use-after-move、self-move、expensive pass-by-value。 |
| ST-08 | integer/bounds | overflow、narrowing、signed/unsigned、array bounds。 |
| ST-09 | nullability | null deref、nullable contract 違反。 |
| ST-10 | concurrency | data race patterns、lock order、atomic misuse、double-checked locking。 |
| ST-11 | memory management | manual delete、delete mismatch、new/delete 禁止。 |
| ST-12 | initialization | uninitialized field、static initialization order。 |
| ST-13 | ODR/header hygiene | inline 定義、anonymous namespace in header、include guard。 |
| ST-14 | template constraints | unconstrained template、SFINAE misuse、concept migration。 |
| ST-15 | style/coding standard | MISRA/CERT/project rule pack。 |
| ST-16 | real-time/safety domain | allocation/lock/logging 禁止領域。 |
| ST-17 | security | injection、path traversal、format string、crypto misuse、secret leakage。 |
| ST-18 | C-specific | macro side effect、reserved identifier、unsafe casts、strict aliasing。 |

### 5.3 Dataflow / Taint / Control-flow

| UC | ユースケース | 必要 API |
|---|---|---|
| DF-01 | local dataflow | 関数内で値がどこへ流れるか。 |
| DF-02 | global dataflow | 関数呼び出しをまたいだ値伝播。 |
| DF-03 | taint tracking | 外部入力から危険 sink への到達。 |
| DF-04 | sanitizer/barrier model | validation、escape、encoding、bounds check。 |
| DF-05 | resource state machine | open/read/close、lock/unlock、init/use/free。 |
| DF-06 | ownership transfer | `unique_ptr`、raw pointer、factory、release。 |
| DF-07 | null-state analysis | maybe-null、checked-not-null、dereferenced。 |
| DF-08 | range analysis | length、index、capacity、array bounds。 |
| DF-09 | effect analysis | allocates、blocks、throws、locks、I/O。 |
| DF-10 | callback/data escape | function pointer、std::function、async task。 |

### 5.4 Call graph / Class hierarchy / Impact analysis

| UC | ユースケース | 内容 |
|---|---|---|
| CG-01 | callers/callees | 直接 call、method call、operator call。 |
| CG-02 | virtual call target candidates | override set、final、devirtualization hints。 |
| CG-03 | API 変更影響範囲 | public API 変更時の call site、override、mock、tests。 |
| CG-04 | class hierarchy graph | base/derived、interface implementation。 |
| CG-05 | callback graph | function pointer、lambda、std::function、signal/slot。 |
| CG-06 | include dependency impact | header 変更で影響する TU。 |
| CG-07 | ownership/dependency graph | object composition、DI、factory。 |
| CG-08 | architecture layering | module/package/subsystem 境界違反。 |
| CG-09 | dead code candidates | unreferenced symbol、unreachable function。 |
| CG-10 | test impact selection | 変更 symbol から関係 test を選ぶ。 |

### 5.5 Refactoring / Codemod

| UC | ユースケース | 例 |
|---|---|---|
| RF-01 | rename | symbol-aware rename。 |
| RF-02 | API replacement | `old_api(x)` → `new_api(x, opts)`。 |
| RF-03 | method-to-function / function-to-method | `s.size()` → `Size(s)` など。 |
| RF-04 | signature migration | 引数追加、削除、順序変更、default 引数。 |
| RF-05 | ownership migration | raw pointer → `std::unique_ptr` / `std::shared_ptr` / `not_null`。 |
| RF-06 | enum migration | unscoped enum → enum class。 |
| RF-07 | error model migration | bool/status code → `expected` / `StatusOr`。 |
| RF-08 | include cleanup | 不足 include 追加、未使用 include 削除。 |
| RF-09 | namespace migration | namespace rename、using 削除。 |
| RF-10 | macro to constexpr/function | function-like macro を inline function へ。 |
| RF-11 | modernize | `NULL` → `nullptr`、range-for、`override` 追加。 |
| RF-12 | concurrency migration | lock guard 導入、mutex policy 変更。 |
| RF-13 | test seam insertion | dependency injection point、interface extraction。 |
| RF-14 | logging/metrics insertion | 指定 API の前後に instrumentation。 |
| RF-15 | generated code-safe transform | generated file を除外/入力元に反映。 |

### 5.6 Mock / Fake / Test generation

| UC | ユースケース | 内容 |
|---|---|---|
| MK-01 | public interface extraction | class の public virtual surface を抽出。 |
| MK-02 | gMock/Trompeloeil mock 生成 | method signature、qualifier、noexcept、ref-qualifier 保持。 |
| MK-03 | minimal fake 生成 | 既定値返却、stateful fake、recording fake。 |
| MK-04 | C API fake 生成 | function pointer table、link seam、weak symbol。 |
| MK-05 | dependency copy | fake に必要な enum/type/include を semantic copy。 |
| MK-06 | ODR safety | inline/template/macro 定義のコピー安全性評価。 |
| MK-07 | test seam refactor | concrete dependency → interface dependency。 |
| MK-08 | fuzz harness | 関数/API から `LLVMFuzzerTestOneInput` skeleton。 |
| MK-09 | golden test generator | 入出力例・fixture・snapshot。 |
| MK-10 | contract-based generator | precondition/postcondition から test cases。 |

### 5.7 動的解析 / 実行時 QA

| UC | ユースケース | 内容 |
|---|---|---|
| DY-01 | ASan test run | memory error 検出 build/test/report。 |
| DY-02 | TSan test run | race 検出。 |
| DY-03 | UBSan/MSan/LSan/CFI | UB、未初期化、leak、control-flow hardening。 |
| DY-04 | DFSan taint experiment | runtime tag propagation。 |
| DY-05 | coverage import | source-based coverage を source range に対応付け。 |
| DY-06 | fuzz campaign | harness build、corpus、minimization、crash dedup。 |
| DY-07 | static-dynamic join | 静的 finding に実行証拠を付ける。 |
| DY-08 | changed-code coverage gate | diff 部分の coverage gate。 |
| DY-09 | flaky/non-deterministic behavior | fuzz target determinism check。 |
| DY-10 | performance instrumentation | hot path、allocation、blocking call。 |

### 5.8 Documentation / Understanding

| UC | ユースケース | 内容 |
|---|---|---|
| DOC-01 | public API inventory | exported symbol、public class、header surface。 |
| DOC-02 | call tree document | 関数から下流 call tree を生成。 |
| DOC-03 | architecture map | namespace/module/package dependency graph。 |
| DOC-04 | ownership map | owner/borrower/lifetime relation。 |
| DOC-05 | documentation coverage | public API のコメント有無、param mismatch。 |
| DOC-06 | API diff | 変更前後の symbol/signature/ABI surface。 |
| DOC-07 | migration guide | codemod 結果と manual action の説明。 |
| DOC-08 | audit evidence bundle | 規則、検出、根拠、抑制、修正のまとめ。 |

### 5.9 CI / PR / Review automation

| UC | ユースケース | 内容 |
|---|---|---|
| CI-01 | diff-only check | 変更行に関係する診断のみ gate。 |
| CI-02 | baseline suppression | 既存違反は記録し、新規違反だけ fail。 |
| CI-03 | severity gate | error/high confidence のみ fail。 |
| CI-04 | SARIF export | code scanning platform へ出力。 |
| CI-05 | review bot comments | finding を PR コメント化。 |
| CI-06 | auto-fix proposal | edit_plan を patch として提示。 |
| CI-07 | incremental index | 変更 TU と依存 TU だけ再解析。 |
| CI-08 | metrics trend | findings、coverage、risk score の推移。 |
| CI-09 | policy pack versioning | rule pack の版管理。 |
| CI-10 | AI review handoff | 機械可検結果を AI に渡し、ファジー判断に集中。 |

### 5.10 Build / Platform / Language frontier

| UC | ユースケース | 内容 |
|---|---|---|
| BL-01 | compile DB validation | flag 欠落、resource-dir、target、stdlib 差分検出。 |
| BL-02 | multi-config analysis | debug/release、platform、feature macro 差分。 |
| BL-03 | generated code policy | 生成コード除外、生成元修正、read-only 扱い。 |
| BL-04 | C++ modules | module interface/import/BMI availability。 |
| BL-05 | precompiled header | PCH による解析差分。 |
| BL-06 | system header policy | system header match を除外/含める。 |
| BL-07 | cross-compilation | target triple、sysroot、embedded toolchain。 |
| BL-08 | mixed C/C++ | `extern "C"`、C header、C++ wrapper。 |
| BL-09 | compiler extensions | GNU/MSVC/Clang attributes、builtins。 |
| BL-10 | vendor SDK | generated headers、opaque macros、binary-only libs。 |

---

## 6. 上位レイヤ全体アーキテクチャ

### 6.1 モジュール一覧

```text
cxxred-core          基本型、workspace、recipe、workflow、evidence、confidence
cxxred-select        ユースケース指向 selector DSL
cxxred-facts         fact model、fact extraction、fact store、incremental cache
cxxred-grep          semantic grep / code search recipes
cxxred-graph         call graph、class hierarchy、include graph、dependency graph
cxxred-rules         rule/check DSL、rule pack、suppression、SARIF report
cxxred-flow          dataflow/taint/resource state/effect analysis recipes
cxxred-security      security source/sink/model packs、危険 API packs
cxxred-safety        safety/reliability/coding-standard packs
cxxred-refactor      codemod/refactoring/migration recipes
cxxred-mock          mock/fake/semantic copy/test seam generation
cxxred-dynamic       sanitizer/coverage/fuzzer workflow
cxxred-review        PR/diff/baseline/CI gate/review bot integration
cxxred-explain       explainability、AI schema、examples、debugging support
cxxred-cli           CLI/config/plugin entrypoint
```

### 6.2 依存関係

```text
core
 ├─ select
 │   ├─ facts
 │   │   ├─ grep
 │   │   ├─ graph
 │   │   ├─ rules
 │   │   ├─ flow
 │   │   ├─ refactor
 │   │   └─ mock
 │   ├─ security ──┐
 │   └─ safety   ──┤ depends on rules/flow/grep/refactor
 ├─ dynamic
 ├─ review
 ├─ explain
 └─ cli
```

全モジュールは `cxxlens-core`、`cxxlens-project`、`cxxlens-session` へ依存可能とする。ただし、上位 API 利用者に raw `clang::*` を露出するのは脱出口のみである。

### 6.3 実行モデル

```cpp
auto ws = cxxred::workspace::open(
  cxxred::workspace_config::from_build_dir("build"));

// fact を必要に応じて lazy build。
ws.ensure_facts(cxxred::fact_profile::semantic_grep());

// recipe 実行。
auto report = cxxred::grep::calls_to_method()
  .receiver_type("my::Service")
  .method_name("start")
  .run(ws);

// review workflow。
auto review = cxxred::review::for_diff("origin/main")
  .add(report)
  .gate(cxxred::gate::no_high_confidence_errors())
  .run(ws);
```

### 6.4 中核データフロー

```text
workspace_config
   ↓
workspace
   ↓ uses cxxlens::project + cxxlens::analysis_session
fact_extractor(s)
   ↓
fact_store: symbols / refs / calls / types / includes / macros / CFG summaries / flow summaries
   ↓
selector / recipe / rule / workflow
   ↓
report: findings + evidence + confidence + edits + unresolved
   ↓
JSON / SARIF / Markdown / diff / graph / patch
```

---

## 7. 中核メタモデル

### 7.1 Workspace

`workspace` は `cxxred` の実行単位である。下位層の `cxxlens::project` と `cxxlens::analysis_session` を保持し、fact store、cache、config、policy を束ねる。

```cpp
namespace cxxred {

class workspace_config {
public:
  static cxxlens::result<workspace_config> from_build_dir(cxxlens::path build_dir);
  static cxxlens::result<workspace_config> from_compile_commands(cxxlens::path file);
  static workspace_config from_cxxlens_project(cxxlens::project project);

  workspace_config& root(cxxlens::path);
  workspace_config& profile(std::string name);
  workspace_config& include_paths(std::vector<cxxlens::path>);
  workspace_config& generated_code_policy(generated_code_policy);
  workspace_config& system_header_policy(system_header_policy);
  workspace_config& language_policy(language_policy);
  workspace_config& cache_dir(cxxlens::path);
  workspace_config& parallelism(size_t);
  workspace_config& option(std::string key, std::string value);

  cxxlens::schema schema() const;
};

class workspace {
public:
  static cxxlens::result<workspace> open(workspace_config);

  cxxlens::project& project();
  cxxlens::analysis_session& session();
  fact_store& facts();
  const fact_store& facts() const;
  workspace_policy policy() const;

  cxxlens::result<void> ensure_facts(fact_profile);
  cxxlens::result<void> refresh(changed_files);
  cxxlens::result<void> save_cache() const;
  cxxlens::result<void> load_cache();

  template <class Fn>
  auto with_cxxlens(Fn&& fn)
    -> cxxlens::result<std::invoke_result_t<Fn, cxxlens::analysis_session&>>;

  std::string explain() const;
  cxxlens::schema schema() const;
};

} // namespace cxxred
```

下位実装:

| `cxxred` API | 利用する `cxxlens` API |
|---|---|
| `workspace_config::from_build_dir()` | `cxxlens::project_config::from_compilation_database()` |
| `workspace::open()` | `cxxlens::project::open()`、`cxxlens::analysis_session::create()` |
| `workspace::ensure_facts()` | `cxxlens::workspace_index::build()`、`query_engine::find()`、`include_analyzer::build_graph()` |
| `workspace::refresh()` | `analysis_session::parse_changed_files()`、index/fact cache invalidation |
| `workspace::with_cxxlens()` | `analysis_session` 脱出口 |

### 7.2 Finding / Evidence / Confidence

```cpp
namespace cxxred {

using finding_id = std::string;

struct evidence_item {
  std::string kind;                // ast_match, type_relation, call_edge, flow_path, dynamic_trace, etc.
  std::string summary;
  std::optional<cxxlens::source_range> range;
  std::map<std::string, std::string> properties;
};

class evidence {
public:
  evidence& add(evidence_item);
  evidence& add_ast_match(cxxlens::match);
  evidence& add_symbol(symbol_ref);
  evidence& add_type_relation(type_relation);
  evidence& add_call_edge(call_edge);
  evidence& add_flow_path(flow_path);
  evidence& add_dynamic_trace(dynamic_trace);
  evidence& add_unresolved(unresolved_reason);

  std::vector<evidence_item> items() const;
  std::string to_json() const;
  std::string to_markdown() const;
};

enum class confidence {
  impossible,
  speculative,
  possible,
  probable,
  high,
  certain
};

class finding {
public:
  finding_id id() const;
  std::string rule_or_recipe() const;
  cxxlens::severity severity() const;
  confidence confidence() const;
  cxxlens::source_range primary_range() const;
  std::string message() const;
  evidence evidence() const;
  std::vector<cxxlens::diagnostic> diagnostics() const;
  std::optional<cxxlens::edit_plan> fix() const;

  std::string explain() const;
  std::string to_json() const;
  std::string to_markdown() const;
};

class finding_set {
public:
  size_t size() const;
  bool empty() const;
  std::vector<finding> all() const;
  finding_set filter(confidence min_confidence) const;
  finding_set only_changed_lines(diff_view) const;
  cxxlens::edit_plan fixes() const;

  std::string to_json() const;
  std::string to_sarif() const;
  std::string to_markdown() const;
};

} // namespace cxxred
```

下位実装:

| 概念 | 利用する `cxxlens` API |
|---|---|
| AST evidence | `cxxlens::match`、`match::node<T>()`、`match::range()` |
| source evidence | `source_range`、`source_manager_view::text()`、macro origin |
| diagnostic | `cxxlens::diagnostic` |
| fix | `cxxlens::edit_plan` |
| SARIF | `cxxlens::check_report::to_sarif()` 相当の schema reuse |

### 7.3 Recipe / Workflow

Recipe は単一ユースケース、Workflow は複数 recipe と gate/出力/CI を組み合わせる単位である。

```cpp
namespace cxxred {

class recipe_context {
public:
  workspace& ws();
  fact_store& facts();
  cxxlens::analysis_session& session();
  recipe_options options() const;
  void emit(finding);

  template <class Fn>
  auto with_cxxlens(Fn&& fn)
    -> cxxlens::result<std::invoke_result_t<Fn, cxxlens::analysis_session&>>;
};

class recipe {
public:
  virtual std::string name() const = 0;
  virtual std::string summary() const = 0;
  virtual cxxlens::schema input_schema() const = 0;
  virtual cxxlens::schema output_schema() const = 0;
  virtual cxxlens::result<finding_set> run(recipe_context&) const = 0;
  virtual std::string explain() const = 0;
  virtual std::vector<std::string> examples() const = 0;
  virtual ~recipe() = default;
};

class workflow {
public:
  workflow& add(std::shared_ptr<recipe>);
  workflow& add_step(std::shared_ptr<workflow_step>);
  workflow& require_facts(fact_profile);
  workflow& gate(gate_policy);
  workflow& output(report_format, cxxlens::path);
  workflow& dry_run(bool = true);

  cxxlens::result<workflow_report> run(workspace&);
  std::string explain() const;
};

} // namespace cxxred
```

下位実装:

- recipe は `cxxlens::query_engine`、`workspace_index`、`run_dataflow`、`transformer`、`dynamic_runner` などを呼び出す。
- workflow は `analysis_session` の parse/cache、`check_runner`、`edit_plan`、SARIF output を統合する。

---

## 8. Fact model 詳細設計

### 8.1 Fact の目的

下位層の `cxxlens::translation_unit` や raw Clang AST は lifetime と thread safety の制約がある。上位層では、解析結果を immutable な fact として抽出し、cross-TU、並列、cache、AI 出力に使う。

### 8.2 Fact 種別

```cpp
namespace cxxred {

enum class fact_kind {
  symbol,
  reference,
  declaration,
  definition,
  type_relation,
  inheritance_edge,
  override_edge,
  call_edge,
  include_edge,
  macro_expansion,
  compile_command,
  cfg_summary,
  dataflow_summary,
  taint_model,
  dynamic_observation,
  coverage_region,
  diagnostic,
  custom
};

struct fact_id {
  std::string opaque;
  friend bool operator==(fact_id, fact_id) = default;
};

class fact {
public:
  fact_id id() const;
  fact_kind kind() const;
  std::string stable_key() const;
  std::optional<cxxlens::source_range> range() const;
  std::string to_json() const;
};

} // namespace cxxred
```

### 8.3 Symbol fact

```cpp
class symbol_ref {
public:
  fact_id id() const;
  std::string name() const;
  std::string qualified_name() const;
  symbol_kind kind() const; // function, method, class, enum, macro, variable, namespace, module, etc.
  cxxlens::source_range declaration_range() const;
  std::optional<cxxlens::source_range> definition_range() const;
  std::string usr() const;       // Clang USR 互換。取得不能なら stable_key。
  std::string display_name() const;
};
```

下位実装:

- `cxxlens::workspace_index::symbols()`
- `cxxlens::decl::qualified_name()`
- canonical decl / USR は Clang index/AST から抽出。
- macro symbol は `translation_unit::preprocessor()` / PP callbacks から抽出。

### 8.4 Reference fact

```cpp
class reference_ref {
public:
  fact_id id() const;
  symbol_ref target() const;
  cxxlens::source_range range() const;
  reference_kind kind() const; // read, write, call, address_taken, type_use, template_arg, override, etc.
  bool from_macro() const;
  evidence evidence() const;
};
```

下位実装:

- `cxxlens::workspace_index::references(symbol_id)`
- `cxxlens::query_engine::find()` による AST 補完。
- 書き込み/読み込み分類は AST node kind、operator、assignment、mutation method model を使用。

### 8.5 Call fact

```cpp
class call_edge {
public:
  fact_id id() const;
  symbol_ref caller() const;
  std::optional<symbol_ref> direct_callee() const;
  std::vector<symbol_ref> possible_callees() const;
  cxxlens::source_range call_site() const;
  call_kind kind() const; // function, member, constructor, destructor, operator, virtual, function_pointer, callback
  dispatch_model dispatch() const;
  confidence confidence() const;
  evidence evidence() const;
};
```

下位実装:

- direct call: `cxxlens::q::call_expr()`、callee canonical decl。
- member call: `q::member_call_expr()`、receiver type。
- virtual: class hierarchy fact + override edge。
- function pointer/callback: assignment/reference/dataflow summary を組み合わせ、confidence を落とす。
- constructor/destructor/operator: AST node kind 別 extractor。

### 8.6 Type relation / inheritance / override fact

```cpp
class type_ref {
public:
  std::string spelling() const;
  std::string canonical_spelling() const;
  std::optional<symbol_ref> declaration() const;
  bool is_pointer() const;
  bool is_reference() const;
  bool is_cv_qualified() const;
};

class type_relation {
public:
  type_ref from() const;
  type_ref to() const;
  type_relation_kind kind() const; // same, base_of, derived_from, converts_to, typedef_of, template_specialization_of
  confidence confidence() const;
  evidence evidence() const;
};
```

下位実装:

- `cxxlens::type::canonical_spelling()`
- `record_decl::bases()`
- method override は Clang AST の CXXMethodDecl override 情報。
- implicit conversion は AST implicit cast / constructor conversion。

### 8.7 Include / macro fact

```cpp
class include_edge {
public:
  cxxlens::path includer() const;
  std::string spelling() const;
  std::optional<cxxlens::path> resolved() const;
  include_kind kind() const; // quote, angle, system, generated
  bool used() const;
  std::vector<symbol_ref> symbols_using_it() const;
};

class macro_expansion_fact {
public:
  std::string macro_name() const;
  cxxlens::source_range expansion_range() const;
  cxxlens::source_range spelling_range() const;
  std::vector<std::string> arguments() const;
  bool function_like() const;
};
```

下位実装:

- `cxxlens::include_analyzer::analyze()`、`include_graph`
- `translation_unit::preprocessor()` callbacks
- `source_manager_view::spelling_location()` / `expansion_location()`

### 8.8 Fact store API

```cpp
namespace cxxred {

class fact_query {
public:
  static fact_query all();
  fact_query kind(fact_kind) const;
  fact_query file(cxxlens::path) const;
  fact_query symbol(std::string qualified_name) const;
  fact_query range_intersects(cxxlens::source_range) const;
  fact_query changed_in(diff_view) const;
  fact_query custom(std::string key, std::string value) const;
};

class fact_store {
public:
  std::vector<fact> find(fact_query) const;

  std::vector<symbol_ref> symbols(symbol_selector) const;
  std::vector<reference_ref> references(reference_selector) const;
  std::vector<call_edge> calls(call_selector) const;
  std::vector<type_relation> type_relations(type_selector, type_selector) const;
  std::vector<include_edge> includes(include_selector) const;
  std::vector<macro_expansion_fact> macros(macro_selector) const;

  cxxlens::result<void> add_custom_fact(fact);
  cxxlens::result<void> add_extractor(std::unique_ptr<fact_extractor>);
  cxxlens::result<void> rebuild(workspace&, fact_profile);

  std::string to_json() const;
};

class fact_extractor {
public:
  virtual std::string name() const = 0;
  virtual fact_profile provides() const = 0;
  virtual cxxlens::result<std::vector<fact>> extract(workspace&) = 0;
  virtual ~fact_extractor() = default;
};

} // namespace cxxred
```

### 8.9 Fact profile

```cpp
enum class fact_profile_kind {
  minimal,
  semantic_grep,
  refactor,
  rule_checking,
  dataflow,
  security,
  dynamic,
  full
};

class fact_profile {
public:
  static fact_profile minimal();
  static fact_profile semantic_grep();
  static fact_profile refactor();
  static fact_profile security();
  static fact_profile full();

  fact_profile& include(fact_kind);
  fact_profile& exclude(fact_kind);
  fact_profile& precision(precision_level);
};
```

---

## 9. Selector DSL 詳細設計

Selector は `cxxred` の最重要 API である。Clang AST matcher 直書きよりも、ユースケースに近い言葉で対象を選ぶ。

### 9.1 基本方針

- selector は immutable builder。
- 文字列は正規表現ではなく、既定では exact qualified name、glob、または `name_pattern` とする。
- すべての selector は `to_cxxlens_query()`、`explain()`、`schema()` を持つ。
- selector は fact store に対する query と、TU に対する AST query の両方へコンパイルできる。
- 曖昧な C++ 構文には `confidence` と `evidence` を返す。

### 9.2 Symbol selector

```cpp
namespace cxxred::select {

class symbol_selector {
public:
  symbol_selector name(std::string exact) const;
  symbol_selector qualified_name(std::string exact) const;
  symbol_selector name_glob(std::string glob) const;
  symbol_selector in_namespace(std::string ns) const;
  symbol_selector kind(symbol_kind) const;
  symbol_selector declared_in(cxxlens::path file) const;
  symbol_selector defined_in(cxxlens::path file) const;
  symbol_selector exported(bool = true) const;
  symbol_selector from_system_header(bool = false) const;
  symbol_selector from_generated_code(bool = false) const;
  symbol_selector annotated_with(std::string attr_or_macro) const;
  symbol_selector satisfying(std::function<bool(const symbol_ref&)> predicate) const;

  cxxlens::query to_cxxlens_query() const;
  std::string explain() const;
};

symbol_selector any_symbol();
symbol_selector function(std::string qualified_name = {});
symbol_selector method(std::string qualified_name = {});
symbol_selector record(std::string qualified_name = {});
symbol_selector enum_type(std::string qualified_name = {});
symbol_selector variable(std::string qualified_name = {});
symbol_selector macro(std::string name = {});

} // namespace cxxred::select
```

下位実装:

| Selector | `cxxlens` 展開 |
|---|---|
| `function("ns::f")` | `cxxlens::q::function().qualified_name("ns::f")` |
| `method("C::m")` | `q::method().qualified_name(...)` |
| `record("C")` | `q::record_decl().qualified_name(...)` |
| `macro("M")` | `q::macro_expansion()` + preprocessor fact |
| `exported(true)` | linkage/visibility fact + public header heuristics |

### 9.3 Type selector

```cpp
class type_selector {
public:
  type_selector spelling(std::string exact) const;
  type_selector canonical(std::string exact) const;
  type_selector declared_as(symbol_selector) const;
  type_selector pointer_to(type_selector) const;
  type_selector reference_to(type_selector) const;
  type_selector const_qualified(bool = true) const;
  type_selector derived_from(std::string base) const;
  type_selector base_of(std::string derived) const;
  type_selector convertible_to(type_selector) const;
  type_selector template_specialization_of(std::string template_name) const;
  type_selector argument(size_t index, type_selector arg) const;
  type_selector any_cvref() const;

  cxxlens::query to_cxxlens_query() const;
};

type_selector type(std::string canonical = {});
type_selector record_type(std::string canonical = {});
type_selector pointer_to(type_selector);
type_selector reference_to(type_selector);
type_selector template_specialization(std::string template_name);
```

下位実装:

- `cxxlens::type` canonical/spelling。
- `record_decl::bases()`。
- template specialization AST type。
- conversion relation fact。

### 9.4 Call selector

```cpp
class call_selector {
public:
  call_selector callee(symbol_selector) const;
  call_selector callee_qualified_name(std::string) const;
  call_selector function_name(std::string) const;
  call_selector method_name(std::string) const;
  call_selector receiver_type(type_selector) const;
  call_selector receiver_derived_from(std::string base) const;
  call_selector include_derived_types(bool = true) const;
  call_selector include_virtual_overrides(bool = true) const;
  call_selector include_operator_calls(bool = true) const;
  call_selector include_constructors(bool = true) const;
  call_selector include_destructors(bool = true) const;
  call_selector include_function_pointers(bool = false) const;
  call_selector argument(size_t index, expr_selector) const;
  call_selector argument_type(size_t index, type_selector) const;
  call_selector inside(symbol_selector function_or_method) const;
  call_selector from_macro(bool) const;
  call_selector dispatch(dispatch_policy) const;
  call_selector precision(precision_level) const;

  cxxlens::query to_cxxlens_query() const;
};

call_selector call();
call_selector calls_to(symbol_selector callee);
call_selector calls_to_function(std::string qualified_name);
call_selector calls_to_method(std::string receiver_type, std::string method_name);
```

#### 9.4.1 「あるクラスに特化したメソッド呼び出し」の設計

利用例:

```cpp
auto report = cxxred::grep::calls_to_method()
  .receiver_type("app::Service")
  .method_name("start")
  .include_derived_types(true)
  .include_virtual_overrides(true)
  .dispatch(cxxred::dispatch_policy::static_and_virtual_candidates())
  .run(ws);
```

意味:

- `app::Service` 型の object/reference/pointer に対する `start` 呼び出しを探す。
- `Service` の派生型に対する呼び出しも含める。
- `start` が virtual の場合は override set も候補に含める。
- 実行時動的型が完全に不明な場合は `probable` または `possible` に落とす。

下位実装手順:

1. `fact_store` から `app::Service` の `symbol_ref` と `type_ref` を取得。
2. `record_decl::bases()` と `inheritance_edge` fact から派生型集合を構築。
3. `q::member_call_expr()` で全 member call を抽出。
4. 各 match で `callee` の canonical decl と method name を確認。
5. receiver expression の `expression_type()` を canonical 化。
6. receiver type が `Service` または派生型なら match。
7. virtual method の場合、override_edge fact から possible callees を付与。
8. dependent type、function pointer、type erasure、callback は unresolved/approximation evidence を追加。
9. 結果を `call_edge` + `finding` として返す。

### 9.5 Expr / Stmt selector

```cpp
class expr_selector {
public:
  expr_selector type(type_selector) const;
  expr_selector is_null_pointer() const;
  expr_selector is_literal() const;
  expr_selector integer_value(int64_t) const;
  expr_selector string_value(std::string) const;
  expr_selector refers_to(symbol_selector) const;
  expr_selector has_descendant(expr_selector) const;
  expr_selector implicit(bool) const;
  expr_selector from_macro(bool) const;
  expr_selector satisfying(std::function<bool(const expr_ref&)> predicate) const;

  cxxlens::query to_cxxlens_query() const;
};

class stmt_selector {
public:
  stmt_selector kind(stmt_kind) const;
  stmt_selector contains(expr_selector) const;
  stmt_selector inside(symbol_selector function) const;
  stmt_selector control_flow(control_flow_shape) const;
  stmt_selector from_macro(bool) const;
};
```

### 9.6 Include / Macro selector

```cpp
class include_selector {
public:
  include_selector header(std::string spelling) const;
  include_selector resolved_to(cxxlens::path) const;
  include_selector used(bool) const;
  include_selector system(bool) const;
  include_selector transitive_of(cxxlens::path) const;
  include_selector providing(symbol_selector) const;
};

class macro_selector {
public:
  macro_selector name(std::string) const;
  macro_selector function_like(bool) const;
  macro_selector expands_to_symbol(symbol_selector) const;
  macro_selector used_in(cxxlens::path) const;
  macro_selector argument_contains(expr_selector) const;
};
```

---

## 10. `cxxred-grep`: Semantic grep API

### 10.1 目的

`cxxred-grep` は、正規表現ではなく C/C++ の構文・意味に基づいてコードを検索する。

### 10.2 公開 API

```cpp
namespace cxxred::grep {

class grep_report {
public:
  finding_set findings() const;
  std::vector<symbol_ref> symbols() const;
  std::vector<reference_ref> references() const;
  std::vector<call_edge> calls() const;
  std::string to_json() const;
  std::string to_markdown() const;
  std::string explain() const;
};

class symbol_search {
public:
  symbol_search selector(select::symbol_selector) const;
  symbol_search name(std::string) const;
  symbol_search qualified_name(std::string) const;
  symbol_search kind(symbol_kind) const;
  symbol_search in_files(file_selector) const;
  symbol_search include_declarations(bool = true) const;
  symbol_search include_definitions(bool = true) const;
  cxxlens::result<grep_report> run(workspace&) const;
};

class reference_search {
public:
  reference_search target(select::symbol_selector) const;
  reference_search reads(bool = true) const;
  reference_search writes(bool = true) const;
  reference_search calls(bool = true) const;
  reference_search address_taken(bool = true) const;
  reference_search in_files(file_selector) const;
  reference_search from_macro(macro_match_policy) const;
  cxxlens::result<grep_report> run(workspace&) const;
};

class call_search {
public:
  call_search selector(select::call_selector) const;
  call_search callee(std::string qualified_name) const;
  call_search receiver_type(std::string canonical_type) const;
  call_search method_name(std::string name) const;
  call_search include_derived_types(bool) const;
  call_search include_virtual_overrides(bool) const;
  call_search include_function_pointers(bool) const;
  call_search argument(size_t, select::expr_selector) const;
  call_search precision(precision_level) const;
  call_search confidence_at_least(confidence) const;
  cxxlens::result<grep_report> run(workspace&) const;
};

symbol_search symbols();
reference_search references_to(select::symbol_selector);
call_search calls();
call_search calls_to_function(std::string qualified_name);
call_search calls_to_method();

class macro_search { /* name/args/expansion site/source site */ };
class include_search { /* used/unused/missing/providing */ };
class conversion_search { /* implicit/user-defined/narrowing */ };
class construction_search { /* constructor/temporary/aggregate */ };
class override_search { /* overrides/overridden_by */ };

} // namespace cxxred::grep
```

### 10.3 下位 API 対応表

| 上位 API | 主な下位 API | 補足 |
|---|---|---|
| `symbols()` | `workspace_index::symbols()`、`q::function()` 等 | index が古い場合は TU parse で補完。 |
| `references_to()` | `workspace_index::references()`、`query_engine::find()` | write/read 分類は AST ノードで追加判定。 |
| `calls_to_function()` | `q::call_expr()`、callee canonical decl | overload 対応。 |
| `calls_to_method()` | `q::member_call_expr()`、`type`、`record_decl::bases()` | receiver type、派生型、override 対応。 |
| `macro_search` | `q::macro_expansion()`、preprocessor callbacks | spelling/expansion range を evidence 化。 |
| `include_search` | `include_analyzer`、`include_graph` | Include Cleaner 相当の fact。 |
| `conversion_search` | implicit cast AST、constructor conversion | `traversal_policy::include_implicit` が必要。 |
| `override_search` | method override relation fact | class hierarchy fact。 |

---

## 11. `cxxred-graph`: グラフ解析 API

### 11.1 グラフ種別

```cpp
enum class graph_kind {
  call_graph,
  class_hierarchy,
  override_graph,
  include_graph,
  dependency_graph,
  ownership_graph,
  dataflow_graph,
  taint_graph,
  test_impact_graph,
  custom
};
```

### 11.2 公開 API

```cpp
namespace cxxred::graph {

class graph_node {
public:
  std::string id() const;
  graph_node_kind kind() const;
  std::string label() const;
  std::optional<cxxlens::source_range> range() const;
  std::map<std::string, std::string> properties() const;
};

class graph_edge {
public:
  std::string from() const;
  std::string to() const;
  graph_edge_kind kind() const;
  confidence confidence() const;
  evidence evidence() const;
};

class semantic_graph {
public:
  graph_kind kind() const;
  std::vector<graph_node> nodes() const;
  std::vector<graph_edge> edges() const;
  semantic_graph subgraph(select::symbol_selector roots, graph_traversal) const;
  semantic_graph filter(std::function<bool(const graph_node&, const graph_edge*)>) const;
  std::string to_dot() const;
  std::string to_json() const;
  std::string explain() const;
};

class graph_builder {
public:
  static cxxlens::result<semantic_graph> call_graph(workspace&, graph_options = {});
  static cxxlens::result<semantic_graph> class_hierarchy(workspace&, graph_options = {});
  static cxxlens::result<semantic_graph> include_graph(workspace&, graph_options = {});
  static cxxlens::result<semantic_graph> dependency_graph(workspace&, dependency_options = {});
  static cxxlens::result<semantic_graph> test_impact_graph(workspace&, diff_view);
};

class impact_query {
public:
  static impact_query from_changed_symbols(std::vector<symbol_ref>);
  impact_query& include_callers(bool = true);
  impact_query& include_callees(bool = true);
  impact_query& include_overrides(bool = true);
  impact_query& include_including_files(bool = true);
  impact_query& include_tests(bool = true);
  impact_query& max_depth(size_t);
  cxxlens::result<impact_report> run(workspace&) const;
};

} // namespace cxxred::graph
```

### 11.3 下位実装

| グラフ | `cxxlens` API | 生成する fact |
|---|---|---|
| call graph | `workspace_index::callers/callees`、`q::call_expr()` | `call_edge` |
| class hierarchy | `record_decl::bases()` | `inheritance_edge` |
| override graph | CXXMethodDecl override info via `raw()` | `override_edge` |
| include graph | `include_analyzer::build_graph()` | `include_edge` |
| dependency graph | include + symbol ref + namespace/module policy | `dependency_edge` |
| test impact | call graph + include graph + diff | `impact_edge` |

---

## 12. `cxxred-rules`: Rule / Check DSL

### 12.1 目的

`cxxred-rules` は、clang-tidy check 直書きよりも少ない boilerplate で、project-specific rule、security rule、style rule、architecture rule を作る。

### 12.2 Rule DSL

```cpp
namespace cxxred::rules {

class rule_builder {
public:
  rule_builder id(std::string);
  rule_builder title(std::string);
  rule_builder summary(std::string);
  rule_builder category(std::string);
  rule_builder severity(cxxlens::severity);
  rule_builder confidence(confidence);
  rule_builder precision(precision_level);
  rule_builder tags(std::vector<std::string>);

  rule_builder when(select::call_selector);
  rule_builder when(select::symbol_selector);
  rule_builder when(select::expr_selector);
  rule_builder when(select::stmt_selector);
  rule_builder when_fact(fact_query);
  rule_builder when_flow(flow::flow_query);

  rule_builder unless(select::call_selector);
  rule_builder unless_guarded_by(flow::barrier_model);
  rule_builder unless_suppressed_by(std::string comment_token);

  rule_builder diagnose(std::string message_template);
  rule_builder note(std::string message_template);
  rule_builder fix(refactor::codemod_recipe);
  rule_builder fix(cxxlens::rewrite_rule);
  rule_builder explain(std::string markdown);

  cxxlens::result<rule> build() const;
};

class rule {
public:
  std::string id() const;
  cxxlens::result<finding_set> run(workspace&) const;
  cxxlens::result<cxxlens::check> to_cxxlens_check() const;
  std::string to_yaml() const;
  std::string explain() const;
};

class rule_pack {
public:
  rule_pack& add(rule);
  rule_pack& add_pack(rule_pack);
  rule_pack& enable(std::string glob);
  rule_pack& disable(std::string glob);
  rule_pack& option(std::string key, std::string value);
  cxxlens::result<rule_report> run(workspace&, rule_run_options = {}) const;
};

rule_builder make_rule(std::string id);

} // namespace cxxred::rules
```

### 12.3 例: 禁止 API rule

```cpp
auto r = rules::make_rule("project.unsafe.strcpy")
  .title("strcpy must not be used")
  .severity(cxxlens::severity::error)
  .when(select::calls_to_function("::strcpy"))
  .diagnose("Use safe_string_copy() instead of strcpy().")
  .fix(refactor::replace_function_call("::strcpy", "project::safe_string_copy"))
  .explain("strcpy does not know destination capacity.")
  .build();
```

下位実装:

1. `select::calls_to_function` → `cxxlens::q::call_expr()`。
2. callee canonical decl を確認。
3. `diagnostic` 生成。
4. fix は `cxxlens::rewrite_rule` / `edit_plan`。
5. `rule::to_cxxlens_check()` で clang-tidy adapter 生成可能。

### 12.4 Suppression

```cpp
class suppression_policy {
public:
  suppression_policy& clang_tidy_nolint(bool = true);
  suppression_policy& cxxred_comment(std::string token = "CXXRED-NOLINT");
  suppression_policy& require_reason(bool = true);
  suppression_policy& expire_after(std::chrono::sys_days);
  suppression_policy& allowed_rules(std::vector<std::string>);
};
```

抑制も fact として保存し、audit report に出力する。

---

## 13. `cxxred-flow`: Dataflow / Taint / Resource state

### 13.1 解析レベル

| レベル | API | 下位実装 |
|---|---|---|
| local dataflow | `local_flow_query` | `cxxlens::run_dataflow()` |
| global summary | `global_flow_query` | fact summary + call graph |
| taint | `taint_policy` | dataflow + source/sink/barrier model |
| resource state | `resource_protocol` | custom lattice |
| path-sensitive | `path_sensitive_query` | Clang Static Analyzer adapter |
| dynamic dataflow | `dynamic_taint_run` | DFSan workflow |

### 13.2 公開 API

```cpp
namespace cxxred::flow {

class flow_node {
public:
  std::optional<cxxlens::expr> as_expr() const;
  std::optional<symbol_ref> as_symbol() const;
  std::string label() const;
  cxxlens::source_range range() const;
};

class flow_path {
public:
  flow_node source() const;
  flow_node sink() const;
  std::vector<flow_node> steps() const;
  confidence confidence() const;
  evidence evidence() const;
  std::string to_dot() const;
  std::string to_markdown() const;
};

class source_model {
public:
  static source_model call_return(select::call_selector);
  static source_model parameter(select::symbol_selector function, size_t index);
  static source_model global(select::symbol_selector variable);
  static source_model dynamic_input(std::string name);
};

class sink_model {
public:
  static sink_model call_argument(select::call_selector, size_t arg_index);
  static sink_model dereference();
  static sink_model array_index();
  static sink_model format_string();
  static sink_model command_execution();
  static sink_model file_path_open();
};

class barrier_model {
public:
  static barrier_model validation_call(select::call_selector);
  static barrier_model bounds_check();
  static barrier_model null_check();
  static barrier_model sanitizer_function(select::call_selector);
  static barrier_model custom(std::function<bool(const flow_path&)>);
};

class taint_policy {
public:
  taint_policy& source(source_model);
  taint_policy& sink(sink_model);
  taint_policy& barrier(barrier_model);
  taint_policy& additional_step(flow_step_model);
  taint_policy& max_depth(size_t);
  taint_policy& interprocedural(bool = true);
  taint_policy& precision(precision_level);
};

class taint_report {
public:
  std::vector<flow_path> paths() const;
  finding_set findings() const;
  std::string to_json() const;
  std::string to_sarif() const;
  std::string to_markdown() const;
};

cxxlens::result<taint_report> run_taint(workspace&, taint_policy);

class resource_protocol {
public:
  resource_protocol& resource(std::string name);
  resource_protocol& acquire(select::call_selector);
  resource_protocol& release(select::call_selector);
  resource_protocol& use(select::call_selector);
  resource_protocol& invalid_use_message(std::string);
  resource_protocol& leak_message(std::string);
};

cxxlens::result<finding_set> check_resource_protocol(workspace&, resource_protocol);

} // namespace cxxred::flow
```

### 13.3 下位実装

| 上位 API | `cxxlens` API | 実装方針 |
|---|---|---|
| `run_taint()` | `run_dataflow()`、`cfg_view`、`workspace_index` | local は関数内 CFG、global は summary fact + call graph。 |
| `source_model::call_return()` | `q::call_expr()` | call expression result node を source にする。 |
| `sink_model::call_argument()` | `q::call_expr()` + argument selector | 引数式を sink にする。 |
| `barrier_model::bounds_check()` | CFG/control-flow + expr analysis | dominating condition を近似。 |
| `resource_protocol` | custom lattice | state: unknown/acquired/released/error。 |
| path-sensitive | CSA adapter | P2。CSA report を `finding` へ正規化。 |

### 13.4 例: 外部入力から command execution への taint

```cpp
auto policy = flow::taint_policy{}
  .source(flow::source_model::call_return(select::calls_to_function("app::read_request_param")))
  .sink(flow::sink_model::call_argument(select::calls_to_function("::system"), 0))
  .barrier(flow::barrier_model::validation_call(select::calls_to_function("app::is_safe_command")))
  .interprocedural(true)
  .precision(precision_level::workspace_dataflow);

auto report = flow::run_taint(ws, policy);
```

---

## 14. `cxxred-security` と `cxxred-safety`

### 14.1 Model pack

Security/safety は、個別 rule だけでなく API model の集合として扱う。

```cpp
namespace cxxred::models {

class api_model_pack {
public:
  api_model_pack& function(std::string qualified_name, api_effects);
  api_model_pack& method(std::string receiver_type, std::string method_name, api_effects);
  api_model_pack& source(std::string name, flow::source_model);
  api_model_pack& sink(std::string name, flow::sink_model);
  api_model_pack& barrier(std::string name, flow::barrier_model);
  api_model_pack& dangerous(std::string name, danger_model);
  api_model_pack& replacement(std::string old_api, std::string new_api);
  cxxlens::result<void> load_yaml(cxxlens::path);
  std::string to_yaml() const;
};

} // namespace cxxred::models
```

### 14.2 Security API

```cpp
namespace cxxred::security {

rule_pack memory_safety_pack();
rule_pack command_injection_pack();
rule_pack path_traversal_pack();
rule_pack format_string_pack();
rule_pack crypto_misuse_pack();
rule_pack unsafe_c_api_pack();
rule_pack project_security_pack(models::api_model_pack);

flow::taint_policy taint_from_untrusted_input_to_dangerous_sinks(models::api_model_pack);

} // namespace cxxred::security
```

### 14.3 Safety / Reliability API

```cpp
namespace cxxred::safety {

rule_pack lifetime_pack();
rule_pack ownership_pack();
rule_pack concurrency_pack();
rule_pack exception_policy_pack(exception_policy);
rule_pack realtime_policy_pack(realtime_policy);
rule_pack include_hygiene_pack();
rule_pack modern_cpp_pack();
rule_pack cert_cpp_profile(cert_profile_options);
rule_pack misra_like_profile(misra_profile_options);
rule_pack project_architecture_pack(architecture_policy);

} // namespace cxxred::safety
```

### 14.4 下位実装

- 危険 API rule は `rules::rule_builder` + `grep::calls_to_function`。
- taint rule は `flow::taint_policy`。
- lifetime/ownership は AST + dataflow + model pack。
- concurrency は AST + resource protocol + optional TSan dynamic evidence。
- coding standard は rule pack と suppression policy。

---

## 15. `cxxred-refactor`: Codemod / Migration API

### 15.1 基本概念

`cxxred-refactor` は、単なる文字列置換ではなく、semantic selector と edit plan に基づく安全な変更を提供する。

```cpp
namespace cxxred::refactor {

class codemod_recipe : public recipe {
public:
  cxxlens::result<cxxlens::edit_plan> plan(workspace&) const;
  cxxlens::result<finding_set> run(recipe_context&) const override;
  codemod_recipe& macro_policy(cxxlens::macro_policy);
  codemod_recipe& format(bool = true);
  codemod_recipe& verify_reparse(bool = true);
  codemod_recipe& include_cleanup(bool = true);
  codemod_recipe& confidence_at_least(confidence);
};

class replacement_template {
public:
  replacement_template text(std::string stencil);
  replacement_template argument(size_t index, std::string placeholder);
  replacement_template preserve_comments(bool);
};

codemod_recipe replace_function_call(std::string old_qualified_name,
                                     std::string new_qualified_name);

codemod_recipe replace_method_call(std::string receiver_type,
                                   std::string old_method,
                                   std::string new_method);

codemod_recipe rewrite_calls(select::call_selector selector,
                             replacement_template replacement);

codemod_recipe add_include_where_needed(std::string header,
                                        select::symbol_selector where_symbol_used);

codemod_recipe remove_unused_includes();

codemod_recipe rename_symbol(select::symbol_selector symbol,
                             std::string new_name);

codemod_recipe add_override_to_overriding_methods();

codemod_recipe introduce_null_check(select::expr_selector maybe_null_expr);

codemod_recipe migrate_raw_pointer_to_unique_ptr(ownership_migration_options);

codemod_recipe wrap_calls_with_guard(select::call_selector,
                                     guard_template);

} // namespace cxxred::refactor
```

### 15.2 Codemod pipeline

```text
selector
  ↓
match/fact collection
  ↓
safety precheck
  - macro policy
  - generated code policy
  - ambiguous overload
  - formatting range
  - include dependencies
  ↓
edit generation
  ↓
conflict detection
  ↓
format changed ranges
  ↓
reparse verification
  ↓
edit_plan + evidence
```

### 15.3 下位 API 対応

| 上位 API | 下位 API |
|---|---|
| `replace_function_call` | `cxxlens::rewrite_rule`、`transformer`、`edit::replace` |
| `rename_symbol` | `workspace_index::references()`、`edit_plan`、Refactoring Engine adapter |
| `add_include_where_needed` | `include_analyzer`、`edit::add_include` |
| `remove_unused_includes` | `include_report::fixes()` |
| `migrate_raw_pointer_to_unique_ptr` | AST/query + dataflow + rewrite plan |
| `wrap_calls_with_guard` | `q::call_expr()` + source range rewrite |

### 15.4 例: API migration

```cpp
auto migration = refactor::rewrite_calls(
    select::calls_to_function("legacy::OpenFile"),
    refactor::replacement_template{}
      .text("newfs::open({arg0}, newfs::OpenOptions{})")
      .argument(0, "arg0"))
  .macro_policy(cxxlens::macro_policy::reject_macro_expansions)
  .format(true)
  .verify_reparse(true);

auto plan = migration.plan(ws);
```

---

## 16. `cxxred-mock`: Mock/Fake/Semantic copy

### 16.1 目的

C++ で mock/fake を安全に作るには、表面的な class text copy では足りない。型、namespace、include、template、default argument、cv/ref qualifier、noexcept、override、ODR、macro などを扱う必要がある。

### 16.2 公開 API

```cpp
namespace cxxred::mock {

class interface_model {
public:
  symbol_ref source_type() const;
  std::vector<method_model> methods() const;
  std::vector<type_ref> required_types() const;
  std::vector<include_edge> required_includes() const;
  std::vector<unresolved_reason> unresolved() const;
  std::string to_cpp_declaration() const;
  std::string explain() const;
};

class fake_behavior {
public:
  static fake_behavior default_values();
  static fake_behavior throw_unimplemented();
  static fake_behavior record_calls();
  static fake_behavior stateful(std::string state_model_name);
};

class generation_options {
public:
  generation_options& framework(mock_framework);
  generation_options& output_header(cxxlens::path);
  generation_options& output_source(cxxlens::path);
  generation_options& namespace_strategy(namespace_strategy);
  generation_options& include_strategy(include_strategy);
  generation_options& method_filter(method_filter);
  generation_options& fake_behavior(fake_behavior);
  generation_options& copy_policy(semantic_copy_policy);
  generation_options& odr_policy(odr_policy);
  generation_options& format(bool = true);
  generation_options& verify_reparse(bool = true);
};

class mock_plan {
public:
  interface_model interface() const;
  cxxlens::edit_plan edits() const;
  finding_set warnings() const;
  std::vector<symbol_ref> copied_symbols() const;
  std::string preview() const;
  std::string explain() const;
};

class generator {
public:
  static generator for_class(std::string qualified_name);
  static generator for_symbol(select::symbol_selector);
  static generator for_c_api_header(cxxlens::path header);

  generator& options(generation_options);
  generator& include_method(select::symbol_selector method);
  generator& exclude_method(select::symbol_selector method);
  generator& fake_behavior(fake_behavior);
  cxxlens::result<mock_plan> plan(workspace&) const;
};

class semantic_copier {
public:
  static cxxlens::result<copy_plan> copy_public_surface(workspace&,
                                                        select::symbol_selector,
                                                        semantic_copy_policy);
  static cxxlens::result<copy_plan> copy_required_types(workspace&,
                                                        interface_model,
                                                        semantic_copy_policy);
};

} // namespace cxxred::mock
```

### 16.3 下位実装

| 上位 API | 下位 API |
|---|---|
| `generator::for_class()` | `grep::symbols()`、`record_decl`、`semantic_copier::extract_public_surface()` |
| method signature extraction | `cxxlens::function_decl`、`type`、raw CXXMethodDecl for qualifiers |
| required include | `include_analyzer`、symbol-to-header fact |
| semantic copy | `cxxlens::mock::semantic_copier` |
| edit generation | `cxxlens::edit_plan` |
| verification | `analysis_session::parse()` reparse |

### 16.4 Mock 生成の安全 gate

| Gate | 内容 | 失敗時 |
|---|---|---|
| G1 | 対象 class が一意に解決できる | unresolved finding |
| G2 | method signature が完全に復元できる | confidence drop / manual action |
| G3 | 依存型の include が分かる | forward declare or unresolved |
| G4 | macro 由来 signature でない | reject or macro policy required |
| G5 | ODR-sensitive inline/template を安全に扱える | warning or reject |
| G6 | 生成後 reparse 成功 | plan invalid |

---

## 17. `cxxred-dynamic`: Sanitizer/Coverage/Fuzzer workflow

### 17.1 公開 API

```cpp
namespace cxxred::dynamic {

class qa_profile {
public:
  static qa_profile memory();        // ASan + LSan + UBSan subset
  static qa_profile concurrency();   // TSan
  static qa_profile undefined_behavior();
  static qa_profile fuzzing();
  static qa_profile coverage();
  static qa_profile hardened();      // CFI / SafeStack 等、環境依存

  qa_profile& sanitizer(cxxlens::sanitizer_kind);
  qa_profile& coverage(cxxlens::coverage_mode);
  qa_profile& build_target(std::string);
  qa_profile& test_command(std::vector<std::string>);
  qa_profile& env(std::string key, std::string value);
  qa_profile& timeout(std::chrono::seconds);
};

class dynamic_workflow {
public:
  static dynamic_workflow for_project(workspace&);
  dynamic_workflow& profile(qa_profile);
  dynamic_workflow& include_static_context(bool = true);
  dynamic_workflow& import_coverage(bool = true);
  dynamic_workflow& associate_with_findings(finding_set);
  cxxlens::result<dynamic_report> run();
};

class fuzz_recipe {
public:
  static fuzz_recipe for_function(select::symbol_selector);
  fuzz_recipe& input_model(fuzz_input_model);
  fuzz_recipe& corpus(cxxlens::path);
  fuzz_recipe& sanitizer_profile(qa_profile);
  fuzz_recipe& max_len(size_t);
  fuzz_recipe& max_total_time(std::chrono::seconds);
  cxxlens::result<fuzz_plan> plan(workspace&) const;
};

} // namespace cxxred::dynamic
```

### 17.2 下位実装

| 上位 API | 下位 API |
|---|---|
| `qa_profile::memory()` | `cxxlens::sanitizer_profile::address()` + UBSan/LSan flags |
| `dynamic_workflow::run()` | `cxxlens::dynamic_runner` |
| coverage import | `dynamic_run_result` + source-based coverage mapping parser |
| fuzz harness | `cxxlens::fuzz_harness_generator` |
| static association | finding source ranges + coverage regions + sanitizer stack traces |

---

## 18. `cxxred-review`: CI/PR/差分レビュー

### 18.1 公開 API

```cpp
namespace cxxred::review {

class diff_view {
public:
  static cxxlens::result<diff_view> from_git(std::string base_ref);
  static cxxlens::result<diff_view> from_unified_diff(std::string);
  bool contains(cxxlens::source_range) const;
  std::vector<cxxlens::path> changed_files() const;
};

class baseline {
public:
  static cxxlens::result<baseline> load(cxxlens::path);
  cxxlens::result<void> save(cxxlens::path) const;
  bool contains_equivalent(finding) const;
};

class gate_policy {
public:
  static gate_policy no_new_errors();
  static gate_policy no_high_confidence_security_findings();
  static gate_policy coverage_for_changed_lines(double min_percent);
  gate_policy& severity_at_least(cxxlens::severity);
  gate_policy& confidence_at_least(confidence);
  gate_policy& changed_lines_only(bool = true);
};

class review_workflow {
public:
  static review_workflow for_diff(diff_view);
  review_workflow& add(rule_pack);
  review_workflow& add(std::shared_ptr<recipe>);
  review_workflow& add_findings(finding_set);
  review_workflow& baseline(baseline);
  review_workflow& gate(gate_policy);
  review_workflow& output_sarif(cxxlens::path);
  review_workflow& output_markdown(cxxlens::path);
  review_workflow& propose_fixes(bool = true);
  cxxlens::result<review_report> run(workspace&);
};

} // namespace cxxred::review
```

### 18.2 下位実装

- diff から changed files → `analysis_session::parse_changed_files()`。
- rule pack → `cxxlens::check_runner` または recipe。
- fix → `edit_plan::preview_unified_diff()`。
- SARIF → `finding_set::to_sarif()`。

---

## 19. `cxxred-explain`: AI/人間向け説明 API

### 19.1 目的

AI が libTooling/ASTMatchers で失敗しやすいのは、探索対象、型、マクロ、暗黙ノード、compile flags、テンプレート instantiation のどこで詰まったかを見失うからである。`cxxred-explain` はそれを API として可視化する。

### 19.2 公開 API

```cpp
namespace cxxred::explain {

class explanation {
public:
  std::string summary() const;
  std::string markdown() const;
  std::string json() const;
  std::vector<std::string> suggested_next_actions() const;
};

explanation why_matched(const finding&);
explanation why_not_matched(workspace&, select::call_selector, cxxlens::path file);
explanation explain_selector(select::call_selector);
explanation explain_recipe(const recipe&);
explanation explain_fact_coverage(workspace&, fact_profile);
explanation explain_precision_loss(const finding&);
explanation explain_edit_safety(const cxxlens::edit_plan&);

class ai_task_card {
public:
  static ai_task_card for_recipe(const recipe&);
  static ai_task_card for_rule(const rules::rule&);
  std::string prompt_context() const;
  std::string input_schema_json() const;
  std::string output_schema_json() const;
  std::vector<std::string> examples() const;
};

} // namespace cxxred::explain
```

---

## 20. 代表ユースケースの詳細設計

### 20.1 ユースケース A: receiver type を指定したメソッド呼び出し検索

#### 要求

「`app::Service` またはその派生型に対する `start()` 呼び出しを調査したい。virtual override も候補として知りたい。」

#### 上位 API

```cpp
auto report = grep::calls_to_method()
  .receiver_type("app::Service")
  .method_name("start")
  .include_derived_types(true)
  .include_virtual_overrides(true)
  .precision(precision_level::workspace_semantic)
  .run(ws);
```

#### 下位処理

1. `workspace.ensure_facts(fact_profile::semantic_grep())`。
2. `fact_store.symbols(record("app::Service"))` で対象型を取得。
3. class hierarchy fact で派生型集合を取得。
4. 各 TU で `cxxlens::q::member_call_expr()` を実行。
5. `match.node<cxxlens::expr>("receiver")` の型を canonical 化。
6. method name と callee canonical decl を照合。
7. override fact で possible callees を構築。
8. `call_edge` と finding を生成。
9. dependent/unresolved cases は `unresolved_reason` として evidence に追加。

#### 結果 schema

```json
{
  "kind": "method_call_finding",
  "receiver_static_type": "app::DerivedService",
  "requested_receiver_type": "app::Service",
  "method_name": "start",
  "direct_callee": "app::DerivedService::start",
  "possible_virtual_callees": ["app::Service::start", "app::DerivedService::start"],
  "confidence": "high",
  "evidence": ["ast_match", "type_relation", "override_edge"]
}
```

### 20.2 ユースケース B: 禁止 API を検出し安全 API へ置換

```cpp
auto rule = rules::make_rule("project.banned.memcpy")
  .severity(cxxlens::severity::warning)
  .when(select::calls_to_function("::memcpy"))
  .unless(select::call().inside(select::function("project::low_level_copy")))
  .diagnose("Use project::copy_bytes with explicit span sizes.")
  .fix(refactor::replace_function_call("::memcpy", "project::copy_bytes"))
  .build();

auto report = rules::rule_pack{}.add(rule.value()).run(ws);
```

下位対応:

- detection: `q::call_expr()` + canonical callee。
- exception: enclosing function selector。
- fix: `rewrite_rule` + `edit_plan`。
- safety: macro reject、reparse verification。

### 20.3 ユースケース C: 外部入力から危険 sink への taint

```cpp
auto policy = flow::taint_policy{}
  .source(flow::source_model::call_return(select::calls_to_function("net::Request::param")))
  .sink(flow::sink_model::call_argument(select::calls_to_function("db::execute_sql"), 0))
  .barrier(flow::barrier_model::validation_call(select::calls_to_function("db::escape_sql")))
  .interprocedural(true);

auto report = flow::run_taint(ws, policy);
```

下位対応:

- local: `cxxlens::cfg_view` + `run_dataflow`。
- global: call graph + summary fact。
- path evidence: source/sink/steps。
- limitations: pointer alias/callback unresolved。

### 20.4 ユースケース D: mock/fake 生成

```cpp
auto plan = mock::generator::for_class("app::PaymentGateway")
  .options(mock::generation_options{}
    .framework(mock_framework::gmock)
    .output_header("test/mocks/mock_payment_gateway.h")
    .include_strategy(include_strategy::minimal_public)
    .fake_behavior(mock::fake_behavior::record_calls()))
  .plan(ws);
```

下位対応:

- `record_decl` 取得。
- public virtual method extraction。
- signature reconstruction。
- required type/include extraction。
- semantic copy。
- edit_plan generation。
- reparse verification。

### 20.5 ユースケース E: include hygiene

```cpp
auto report = safety::include_hygiene_pack().run(ws);
auto fixes = report.findings().fixes();
```

下位対応:

- `cxxlens::include_analyzer::analyze()`。
- include graph。
- symbol use fact。
- `edit::add_include` / `edit::remove_include`。

### 20.6 ユースケース F: PR 差分品質 gate

```cpp
auto diff = review::diff_view::from_git("origin/main").value();

auto review_report = review::review_workflow::for_diff(diff)
  .add(security::unsafe_c_api_pack())
  .add(safety::lifetime_pack())
  .gate(review::gate_policy::no_high_confidence_security_findings())
  .output_sarif("out/cxxred.sarif")
  .propose_fixes(true)
  .run(ws);
```

下位対応:

- changed files → `parse_changed_files`。
- findings → `only_changed_lines`。
- SARIF → finding_set export。
- fixes → edit_plan diff。

---

## 21. 公開 API 一覧サマリ

### 21.1 `cxxred-core`

| API | 目的 |
|---|---|
| `workspace_config` | project/build/context 設定。 |
| `workspace` | 実行単位。`cxxlens::project/session` を束ねる。 |
| `finding` / `finding_set` | ユースケース結果。 |
| `evidence` / `evidence_item` | 根拠。 |
| `confidence` | 確信度。 |
| `recipe` / `recipe_context` | 単一ユースケース実行単位。 |
| `workflow` | 複数 recipe/gate/output の実行単位。 |
| `unresolved_reason` | 解析不能・近似理由。 |

### 21.2 `cxxred-select`

| API | 目的 |
|---|---|
| `symbol_selector` | symbol 選択。 |
| `type_selector` | type 選択。 |
| `call_selector` | call 選択。 |
| `expr_selector` | expression 選択。 |
| `stmt_selector` | statement 選択。 |
| `include_selector` | include 選択。 |
| `macro_selector` | macro 選択。 |
| `to_cxxlens_query()` | 下位 query へ展開。 |

### 21.3 `cxxred-facts`

| API | 目的 |
|---|---|
| `fact` / `fact_id` / `fact_kind` | immutable fact。 |
| `fact_store` | fact の検索・保存・再構築。 |
| `fact_query` | fact 検索条件。 |
| `fact_extractor` | 独自 fact 抽出拡張。 |
| `symbol_ref` / `reference_ref` | symbol/reference fact。 |
| `call_edge` | call graph fact。 |
| `type_relation` | 型関係 fact。 |
| `include_edge` | include fact。 |
| `macro_expansion_fact` | macro fact。 |
| `fact_profile` | 必要 fact 群。 |

### 21.4 `cxxred-grep`

| API | 目的 |
|---|---|
| `grep::symbols()` | semantic symbol search。 |
| `grep::references_to()` | 参照検索。 |
| `grep::calls()` | call 検索。 |
| `grep::calls_to_function()` | function call 検索。 |
| `grep::calls_to_method()` | receiver type/method call 検索。 |
| `macro_search` | macro 使用検索。 |
| `include_search` | include 使用検索。 |
| `conversion_search` | implicit/user-defined conversion 検索。 |
| `grep_report` | 結果。 |

### 21.5 `cxxred-graph`

| API | 目的 |
|---|---|
| `semantic_graph` | 各種 graph。 |
| `graph_builder::call_graph()` | call graph。 |
| `graph_builder::class_hierarchy()` | class hierarchy。 |
| `graph_builder::include_graph()` | include graph。 |
| `graph_builder::dependency_graph()` | architecture dependency graph。 |
| `impact_query` | 変更影響解析。 |

### 21.6 `cxxred-rules`

| API | 目的 |
|---|---|
| `make_rule()` / `rule_builder` | rule DSL。 |
| `rule` | 単一 check。 |
| `rule_pack` | check 集合。 |
| `suppression_policy` | 抑制コメント/理由/期限。 |
| `rule_report` | findings/fixes/SARIF。 |

### 21.7 `cxxred-flow`

| API | 目的 |
|---|---|
| `flow_node` / `flow_path` | dataflow path。 |
| `source_model` | taint source。 |
| `sink_model` | taint sink。 |
| `barrier_model` | sanitizer/guard。 |
| `taint_policy` | taint 解析仕様。 |
| `run_taint()` | taint 実行。 |
| `resource_protocol` | acquire/use/release 検査。 |
| `check_resource_protocol()` | resource state check。 |

### 21.8 `cxxred-security` / `safety`

| API | 目的 |
|---|---|
| `security::*_pack()` | security rule pack。 |
| `safety::*_pack()` | reliability/safety rule pack。 |
| `models::api_model_pack` | source/sink/effect/danger/replacement model。 |
| `architecture_policy` | subsystem/layering rule。 |

### 21.9 `cxxred-refactor`

| API | 目的 |
|---|---|
| `codemod_recipe` | 変換 recipe。 |
| `replace_function_call()` | function API replacement。 |
| `replace_method_call()` | method API replacement。 |
| `rewrite_calls()` | call rewrite。 |
| `rename_symbol()` | symbol rename。 |
| `remove_unused_includes()` | include cleanup。 |
| `migrate_raw_pointer_to_unique_ptr()` | ownership migration。 |

### 21.10 `cxxred-mock`

| API | 目的 |
|---|---|
| `interface_model` | mock 対象 public surface。 |
| `generation_options` | mock/fake 生成設定。 |
| `mock::generator` | mock/fake 生成。 |
| `semantic_copier` | 依存型/公開面の semantic copy。 |
| `mock_plan` | edit_plan + warnings + explanation。 |

### 21.11 `cxxred-dynamic`

| API | 目的 |
|---|---|
| `qa_profile` | sanitizer/coverage/fuzzing profile。 |
| `dynamic_workflow` | instrument/build/run/report。 |
| `fuzz_recipe` | fuzz harness/campaign plan。 |
| `dynamic_report` | runtime findings/coverage/crashes。 |

### 21.12 `cxxred-review`

| API | 目的 |
|---|---|
| `diff_view` | PR/diff 対象。 |
| `baseline` | 既存違反管理。 |
| `gate_policy` | CI gate。 |
| `review_workflow` | review automation。 |
| `review_report` | SARIF/Markdown/patch/comments。 |

### 21.13 `cxxred-explain`

| API | 目的 |
|---|---|
| `why_matched()` | match 根拠。 |
| `why_not_matched()` | no match 理由。 |
| `explain_selector()` | selector 展開説明。 |
| `explain_recipe()` | recipe 説明。 |
| `ai_task_card` | AI に渡す schema/examples/context。 |

---

## 22. 下位 API との詳細対応マトリクス

| 上位ユースケース | `cxxred` API | `cxxlens` API | LLVM/Clang 公式機能 |
|---|---|---|---|
| semantic symbol search | `grep::symbols` | `workspace_index::symbols`, `q::function`, `q::record_decl` | LibTooling, ASTMatchers, clangd index |
| references | `grep::references_to` | `workspace_index::references` | clangd index, AST traversal |
| function calls | `grep::calls_to_function` | `q::call_expr`, `match::node`, `decl::canonical_decl` | ASTMatchers, ASTContext |
| method calls by type | `grep::calls_to_method` | `q::member_call_expr`, `type`, `record_decl::bases` | ASTMatchers, Clang AST |
| virtual candidates | `graph::class_hierarchy`, `call_selector::include_virtual_overrides` | raw CXXMethodDecl overrides via `raw()` | Clang AST CXXMethodDecl |
| macro use | `grep::macro_search` | `translation_unit::preprocessor`, `source_manager_view` | Preprocessor callbacks, SourceManager |
| include hygiene | `safety::include_hygiene_pack` | `include_analyzer`, `include_graph` | clangd Include Cleaner concepts |
| rule authoring | `rules::make_rule` | `check`, `check_runner`, `diagnostic` | clang-tidy, LibTooling |
| source-to-source rewrite | `refactor::codemod_recipe` | `rewrite_rule`, `transformer`, `edit_plan` | Clang Transformer, Refactoring Engine, AtomicChange |
| formatting | codemod format | `formatter::format_changed_ranges` | LibFormat |
| local dataflow | `flow::run_taint` local | `run_dataflow`, `cfg_view` | Clang dataflow/CFG |
| path-sensitive bug finding | `flow::path_sensitive_query` | CSA adapter | Clang Static Analyzer |
| mock generation | `mock::generator` | `mock_generator`, `semantic_copier` | Clang AST, Replacements |
| sanitizer workflow | `dynamic::dynamic_workflow` | `dynamic_runner`, `sanitizer_profile` | Sanitizers |
| fuzzing | `dynamic::fuzz_recipe` | `fuzz_harness_generator` | libFuzzer, SanitizerCoverage |
| coverage | `dynamic::qa_profile::coverage` | dynamic report importer | source-based coverage |
| PR gate | `review::review_workflow` | `check_runner`, `edit_plan`, report export | SARIF, clang-tidy style workflows |

---

## 23. 設定ファイル設計

### 23.1 `.cxxred.yaml`

```yaml
version: 1
workspace:
  build_dir: build
  cache_dir: .cxxred/cache
  generated_code:
    policy: exclude_by_default
    patterns:
      - "**/generated/**"
      - "**/*.pb.cc"
  system_headers: exclude
  profiles:
    default:
      fact_profile: semantic_grep
      precision: workspace_semantic

rules:
  enable:
    - security.*
    - safety.lifetime.*
    - project.*
  disable:
    - safety.experimental.*
  suppression:
    clang_tidy_nolint: true
    require_reason: true

models:
  api_model_files:
    - tools/cxxred/models/project_apis.yaml

review:
  baseline: .cxxred/baseline.json
  sarif: out/cxxred.sarif
  gate:
    changed_lines_only: true
    min_severity: warning
    min_confidence: probable

refactor:
  macro_policy: reject_macro_expansions
  format: true
  verify_reparse: true
```

### 23.2 API model YAML

```yaml
apis:
  - name: project.read_request_param
    function: app::Request::param
    effects:
      returns_untrusted_input: true

  - name: project.execute_sql
    function: db::execute_sql
    sinks:
      - arg: 0
        kind: sql

  - name: project.escape_sql
    function: db::escape_sql
    barriers:
      - kind: sql_escape

  - name: legacy.copy
    function: ::memcpy
    danger:
      kind: unsafe_memory_copy
    replacement: project::copy_bytes
```

---

## 24. CLI 設計

```text
cxxred init                         # 設定生成
cxxred facts build                  # fact cache 作成
cxxred grep symbols                 # symbol search
cxxred grep calls                   # call search
cxxred grep refs                    # reference search
cxxred graph call                   # call graph output
cxxred graph include                # include graph output
cxxred rules run                    # rule pack 実行
cxxred flow taint                   # taint policy 実行
cxxred refactor plan                # codemod dry-run
cxxred refactor apply               # codemod apply
cxxred mock generate                # mock/fake 生成
cxxred dynamic run                  # sanitizer/coverage 実行
cxxred fuzz plan                    # fuzz harness plan
cxxred review                       # diff review workflow
cxxred explain selector             # selector 展開説明
cxxred explain finding              # finding 根拠説明
```

CLI はライブラリ API の薄い wrapper であり、全機能は C++ API からも使用可能にする。

---

## 25. エラー処理・限界表現

### 25.1 unresolved_kind

```cpp
enum class unresolved_kind {
  missing_compile_command,
  parse_failed,
  ambiguous_symbol,
  dependent_type,
  unresolved_overload,
  macro_expansion_unsafe,
  generated_code_readonly,
  missing_module_bmi,
  function_pointer_target_unknown,
  callback_target_unknown,
  virtual_dynamic_type_unknown,
  alias_analysis_required,
  dataflow_budget_exceeded,
  path_sensitive_budget_exceeded,
  sanitizer_build_failed,
  coverage_data_missing,
  unsupported_language_extension,
  custom
};
```

### 25.2 Error policy

- parse 失敗は workspace 全体失敗にせず、TU-level unresolved として保持可能。
- ただし `require_exact_compile_commands` の場合、compile command 欠落は fatal。
- 編集系 recipe は unresolved がある場合、既定で plan を invalid にする。
- grep 系 recipe は unresolved を findings の evidence として返す。
- flow 系 recipe は budget 超過時に partial result + confidence drop を返す。

---

## 26. 性能・スケーラビリティ設計

### 26.1 原則

- AST は長期共有しない。immutable fact に抽出する。
- full parse を避けられる場面では fact cache を使う。
- query は selector → fact prefilter → TU parse → AST verification の順で narrowing する。
- high precision は明示 opt-in。
- CI では diff と dependency graph により解析範囲を削る。

### 26.2 Cache key

```text
cache_key = hash(
  llvm_version,
  cxxlens_version,
  compile_command.arguments,
  file_content_hash,
  included_header_hashes,
  model_pack_hash,
  fact_extractor_version
)
```

### 26.3 Query planning

`cxxred` は selector をそのまま全 TU に流さず、query plan を作る。

```cpp
class query_plan {
public:
  std::vector<fact_query> fact_prefilters() const;
  std::vector<cxxlens::query> tu_queries() const;
  precision_level precision() const;
  std::string explain() const;
};
```

例: `calls_to_method(receiver_type="Service", method="start")`

1. fact store で `method_name=start` の call_edge を prefilter。
2. receiver type が記録済みなら fact だけで high confidence。
3. receiver type 不明の call site のみ TU parse で AST verification。
4. virtual dispatch が必要なら class hierarchy fact を参照。

---

## 27. テスト戦略

### 27.1 Library 自体のテスト

| 種類 | 内容 |
|---|---|
| golden tests | 小さな C++ snippet に対し findings/fixes を期待。 |
| compile DB tests | 複数 flags/standards/config を検証。 |
| macro tests | spelling/expansion/macro policy。 |
| template tests | pattern/instantiation/dependent type。 |
| refactor idempotence | codemod を 2 回適用して変化なし。 |
| reparse verification | edit_plan 適用後 parse 成功。 |
| fuzz tests | selector parser/config parser/model parser。 |
| differential tests | clang-tidy/clang-query との差分確認。 |

### 27.2 User recipe のテスト API

```cpp
namespace cxxred::testing {

class recipe_fixture {
public:
  static recipe_fixture cpp(std::string code);
  recipe_fixture& file(cxxlens::path, std::string code);
  recipe_fixture& compile_arg(std::string);
  recipe_fixture& standard(std::string);
  recipe_fixture& model_pack(models::api_model_pack);
  cxxlens::result<workspace> workspace();
};

class recipe_golden_test {
public:
  recipe_golden_test& run(recipe&);
  recipe_golden_test& expect_finding(std::string message_contains);
  recipe_golden_test& expect_confidence(confidence);
  recipe_golden_test& expect_fix_contains(std::string text);
  recipe_golden_test& expect_no_unresolved();
  cxxlens::result<void> verify();
};

} // namespace cxxred::testing
```

---

## 28. ロードマップ

### Phase 0: Vertical slice

- `workspace`、`fact_store` minimal。
- `select::call_selector`。
- `grep::calls_to_method`。
- `finding/evidence` schema。
- `explain::why_matched`。

### Phase 1: Semantic grep toolkit

- symbol/reference/call/type/include/macro search。
- class hierarchy、override graph。
- JSON/Markdown output。
- `.cxxred.yaml`。

### Phase 2: Rule/refactor/mock MVP

- rule DSL。
- SARIF output。
- replace API codemod。
- include cleanup。
- mock/fake public surface extraction。

### Phase 3: Flow/security/review

- local dataflow/taint。
- resource protocol。
- security/safety model packs。
- diff review、baseline、gate。

### Phase 4: Dynamic QA

- sanitizer profiles。
- coverage import。
- fuzz harness generation。
- static/dynamic evidence association。

### Phase 5: Advanced ecosystem

- global dataflow summaries。
- CSA adapter。
- C++ modules/BMI support。
- custom fact plugin ABI。
- AI task cards and tool-generation SDK。

---

## 29. `cxxred` が libTooling 直書きより優れる理由

| 観点 | libTooling/ASTMatchers 直書き | `cxxred` |
|---|---|---|
| API 発見性 | matcher/reference を調べる必要。 | `grep::calls_to_method()` のようにユースケース名から開始。 |
| 多態性 | 自分で class hierarchy/override を構築。 | receiver type、derived、virtual candidates を標準オプション化。 |
| 結果根拠 | 自前でログ/diagnostic を作る。 | evidence/confidence/unresolved が標準。 |
| 安全編集 | Replacements、AtomicChange、format、reparse を自分で統合。 | edit_plan pipeline に統合済み。 |
| CI | clang-tidy/SARIF/baseline/diff を自分で構築。 | review workflow。 |
| mock/fake | AST から signature/include を自分で復元。 | public surface extraction + semantic copy。 |
| AI 利用 | 失敗原因が構造化されにくい。 | explain/schema/examples が標準。 |
| 未対応 | raw Clang 直書きに戻る。 | `cxxred` → `cxxlens` → raw Clang へ段階的に降りる。 |

---

## 30. 重要な設計上の約束

1. `cxxred` は `cxxlens` の機能を隠して殺さない。
2. すべての上位 recipe は、下位 query/edit/dataflow/check に展開できることを説明できる。
3. 正規表現は補助 filter であり、C/C++ 意味解析の主手段にしない。
4. finding は必ず evidence と confidence を持つ。
5. 近似・未解決・budget 超過は隠さない。
6. 編集は必ず dry-run 可能で、reparse verification を標準化する。
7. macro/generated/system header は policy で明示する。
8. AI 向け schema は stable に保つ。
9. rule/model/recipe は YAML/JSON で宣言可能にし、C++ API と対応させる。
10. 上位 API の不足は custom recipe/fact/selector で補える。

---

## 31. 付録 A: 代表 API 使用例集

### A.1 派生型を含むメソッド呼び出し

```cpp
auto report = grep::calls_to_method()
  .receiver_type("core::Allocator")
  .method_name("allocate")
  .include_derived_types(true)
  .include_virtual_overrides(true)
  .run(ws);
```

### A.2 enum value 使用箇所

```cpp
auto report = grep::references_to(select::symbol()
  .qualified_name("app::Mode::Experimental")
  .kind(symbol_kind::enum_constant))
  .run(ws);
```

### A.3 implicit narrowing conversion

```cpp
auto report = grep::conversion_search{}
  .kind(conversion_kind::narrowing)
  .from(select::type("long"))
  .to(select::type("int"))
  .run(ws);
```

### A.4 lock/unlock protocol

```cpp
auto protocol = flow::resource_protocol{}
  .resource("mutex")
  .acquire(select::calls_to_method("std::mutex", "lock"))
  .release(select::calls_to_method("std::mutex", "unlock"))
  .use(select::calls_to_method("SharedState", "mutate"));

auto findings = flow::check_resource_protocol(ws, protocol);
```

### A.5 public API diff

```cpp
auto impact = graph::impact_query::from_changed_symbols(changed_public_symbols)
  .include_callers(true)
  .include_tests(true)
  .max_depth(4)
  .run(ws);
```

### A.6 gMock 生成

```cpp
auto mock = mock::generator::for_class("net::Transport")
  .options(mock::generation_options{}
    .framework(mock_framework::gmock)
    .output_header("tests/mock_transport.h"))
  .plan(ws);
```

### A.7 fuzz harness 生成

```cpp
auto fuzz = dynamic::fuzz_recipe::for_function(select::function("codec::ParsePacket"))
  .input_model(fuzz_input_model::bytes())
  .sanitizer_profile(dynamic::qa_profile::memory())
  .plan(ws);
```

---

## 32. 参考資料

[^clang-docs]: Clang 23 documentation, “Using Clang as a Library”, https://clang.llvm.org/docs/index.html
[^libtooling]: Clang documentation, “LibTooling”, https://clang.llvm.org/docs/LibTooling.html
[^ast-matcher-reference]: Clang documentation, “AST Matcher Reference”, https://clang.llvm.org/docs/LibASTMatchersReference.html
[^transformer]: Clang documentation, “Clang Transformer Tutorial”, https://clang.llvm.org/docs/ClangTransformerTutorial.html
[^clang-tidy]: Extra Clang Tools documentation, “Clang-Tidy”, https://clang.llvm.org/extra/clang-tidy/
[^csa]: Clang documentation, “Clang Static Analyzer”, https://clang.llvm.org/docs/ClangStaticAnalyzer.html
[^dataflow]: Clang documentation, “Data flow analysis: an informal introduction”, https://clang.llvm.org/docs/DataFlowAnalysisIntro.html
[^json-cdb]: Clang documentation, “JSON Compilation Database Format Specification”, https://clang.llvm.org/docs/JSONCompilationDatabase.html
[^include-cleaner]: clangd design documentation, “Include Cleaner”, https://clangd.llvm.org/design/include-cleaner
[^source-coverage]: Clang documentation, “Source-based Code Coverage”, https://clang.llvm.org/docs/SourceBasedCodeCoverage.html
[^asan]: Clang documentation, “AddressSanitizer”, https://clang.llvm.org/docs/AddressSanitizer.html
[^libfuzzer]: LLVM documentation, “libFuzzer – a library for coverage-guided fuzz testing”, https://llvm.org/docs/LibFuzzer.html
[^dfsan]: Clang documentation, “DataFlowSanitizer”, https://clang.llvm.org/docs/DataFlowSanitizer.html
[^sarif]: OASIS SARIF Technical Committee, https://www.oasis-open.org/committees/tc_home.php?wg_abbrev=sarif
[^codeql-dataflow]: GitHub CodeQL documentation, “Analyzing data flow in C and C++”, https://codeql.github.com/docs/codeql-language-guides/analyzing-data-flow-in-cpp/

---

## 33. 結論

`cxxlens` が LLVM/Clang の強力さを安全なプリミティブとして取り出す下位層だとすれば、`cxxred` はそれらを **品質保証・コード調査・変換・テスト生成・レビュー自動化という実用目的に再構成する上位層**である。

この二階層構造により、利用者は次のいずれの抽象度でも作業できる。

- 「この API 呼び出しを探したい」なら `cxxred::grep`。
- 「この規則を CI で gate したい」なら `cxxred::rules` / `review`。
- 「この API を安全に置換したい」なら `cxxred::refactor`。
- 「この class の mock/fake を作りたい」なら `cxxred::mock`。
- 「この taint を追いたい」なら `cxxred::flow`。
- 「既存 API で足りない」なら `cxxred` の custom recipe から `cxxlens`、さらに Clang raw API へ降りる。

つまり `cxxred` は、LLVM/Clang の叡智をさらにアプリケーション開発者と AI エージェントに近づけるための、ユースケース駆動の C/C++ 解析アプリケーションフレームワークである。
