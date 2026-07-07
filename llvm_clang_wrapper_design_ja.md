# LLVM/Clang ラッパライブラリ構想設計書

**仮称:** `cxxlens`  
**対象:** LLVM/Clang 関連 API を尊重しつつ、初心者・熟練者・AI コーディングエージェントが C++ 解析・検査・変換・モック/フェイク生成・動的解析支援を安全かつ高速に実装できる C++23 ライブラリ群  
**作成日:** 2026-07-05  
**状態:** 実装前の要件定義・外部設計・公開 API 設計

---

## 0. エグゼクティブサマリー

`cxxlens` は、Clang/LLVM の既存機能を隠蔽して簡略化するだけの薄いラッパではなく、**「正規表現ではなく C++ の意味構造を扱う」ことを日常的な開発行為にするための統合ツールキット**として設計する。

中核方針は次の 5 点である。

1. **AST/型/名前解決/プリプロセッサ/ビルド設定を第一級概念にする。** 文字列検索・正規表現ベースの解析を標準経路にしない。
2. **高水準 API と Clang 直呼び API を同時に提供する。** 未対応機能に遭遇したときでも、ユーザーがライブラリを捨てずに `raw()` / `unsafe_raw()` / `with_clang()` から libTooling、ASTMatchers、Transformer、clang-tidy、Static Analyzer、LLVM Pass へ降りられる。
3. **安全なデフォルトを強制する。** 既定では `IgnoreUnlessSpelledInSource` 相当の「ソースに書かれたもの中心」の AST traversal、マクロ展開部の非編集、トランザクション編集、再パース検証、clang-format 適用、compile_commands.json の明示確認を行う。
4. **AI エージェントにとって機械可読な API にする。** API は一貫した命名、自己説明的な型、JSON/SARIF/Markdown 出力、dry-run、差分プレビュー、`explain()`、`schema()`、`examples()` を備える。
5. **スケールを前提にする。** 単一ファイル用の `run_on_code()` から、compile database、preamble cache、background index、差分解析、並列 TU 解析、sanitizer/fuzzer 実行計画まで同じ設計体系で扱う。

重要なのは、`cxxlens` が「libTooling を使えなくする抽象化」ではなく、**libTooling をより安全・予測可能・発見可能・組み合わせ可能にする上位レイヤー**である点である。高水準 API で 80% のユースケースを短く書き、残り 20% は公式 API へ型付きで脱出できる。この設計により「ラッパがしょぼくて作れない」という状態を避ける。

---

## 1. 背景と問題設定

C++ の品質保証では、次のような課題が今後さらに重要になる。

- 人間レビューや AI レビューだけでは、巨大コードベースでの網羅性・再現性・差分追跡に限界がある。
- 機械可検な規則は、静的解析・動的解析・自動修正・自動テスト生成へ委譲すべきである。
- C++ は Rust などに置き換わる領域がある一方、既存資産・低レイヤ・性能要求・ABI 制約などにより長期にわたり残る。
- 正規表現やテキスト処理では、C++ のテンプレート、オーバーロード、マクロ、名前空間、型推論、`using`、ADL、プリプロセッサ条件分岐、生成コード、ビルド設定差分を正しく扱えない。
- Clang/LLVM は強力だが、API が巨大で、所有権・ライフタイム・`SourceLocation`・マクロ・`ASTContext`・`FrontendAction`・`CompilationDatabase`・`Replacements` などの理解に壁がある。

`cxxlens` はこの壁を下げる。目標は「LLVM/Clang の専門家だけが使える解析基盤」ではなく、**C++ を機械的に理解するための標準的な作業台**を提供することである。

---

## 2. 公式 LLVM/Clang API の調査結果

この章では、設計の前提となる LLVM/Clang 関連ライブラリの役割を整理する。記載は主に公式ドキュメントに基づく。

### 2.1 Clang をライブラリとして使う主要インターフェース

Clang 公式ドキュメントは、C/C++/Objective-C 系言語向けのフロントエンドおよびツーリング基盤として、複数の利用経路を提供している。Clang のドキュメント索引には「Using Clang as a Library」として、LibTooling、LibClang、LibFormat、Plugins、RecursiveASTVisitor、LibASTMatchers、Clang Transformer、ASTImporter、JSON Compilation Database、Refactoring Engine などが並んでいる。[^clang-index]

| 領域 | 公式コンポーネント | できること | 注意点 | `cxxlens` での扱い |
|---|---|---|---|---|
| 安定 C API | libclang | C API で AST cursor、source location、diagnostics などにアクセス。IDE/外部言語バインディング向き。 | 完全な C++ AST 情報や変換用途には不足しがち。 | 外部言語連携・軽量インデックス・安定 ABI 境界として補助利用。 |
| スタンドアロンツール | LibTooling | `FrontendAction` を用いて実ファイルまたは in-memory code を解析。`CompilationDatabase` と連携。 | C++ API は強力だが、学習コスト・ライフタイム・ビルド設定の扱いが難しい。 | 中核基盤。Project/Session/TU の高水準 API で包む。 |
| コンパイル時拡張 | Clang Plugins | コンパイル中に独自 `PluginASTAction` を実行。 | ビルドシステム統合が必要。外部単独実行には不向き。 | `cxxlens-plugin-adapter` で同一 Check を plugin/CLI 両対応にする。 |
| AST traversal | RecursiveASTVisitor | AST ノードを C++ visitor で巡回。 | 全ノード種別・暗黙ノード・テンプレート instantiation などの扱いが難しい。 | 低レベル脱出口と、高水準 `visit()` API の実装基盤。 |
| AST パターン | LibASTMatchers | C++ DSL で AST パターン検索。clang-query と親和性。 | matcher の発見、暗黙ノード、型の bind、source range の扱いに癖がある。 | `cxxlens::query` で意図中心の DSL に再構成。raw matcher も受け入れる。 |
| 変換 | Transformer / Refactoring Engine / Replacements / AtomicChange | AST match に基づく rewrite、diagnostics、atomic edits、header insertion/deletion。 | 複数ファイル・衝突・整形・再検証・マクロ編集の安全性を自分で担保する必要がある。 | `edit_plan` / `rewrite_rule` / `transaction` として安全化。 |
| 整形 | LibFormat | `reformat()` が `tooling::Replacements` を返し、LLVM/Google/Chromium などの style を扱う。 | 編集範囲・style 解決・include 整理との接続を設計する必要がある。 | 編集適用前後の標準ステップに組み込む。 |
| 静的 lint | clang-tidy | LibTooling ベースの C++ linter。checks、fix-it、Clang Static Analyzer check 実行にも対応。 | check 実装の boilerplate が多い。オプションや matcher 登録に慣れが必要。 | `cxxlens-check` を clang-tidy 互換にし、同一 check を CLI/SARIF/clang-tidy 化。 |
| 経路依存静的解析 | Clang Static Analyzer | path-sensitive inter-procedural symbolic execution と checker。 | checker 開発は難度が高い。内部 API 理解が必要。 | P2 で adapter。P1 では CSA 結果の取り込みと既存 checker 実行を優先。 |
| データフロー解析 | Clang dataflow | CFG 上で全経路に対する事実を fixpoint 計算。 | lattice/transfer/join 設計が必要。 | `dataflow_analysis<Lattice>` と canned lattice を提供。 |
| 全コードベース情報 | clangd index / Include Cleaner | symbols、references、relations、headers の利用状況、background index。 | clangd 内部設計はサービス向きで、AST は thread-safe ではない。 | `workspace_index` として抽出 fact を immutable に保持。 |
| 動的解析 | Sanitizers / SanitizerCoverage / libFuzzer / source-based coverage / CFI | ASan/TSan/MSan/UBSan/LSan/DFSan/RTSan/coverage/fuzzing/control-flow integrity。 | ビルド・リンク・実行・レポート収集・環境差分が複雑。 | `dynamic_runner` で instrument/build/run/report を宣言的に扱う。 |
| LLVM IR | LLVM IR / New Pass Manager / ORC JIT | SSA-based IR、analysis/transform pass、JIT。 | C++ ソース AST とは別抽象。用途を分ける必要。 | ソース解析とは別 module `cxxlens-ir`。AST から IR 名寄せを限定的に提供。 |

### 2.2 LibTooling

LibTooling は、Clang を用いたスタンドアロンツール作成の基本基盤である。公式ドキュメントでは、`runToolOnCode` による in-memory 実行、`CompilationDatabase`、`CommonOptionsParser`、`ClangTool`、`FrontendActionFactory` といった典型経路が示されている。[^libtooling]

`cxxlens` にとって LibTooling は最重要基盤である。ただし、利用者に `FrontendAction`、`ASTConsumer`、`CompilerInstance`、`SourceManager`、`ASTContext` を毎回直接組み立てさせない。代わりに以下の抽象を提供する。

- `project`: compile_commands.json と toolchain 情報の正規化。
- `analysis_session`: VFS、preamble/cache、diagnostics、parallelism を含む解析セッション。
- `translation_unit`: `ASTContext`、`Preprocessor`、`SourceManager`、diagnostics を一つの lifetime に束ねる。
- `tool`: query/check/transform/index/dynamic などを同じ runner で実行する単位。
- `raw()` / `with_clang()`: 必要時に `CompilerInstance` などへ降りる脱出口。

### 2.3 libclang

libclang は C API で、`CXIndex`、`CXTranslationUnit`、`CXCursor`、cursor traversal、source location などを提供する。公式チュートリアルは、C++ API より安定した外部インターフェースとしての利用を示している。[^libclang]

`cxxlens` は中核を Clang C++ API で実装するが、libclang は次の用途に限って活用できる。

- Python/Go/Rust など外部言語バインディングの薄い互換レイヤ。
- 「完全変換は不要だが symbol 一覧だけ欲しい」軽量用途。
- `cxxlens` の stable schema と libclang cursor 情報の対応付け。

ただし、テンプレートや詳細な C++ AST、source-to-source 変換の精度が必要な用途では libclang を主経路にしない。

### 2.4 Clang AST、RecursiveASTVisitor、ASTContext

Clang AST は、書かれた C++ ソースと C++ 標準の構造に近い形を保つため、リファクタリングや静的検査に向いている。公式 AST 入門では、`ASTContext` が AST 情報を束ね、`Decl`、`Stmt`、`Type`、`DeclContext` など複数階層が存在し、`RecursiveASTVisitor` が AST 全体の巡回手段になることが説明されている。[^clang-ast]

`cxxlens` は AST を次のように扱う。

- AST ノードを `ast_node` / `decl` / `stmt` / `expr` / `type` / `entity` に包む。
- 生ポインタ寿命を `translation_unit` に結びつけ、dangling を API 設計で避ける。
- `spelled_range()`、`expanded_range()`、`semantic_range()` を分ける。
- 暗黙ノード、テンプレート instantiation、マクロ展開の扱いを `traversal_policy` と `macro_policy` で明示する。
- raw Clang ノードへ `raw()` でアクセスできる。

### 2.5 ASTMatchers と traversal mode

LibASTMatchers は、AST を C++ DSL で検索する強力な基盤である。公式ドキュメントでは `recordDecl(hasName("Foo"), isDerivedFrom("Bar"))` のような matcher が示され、LibTooling と組み合わせた query/変換ツール作成に使われる。[^astmatchers]

一方、AST Matcher Reference は、既定 traversal が暗黙ノードも対象にすること、`IgnoreUnlessSpelledInSource` / `traverse(TK_IgnoreUnlessSpelledInSource, ...)` によって、ソースに書かれたものに限定する方が変換で安全な場合があることを明記している。[^matcher-reference]

`cxxlens` の query API は、既定で `spelled_only` とする。

```cpp
auto q = cxxlens::q::function()
  .name("do_work")
  .is_definition()
  .from_main_file()
  .bind("fn");
```

これは内部的には ASTMatchers を使うが、利用者は matcher の名前探索に悩みにくい。必要なら次のように raw matcher も渡せる。

```cpp
auto raw = clang::ast_matchers::functionDecl(
  clang::ast_matchers::hasName("do_work"));
auto q = cxxlens::q::from_raw(raw).bind("fn");
```

### 2.6 Transformer、Refactoring Engine、Replacements、AtomicChange、LibFormat

Clang Transformer は、AST matcher の pattern と edit を組み合わせて C++ diagnostics / transformations を構築するフレームワークで、低レベル Clang の複雑さを隠す目的を持つ。[^transformer] Refactoring Engine は local refactoring、source replacements、symbol occurrences、AtomicChanges などのモデルを提供する。[^refactoring-engine] `AtomicChange` は source edits と header insertions/deletions を関連する変更単位としてまとめ、全適用または非適用の atomicity を持つ。[^atomic-change] LibFormat は `reformat()` により formatting replacements を返し、style を設定できる。[^libformat]

`cxxlens` はこれらを以下のように統合する。

- `edit`: 単一挿入/置換/削除/ヘッダ変更。
- `atomic_edit`: 意味的に一つの変更。
- `edit_plan`: 複数ファイルにまたがる全体変更。conflict detection、format、reparse verification を含む。
- `rewrite_rule`: query + edit generator + diagnostics。
- `transaction`: dry-run、preview、apply、rollback metadata。

### 2.7 JSON Compilation Database とビルド文脈

Clang tools が実際の C++ を正しく解析するには、`-I`、`-D`、`-std`、target、resource-dir などのコンパイル引数が不可欠である。JSON Compilation Database は、ビルドシステムから独立して単一 translation unit のコンパイルを再現するための形式で、CMake の `CMAKE_EXPORT_COMPILE_COMMANDS` や Bear などで生成できる。[^json-cdb] clangd の compile command 仕様も、ソースコード解析にはコンパイル文脈が必要で、特に include path、標準、define、target などが重要であると説明している。[^clangd-compile]

`cxxlens` は compile database を「任意」ではなく「第一級入力」とする。fallback parse は明示オプションにし、既定では warning ではなく structured diagnostic を出す。

### 2.8 clang-tidy

clang-tidy は Clang ベースの C++ linter で、style violation、interface misuse、静的バグなどの check を提供し、LibTooling を基盤とする。Clang Static Analyzer checks の実行、glob による check 選択、parallel 実行、diff 行のみの警告フィルタ、`NOLINT` による抑制などもサポートする。[^clang-tidy] check 実装では `ClangTidyCheck`、`registerMatchers()`、`check()`、`registerPPCallbacks()`、options の保存などを用いる。[^clang-tidy-dev]

`cxxlens` の check framework は clang-tidy と競合しない。むしろ、同じ check 定義から次を生成できる設計にする。

- `cxxlens check` CLI 実行。
- clang-tidy module としての adapter。
- SARIF/JSON/Markdown レポート。
- 自動修正 `edit_plan`。
- AI 向け explainable diagnostic。

### 2.9 Clang Static Analyzer と dataflow

Clang Static Analyzer は C/C++/Objective-C 向けの source code analysis tool で、path-sensitive inter-procedural symbolic execution を用い、checker が bug report を出す。[^csa] checker 開発マニュアルでは symbolic execution、ExplodedGraph、ProgramState、checker callback などが説明されている。[^csa-checker]

Clang dataflow は、CFG を用いて全経路に対してプログラム事実を証明する static analysis の考え方を提供し、fixpoint propagation を行う。[^dataflow]

`cxxlens` では、P1 として dataflow API を提供し、P2 として CSA checker authoring adapter を提供する。

- Dataflow は型付き lattice と transfer function による比較的扱いやすい API にする。
- CSA は内部 API の複雑さが大きいため、最初は「既存 analyzer 実行 + レポート正規化 + checker skeleton 生成」を提供し、徐々に path-sensitive adapter を拡張する。

### 2.10 clangd index、Include Cleaner、threading

clangd index は、symbols、references、relations を保存し、FileIndex、BackgroundIndex、static index などの設計を持つ。BackgroundIndex は compile database に基づき全ファイルを parse し、cache する。[^clangd-index] Include Cleaner は AST を用いて使用ヘッダを判定し、IWYU pragmas を尊重し、未使用/不足 include の診断に関わる。[^include-cleaner]

clangd の threading 設計では、preamble は AST と分離され thread-safe に共有できる一方、AST は thread-safe ではないため、file ごとの worker で管理される。[^clangd-threading]

`cxxlens` はこの知見に従い、AST そのものをスレッド間で自由共有しない。代わりに、`symbol_fact`、`reference_fact`、`include_fact` などの immutable fact を抽出して cross-TU / cross-thread で利用する。

### 2.11 LLVM IR、Pass Manager、ORC JIT

LLVM IR は SSA-based、type-safe、low-level かつ flexible な共通コード表現であり、in-memory、bitcode、assembly の形を持つ。Verifier が well-formedness を検査する。[^llvm-ir] New Pass Manager は Module/CGSCC/Function/Loop などの単位で analysis/transform pass を構成し、`PassBuilder` や adaptor を用いる。[^llvm-pass] ORCv2 の `LLJIT` は LLVM IR をコンパイルし relocatable object とリンクする JIT 基盤である。[^orc]

`cxxlens` の主目的はソースレベル C++ 解析だが、次の用途で `cxxlens-ir` を提供する。

- IR pass を用いた低レベル解析。
- sanitizer/fuzzer/coverage と IR instrumentation の統合。
- ソース AST 上の symbol と IR function/global の限定的対応付け。
- JIT を用いた小規模評価・生成コード検証。

### 2.12 Sanitizers、coverage、libFuzzer、CFI

Clang/LLVM は多様な動的解析基盤を持つ。

| 技術 | 主な目的 | `cxxlens` での位置づけ |
|---|---|---|
| AddressSanitizer | heap/stack/global out-of-bounds、use-after-free、double-free、leak などの検出。典型 slowdown 約 2x。[^asan] | memory safety profile。 |
| ThreadSanitizer | data race 検出。compiler instrumentation + runtime。[^tsan] | concurrency profile。 |
| MemorySanitizer | uninitialized memory use 検出。[^msan] | initialization profile。 |
| UndefinedBehaviorSanitizer | array OOB、shift、null/misaligned、signed overflow 等の UB 検出。[^ubsan] | UB profile。 |
| LeakSanitizer | leak 検出。ASan と統合可能。[^lsan] | leak-only profile。 |
| DataFlowSanitizer | data flow tag を追跡する汎用 dynamic data flow analysis。[^dfsan] | taint tracking / provenance profile。 |
| TypeSanitizer | strict type aliasing violation 検出。[^tysan] | aliasing profile。 |
| RealtimeSanitizer | `[[clang::nonblocking]]` contexts で malloc/free/mutex lock 等の realtime violation を検出。[^rtsan] | realtime profile。 |
| Function Effect Analysis | `nonblocking` / `nonallocating` 等の compile-time warning。[^function-effect] | static + dynamic realtime policy。 |
| SanitizerCoverage | function/basic-block/edge などに coverage instrumentation を挿入。[^sancov] | fuzzing/coverage adapter。 |
| Source-based coverage | AST/preprocessor information に基づく coverage。[^source-coverage] | report importer。 |
| Control Flow Integrity | control-flow を崩す UB を検出/abort。LTO が必要な mode が多い。[^cfi] | hardening profile。 |
| libFuzzer | in-process coverage-guided evolutionary fuzzing。SanitizerCoverage と連携。[^libfuzzer] | fuzz harness generator / runner。 |

`cxxlens-dynamic` は sanitizer そのものを再実装しない。代わりに、**計測ビルド・実行・レポート解析・CI 統合・コード位置への対応付け**を提供する。

### 2.13 LLVM Support、Basic、Diagnostics、TableGen

LLVM/Clang の内部には、`libSupport`、`Basic`、Diagnostics、SourceManager、Lexer、Parser、AST、Sema、CodeGen、CFG、ASTImporter などの層がある。Clang Internals Manual は、source buffer、source location、diagnostics、tokens、target/language options などが Basic library に含まれることを説明している。[^clang-internals]

LLVM Programmer’s Manual は `Expected<T>` / `Error` の使用を説明し、全ての `Error` は check または move されなければならないという規律を示している。[^llvm-error] TableGen は、多数の domain-specific records を保守し、重複やミスを減らすための仕組みで、Clang diagnostics や attributes などにも使われる。[^tablegen]

`cxxlens` は以下を採用する。

- `result<T>` は `llvm::Expected<T>` に近い意味を持つが、公開 surface は安定化する。
- diagnostics は machine-readable ID、severity、category、location、notes、fixes、explanation を必須にする。
- matcher/check/options/schema などの反復的コードは TableGen または独自 declarative schema で生成可能にする。

---

## 3. 良いライブラリに求める評価軸

「libTooling 直呼びより優れている」ことを判断するため、`cxxlens` は次の評価軸を満たす必要がある。

| 評価軸 | 要求 | `cxxlens` の設計方針 |
|---|---|---|
| 正確性 | C++ の意味構造を壊さない。 | compile command 必須、AST/型/名前解決/PP を利用。正規表現を解析の主手段にしない。 |
| 完全性 | ラッパの都合でやりたいことができなくならない。 | 全層に raw escape hatch、custom action/matcher/pass/check を受け入れる。 |
| 安全性 | 誤変換・マクロ破壊・部分適用を防ぐ。 | macro policy、transactional edits、conflict detection、format、reparse verification。 |
| 予測可能性 | 初見でも API の意味が推測できる。 | `project → session → translation_unit → query/check/transform` の一貫構造。 |
| 発見可能性 | API 探索が容易。 | fluent DSL、`schema()`、`examples()`、typed enum、diagnostic suggestions。 |
| 段階的学習 | 初心者から LLVM 熟練者まで使える。 | high-level one-liner、mid-level builder、low-level raw の三段階。 |
| 性能 | 大規模コードベースでもスケールする。 | preamble cache、background index、diff scope、parallel TU、immutable facts。 |
| 保守性 | LLVM バージョン更新に耐える。 | version shim、feature gates、compatibility tests、generated adapters。 |
| 可観測性 | なぜその結果になったか説明できる。 | `explain_match()`、`trace_query()`、`explain_fix()`、debug AST dump。 |
| AI 親和性 | AI が API を間違えにくい。 | 小さい型付き API、JSON schema、dry-run、explicit error、サンプル自動生成。 |
| テスト容易性 | check/transform を小さく検証できる。 | `run_on_code()`、fixture compile DB、golden diff、reparse assertion、fuzz test。 |
| CI 互換 | 現実の開発フローで使える。 | SARIF、JUnit、clang-tidy export、Git diff filter、exit code policy。 |
| 拡張性 | 新しい解析を追加しやすい。 | plugin registry、check registry、query extension、pass adapter、dynamic profile。 |
| 透明性 | 何を隠し、何を隠していないか明確。 | public API と raw API の境界を文書化。 |

---

## 4. 要件定義

### 4.1 上位目標

`cxxlens` は次を可能にする。

1. **C++ コード解析を正規表現なしで実装する。** 変数、関数、class、継承、template、call、type、namespace、macro、include、diagnostic、CFG、symbol reference を AST/semantic API で扱う。
2. **静的解析機を実装する。** AST matcher check、preprocessor check、dataflow analysis、clang-tidy compatible check、必要に応じて CSA adapter を使う。
3. **動的解析を実行・統合する。** Sanitizer/fuzzer/coverage/CFI を build profile として定義し、実行結果を source location に戻す。
4. **安全なコピーコードを生成する。** public surface extraction、symbol dependency closure、ODR/ABI/macro/include policy、format/reparse verification を備える。
5. **モック/フェイクを生成する。** class/interface/function/free function/template の安全な抽出と、gMock/custom/minimal fake への生成を支援する。
6. **AI コーディングエージェントが使いやすい。** API の意図、入力、出力、失敗理由、編集計画、危険箇所を機械可読にする。

### 4.2 ハード要件

| ID | 要件 | 内容 |
|---|---|---|
| R-HARD-001 | 未対応機能で詰まらない | 高水準 API が未対応でも、Clang/LLVM の raw API に型付きで降りられる。 |
| R-HARD-002 | 正規表現非依存 | C++ 構文/意味解析の標準経路で正規表現を使わない。テキスト検索は補助用途に限定。 |
| R-HARD-003 | compile command 忠実性 | TU 解析は compile_commands.json または同等情報を使う。fallback は明示 opt-in。 |
| R-HARD-004 | source location 正確性 | spelling/expansion/presumed/file location を区別する。 |
| R-HARD-005 | macro 安全性 | 既定ではマクロ展開内を編集しない。編集する場合は明示 policy が必要。 |
| R-HARD-006 | atomic 変更 | 自動修正は全適用/非適用の編集計画として扱い、衝突検出を必須にする。 |
| R-HARD-007 | 再検証 | 変換後は再パースし、diagnostic regression と構文破壊を検出する。 |
| R-HARD-008 | C++23 実装 | ライブラリ実装は C++23。解析対象の `-std` は compile command に従う。 |
| R-HARD-009 | LLVM version 明示 | 対応 LLVM major version を runtime/build metadata に含める。 |
| R-HARD-010 | diagnostic 機械可読性 | 全診断は ID、severity、category、location、message、explanation、fixes を持つ。 |

### 4.3 優先度別要件

#### P0: MVP で必須

- Project/session/TU 抽象。
- compile_commands.json 読み込み、command 正規化、resource-dir/toolchain 検査。
- AST query DSL と raw ASTMatchers bridge。
- Check framework と diagnostics/fix-it。
- Source edit plan、format、dry-run、apply、reparse verification。
- Single-file fixture testing API。
- CLI: `query`、`check`、`rewrite`。
- JSON/SARIF/Markdown 出力。
- raw escape hatch。

#### P1: 早期に必要

- Workspace index / references / call graph / include graph。
- clang-tidy adapter。
- Dataflow framework。
- Include Cleaner adapter。
- Mock/fake generator。
- Sanitizer/fuzzer/coverage build runner。
- Diff-based incremental analysis。
- Parallel TU scheduler。

#### P2: 拡張

- Clang Static Analyzer checker authoring adapter。
- Clang plugin adapter。
- LLVM IR pass adapter。
- ORC JIT utilities。
- LSP/JSON-RPC service。
- Declarative rule language と TableGen/codegen。
- Cross-TU semantic copy with ASTImporter-backed proof mode。

### 4.4 非目標

- Clang/LLVM の再実装。
- C++ 標準の独自 parser 実装。
- すべての raw LLVM API を同じ高水準抽象で完全再表現すること。
- ビルドシステムそのものの再実装。
- sanitizer/fuzzer runtime の再実装。

ただし、非目標は「使えない」という意味ではない。raw escape hatch と adapter により、必要な低レベル機能へ到達可能にする。

---

## 5. 全体アーキテクチャ

```text
+--------------------------------------------------------------------------------+
|                                  cxxlens CLI / SDK                              |
| query | check | rewrite | mock | copy | index | dynamic | ir | test | explain   |
+-------------------------------------+------------------------------------------+
                                      |
+-------------------------------------v------------------------------------------+
|                               Integration Layer                                |
| SARIF/JSON/Markdown | clang-tidy adapter | plugin adapter | LSP/JSON-RPC       |
+-------------------------------------+------------------------------------------+
                                      |
+--------------------+----------------+----------------+-----------------------+
| Query/Check        | Transform       | Index/Include   | Dynamic/IR             |
| AST DSL            | Edit Plan       | Workspace facts | Sanitizer/Fuzzer/Pass  |
| Dataflow           | Mock/Fake       | clangd-derived  | Coverage/JIT           |
+--------------------+----------------+----------------+-----------------------+
                                      |
+-------------------------------------v------------------------------------------+
|                           Semantic Frontend Layer                              |
| translation_unit | ast_node/entity/type | source locations | diagnostics | CFG   |
+-------------------------------------+------------------------------------------+
                                      |
+-------------------------------------v------------------------------------------+
|                              Project/Session Layer                             |
| compile database | toolchain | VFS | preamble cache | scheduler | options       |
+-------------------------------------+------------------------------------------+
                                      |
+-------------------------------------v------------------------------------------+
|                                 LLVM/Clang Layer                               |
| LibTooling | ASTMatchers | Transformer | clang-tidy | clangd | CSA | LLVM IR    |
| LibFormat  | Diagnostics | SourceManager | Preprocessor | Sanitizers | ORC      |
+--------------------------------------------------------------------------------+
```

### 5.1 レイヤリング原則

- **上位レイヤは下位レイヤの寿命を壊さない。** `ast_node` は `translation_unit` より長生きしない。
- **上位レイヤは raw Clang オブジェクトの存在を否定しない。** すべての主要型に `raw()` または `with_clang()` を設ける。
- **副作用は計画化する。** parse/check/query は読み取り、rewrite/apply/build/run は明示的に計画と実行を分ける。
- **安全でない操作は名前で分かる。** `unsafe_raw()`、`allow_macro_edit()`、`allow_fallback_compile_command()` のように危険性を API 名に出す。
- **一貫した data model を使う。** diagnostics、source ranges、symbols、edits、reports は全 module で同じ型を使う。

---

## 6. ライブラリ構成

### 6.1 パッケージ一覧

| パッケージ | 役割 | 主にラップする LLVM/Clang 要素 |
|---|---|---|
| `cxxlens-core` | 共通型、result/error、diagnostics、schema、logging。 | LLVM Support、Error/Expected、Diagnostics。 |
| `cxxlens-source` | file id、source location/range、macro/source text、VFS。 | SourceManager、FileManager、Lexer、VirtualFileSystem。 |
| `cxxlens-project` | compile DB、toolchain、command normalization、project model。 | CompilationDatabase、CommonOptionsParser、clangd compile command ideas。 |
| `cxxlens-session` | parse session、TU lifecycle、scheduler、cache。 | ClangTool、FrontendAction、CompilerInstance、clangd threading/preamble concepts。 |
| `cxxlens-ast` | AST wrapper、entity/type model、visitor。 | ASTContext、Decl、Stmt、Expr、Type、RecursiveASTVisitor。 |
| `cxxlens-query` | 高水準 AST query DSL、raw matcher bridge、explain。 | LibASTMatchers、clang-query concepts。 |
| `cxxlens-analysis` | check framework、diagnostics、fix-it、rule registry。 | clang-tidy check model、DiagnosticsEngine。 |
| `cxxlens-dataflow` | lattice/transfer/CFG fact propagation。 | Clang dataflow、CFG。 |
| `cxxlens-csa` | Static Analyzer 実行/レポート/将来の checker adapter。 | Clang Static Analyzer。 |
| `cxxlens-transform` | rewrite rule、edit plan、transaction、verification。 | Transformer、Refactoring Engine、Replacements、AtomicChange。 |
| `cxxlens-format` | formatting/include formatting style。 | LibFormat。 |
| `cxxlens-index` | symbol/reference/relation index。 | clangd SymbolIndex/BackgroundIndex concepts。 |
| `cxxlens-include` | include graph、unused/missing include、IWYU pragma。 | Include Cleaner、Preprocessor callbacks。 |
| `cxxlens-mock` | semantic copy、mock/fake generation。 | AST、ASTImporter、Rewriter、LibFormat。 |
| `cxxlens-dynamic` | sanitizer/fuzzer/coverage 実行計画と report parser。 | Sanitizers、SanitizerCoverage、libFuzzer、source-based coverage、CFI。 |
| `cxxlens-ir` | LLVM IR module/pass/JIT adapter。 | LLVM IR、New Pass Manager、ORC JIT。 |
| `cxxlens-testing` | snippet/fixture/golden/reparse/property test。 | runToolOnCode、FileCheck 的運用、LibTooling。 |
| `cxxlens-cli` | CLI。 | 全 module。 |
| `cxxlens-codegen` | schema/API/check skeleton 生成。 | TableGen または独自 YAML/JSON schema。 |

### 6.2 依存関係

```text
cxxlens-core
  ├─ cxxlens-source
  │    └─ cxxlens-project
  │          └─ cxxlens-session
  │                ├─ cxxlens-ast
  │                │    ├─ cxxlens-query
  │                │    ├─ cxxlens-analysis
  │                │    │    ├─ cxxlens-dataflow
  │                │    │    └─ cxxlens-csa
  │                │    ├─ cxxlens-transform
  │                │    │    ├─ cxxlens-format
  │                │    │    └─ cxxlens-mock
  │                │    ├─ cxxlens-index
  │                │    └─ cxxlens-include
  │                ├─ cxxlens-dynamic
  │                └─ cxxlens-ir
  └─ cxxlens-testing
```

依存方向は基本的に下から上へ一方向にする。`dynamic` は AST を使って harness を生成する場合のみ `ast/query/transform` に依存し、sanitizer 実行だけなら `project` までで完結できる。

---

## 7. 主要設計ポリシー

### 7.1 Raw Corridor: 未対応機能で詰まらないための脱出口

`cxxlens` の最重要仕様は **Raw Corridor** である。

| 高水準型 | raw access | 目的 |
|---|---|---|
| `translation_unit` | `raw_ast_context()`、`raw_preprocessor()`、`raw_source_manager()`、`raw_compiler_instance()` | Clang C++ API を直接使う。 |
| `ast_node` / `decl` / `expr` | `raw<T>()` | 特殊な AST node API にアクセス。 |
| `query` | `q::from_raw(matcher)`、`to_raw_matcher()` | ASTMatchers 既存資産を使う。 |
| `rewrite_rule` | `from_clang_transformer_rule()` | Transformer 既存 rule を使う。 |
| `check` | `clang_tidy_adapter` / `with_raw_match_finder()` | clang-tidy check を再利用。 |
| `edit_plan` | `to_replacements()`、`to_atomic_changes()` | Clang tooling に戻す。 |
| `ir_module` | `raw_llvm_module()` | LLVM pass/JIT を直接使う。 |
| `dynamic_profile` | `extra_compile_args()`、`extra_link_args()` | sanitizer 未対応 flag を通す。 |

Raw Corridor は単なる `void*` ではない。lifetime、threading、mutation safety を記録する `raw_access_token` を通す。

```cpp
session.with_translation_unit(file, [](cxxlens::translation_unit& tu) {
  tu.with_clang([](clang::ASTContext& ast,
                   clang::Preprocessor& pp,
                   clang::SourceManager& sm) {
    // ここでは通常の Clang API を直接使える。
  });
});
```

### 7.2 Traversal policy

既定値は `traversal_policy::spelled_only`。

```cpp
enum class traversal_policy {
  spelled_only,       // TK_IgnoreUnlessSpelledInSource 相当。既定。
  as_is,              // Clang AST の既定 traversal。
  include_implicit,   // 暗黙ノードを明示的に含める。
  template_patterns,  // template pattern を重視。
  instantiations,     // instantiation を重視。
  custom
};
```

理由は、初心者や AI エージェントが暗黙ノードを対象にして誤診断・誤変換する事故を減らすためである。raw Clang と同じ挙動が必要な場合は `as_is` を明示する。

### 7.3 Macro policy

```cpp
enum class macro_policy {
  reject_macro_expansions,     // 既定。マクロ展開内の編集を拒否。
  allow_spelling_location,     // spelling location 側を編集。
  allow_expansion_location,    // expansion location 側を編集。
  allow_macro_definition_edit, // マクロ定義を編集。危険。
  custom
};
```

クエリはマクロ由来ノードを見つけられる。しかし編集は既定拒否とする。診断のみは可能。

### 7.4 Source range model

Clang の `SourceLocation` は強力だが、初心者にとって spelling/expansion/presumed の差が難しい。`cxxlens` は range を明示型にする。

```cpp
struct source_range {
  source_location begin;
  source_location end;
  range_kind kind;          // spelled / expansion / presumed / token / char
  macro_origin macro;
};
```

### 7.5 編集は常に edit_plan

高水準 API から直接ファイルを書き換えない。

```cpp
auto plan = rule.apply(session);
plan.validate();
plan.format(format_style::project_default());
plan.verify_reparse();
plan.apply(apply_mode::write_files);
```

### 7.6 解析対象 C++ バージョン

`cxxlens` 実装は C++23 とする。解析対象の C++ 標準は固定せず、compile command の `-std=` に従う。Clang 自体は C++98/03/11/14/17/20/23/26/29 など複数標準モードを持つため、`cxxlens` は標準バージョンを独自に解釈せず Clang に委譲する。[^clang-cxx-standards]

### 7.7 バージョン戦略

Clang C++ API は libclang ほど安定ではない。よって `cxxlens` は次を仕様化する。

- `cxxlens` の各 release は LLVM major version を明示的に target する。
- `cxxlens::llvm_version()` と `cxxlens::feature_set()` を提供する。
- raw corridor は version-dependent API であることを型と名前で示す。
- 内部 adapter は `cxxlens-llvm-shim-XX` として分離する。
- CI は複数 LLVM major で source compatibility test を実行する。
- 高水準 public API は semantic versioning で安定化する。

---

## 8. 代表ユースケース設計

### 8.1 正規表現なしで「特定 API 呼び出し」を検出する

```cpp
using namespace cxxlens;

int main(int argc, char** argv) {
  auto project = project::open(project_config::from_args(argc, argv)).value();
  auto session = analysis_session::create(project).value();

  auto malloc_call = q::call_expr()
    .callee(q::function().qualified_name("::malloc"))
    .bind("call");

  auto report = session.find(malloc_call)
    .diagnose([](match_context& m) {
      auto call = m.node<expr>("call");
      return diagnostic::warning("cpp.no-malloc", call.range(),
        "C++ code should not call malloc directly")
        .note("Use RAII or an allocator abstraction instead.");
    });

  return report.print_and_exit();
}
```

### 8.2 自動修正付き check

```cpp
class use_nullptr final : public cxxlens::check {
public:
  void register_queries(query_registry& q) override {
    q.add("zero-null",
      q::integer_literal(0)
        .converted_to_pointer()
        .spelled()
        .bind("zero"));
  }

  void on_match(match_context& m) override {
    auto zero = m.node<expr>("zero");
    m.emit(diagnostic::warning("modernize.use-nullptr", zero.range(),
      "use nullptr instead of 0")
      .fix(edit::replace(zero.spelled_range(), "nullptr")));
  }
};
```

### 8.3 安全なモック生成

```cpp
auto iface = session.find_one(
  q::record_decl().qualified_name("::net::Client").is_abstract().bind("iface"));

auto plan = mock_generator::for_record(iface.node<record_decl>("iface"))
  .framework(mock_framework::gmock)
  .include_strategy(include_strategy::minimal_public)
  .namespace_strategy(namespace_strategy::mirror_original)
  .method_policy(method_policy::virtual_public_only)
  .generate();

plan.verify_reparse();
plan.write_to("tests/mocks/mock_net_client.hpp");
```

### 8.4 Dataflow check

```cpp
struct nullness_lattice {
  enum class state { unknown, null, nonnull, maybe_null };
  state value = state::unknown;

  friend bool operator==(nullness_lattice, nullness_lattice) = default;
  static nullness_lattice join(nullness_lattice a, nullness_lattice b);
};

class null_deref_analysis final
  : public dataflow_analysis<nullness_lattice> {
public:
  nullness_lattice initial(function_decl fn) override;
  nullness_lattice transfer(program_point p,
                            const nullness_lattice& in) override;
  void diagnose(program_point p,
                const nullness_lattice& state,
                diagnostic_sink& out) override;
};
```

### 8.5 sanitizer/fuzzer 実行

```cpp
auto profile = sanitizer_profile::address()
  .with_undefined_behavior()
  .with_coverage(coverage_mode::source_based);

auto build = dynamic_runner::for_project(project)
  .profile(profile)
  .target("unit_tests")
  .configure();

auto result = build.run_tests(run_options::with_timeout(std::chrono::minutes(10)));
result.export_sarif("asan.sarif");
```

---

## 9. 公開 API 設計

以降は実装を除いた public API の設計である。C++23 header sketch として記述する。

### 9.1 命名規則

- namespace は `cxxlens`。
- 型は `snake_case`。
- 関数は `snake_case`。
- builder は fluent API を採用するが、副作用を起こさない。
- 危険 API は `unsafe_` prefix または explicit policy を必要とする。
- `find_*` は query 実行、`make_*` は純粋生成、`run_*` は外部実行を含む。
- `apply()` はファイル変更を伴う可能性があるため、`apply_mode` を必須引数にする。

### 9.2 `cxxlens-core`

#### 主な型

```cpp
namespace cxxlens {

using string = std::string;
using path = std::filesystem::path;

enum class severity {
  ignored,
  note,
  remark,
  warning,
  error,
  fatal
};

enum class confidence {
  low,
  medium,
  high,
  certain
};

struct rule_id {
  std::string value;
};

struct category {
  std::string value;
};

class error {
public:
  std::string message() const;
  std::string code() const;
  std::vector<error> causes() const;
  std::string explain() const;
};

template <class T>
class result {
public:
  bool has_value() const noexcept;
  explicit operator bool() const noexcept;
  T& value() &;
  const T& value() const&;
  T&& value() &&;
  error error() const;
};

class schema {
public:
  std::string name() const;
  std::string json_schema() const;
  std::string markdown() const;
};

class explainable {
public:
  virtual std::string explain() const = 0;
  virtual schema output_schema() const = 0;
  virtual ~explainable() = default;
};

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `result<T>` | 例外に依存しない結果型。 |
| `error` | machine-readable かつ人間可読な失敗情報。 |
| `severity` | 診断 severity。 |
| `confidence` | 静的解析結果の確信度。 |
| `schema` | AI/CLI/UI 向け入出力 schema。 |
| `explainable` | `explain()` を持つ型の共通概念。 |
| `llvm_version()` | build-time LLVM version。 |
| `feature_set()` | 利用可能機能一覧。 |

```cpp
namespace cxxlens {
std::string llvm_version();
std::vector<std::string> feature_set();
schema public_api_schema();
} // namespace cxxlens
```

### 9.3 `cxxlens-source`

```cpp
namespace cxxlens {

struct file_id {
  uint64_t value;
  friend bool operator==(file_id, file_id) = default;
};

enum class location_kind {
  file,
  spelling,
  expansion,
  presumed,
  macro_definition,
  invalid
};

struct source_location {
  file_id file;
  uint32_t line = 0;
  uint32_t column = 0;
  uint32_t offset = 0;
  location_kind kind = location_kind::invalid;
  bool is_valid() const noexcept;
};

enum class range_kind {
  token_range,
  char_range,
  spelled_range,
  expansion_range,
  semantic_range
};

struct macro_origin {
  bool from_macro = false;
  std::string macro_name;
  source_location spelling_location;
  source_location expansion_location;
};

struct source_range {
  source_location begin;
  source_location end;
  range_kind kind = range_kind::char_range;
  macro_origin macro;
  bool is_valid() const noexcept;
};

class source_file {
public:
  file_id id() const;
  path absolute_path() const;
  std::string_view content() const;
  std::string line(uint32_t line_number) const;
};

class source_manager_view {
public:
  result<source_file> file(file_id) const;
  result<source_location> spelling_location(source_location) const;
  result<source_location> expansion_location(source_location) const;
  result<std::string> text(source_range) const;
  bool is_inside_macro(source_range) const;

  clang::SourceManager& raw();
  const clang::SourceManager& raw() const;
};

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `source_location` | file/spelling/expansion/presumed を区別した位置。 |
| `source_range` | token/char/spelled/semantic range を区別。 |
| `macro_origin` | macro 由来情報。 |
| `source_manager_view::text()` | 安全に source text を取得。 |
| `source_manager_view::raw()` | Clang `SourceManager` への脱出口。 |

### 9.4 `cxxlens-project`

```cpp
namespace cxxlens {

enum class compile_command_policy {
  require_exact,
  allow_header_inference,
  allow_fallback_flags
};

struct language_standard {
  std::string spelling; // c++17, gnu++20, c++2b など。Clang へ委譲。
};

class compile_command {
public:
  path file() const;
  path directory() const;
  std::vector<std::string> arguments() const;
  std::optional<language_standard> standard() const;
  bool has_flag(std::string_view flag) const;
  compile_command with_added_arg(std::string arg) const;
  compile_command with_removed_arg(std::string_view arg) const;
  std::string render_shell_escaped() const;
};

class project_config {
public:
  static result<project_config> from_compilation_database(path build_dir);
  static result<project_config> from_args(int argc, char** argv);
  static project_config from_single_file(path file, std::vector<std::string> args);

  project_config& compile_command_policy(compile_command_policy);
  project_config& resource_dir(path);
  project_config& query_driver_allowlist(std::vector<path>);
  project_config& extra_arg(std::string);
  project_config& remove_arg(std::string);
  project_config& vfs_overlay(path yaml_file);
  project_config& working_directory(path);
};

class project {
public:
  static result<project> open(project_config);

  std::vector<path> translation_units() const;
  result<compile_command> command_for(path file) const;
  result<compile_command> normalized_command_for(path file) const;
  path root() const;
  schema schema() const;

  clang::tooling::CompilationDatabase& raw_compilation_database();
  const clang::tooling::CompilationDatabase& raw_compilation_database() const;
};

class toolchain {
public:
  static result<toolchain> detect();
  static result<toolchain> from_clang(path clang_executable);

  path clang() const;
  path clangxx() const;
  path resource_dir() const;
  std::string target_triple() const;
  std::string version() const;
};

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `project_config::from_compilation_database()` | compile_commands.json から project を開く。 |
| `project_config::from_single_file()` | snippet/単一ファイル解析。 |
| `compile_command_policy` | fallback parse の危険性を明示。 |
| `compile_command` | command の正規化・編集。 |
| `project::translation_units()` | 解析対象 TU 一覧。 |
| `toolchain` | Clang/resource-dir/target 情報。 |

### 9.5 `cxxlens-session`

```cpp
namespace cxxlens {

struct session_options {
  size_t parallelism = std::thread::hardware_concurrency();
  bool enable_preamble_cache = true;
  bool enable_background_index = false;
  size_t memory_budget_mb = 4096;
  compile_command_policy command_policy = compile_command_policy::require_exact;
};

class diagnostic_set;
class translation_unit;

class analysis_session {
public:
  static result<analysis_session> create(project, session_options = {});

  result<translation_unit> parse(path file);
  result<std::vector<translation_unit>> parse_all();
  result<std::vector<translation_unit>> parse_changed_files(std::vector<path> files);

  project& owning_project();
  const project& owning_project() const;

  source_manager_view source_manager_for(path file);
  diagnostic_set diagnostics() const;

  template <class Fn>
  auto with_translation_unit(path file, Fn&& fn) -> result<std::invoke_result_t<Fn, translation_unit&>>;
};

class translation_unit {
public:
  path main_file() const;
  source_manager_view sources();
  diagnostic_set diagnostics() const;

  class ast_view ast();
  class preprocessor_view preprocessor();
  class cfg_view cfg();

  clang::ASTContext& raw_ast_context();
  clang::Preprocessor& raw_preprocessor();
  clang::SourceManager& raw_source_manager();
  clang::CompilerInstance& raw_compiler_instance();

  template <class Fn>
  auto with_clang(Fn&& fn) -> result<std::invoke_result_t<Fn,
    clang::ASTContext&, clang::Preprocessor&, clang::SourceManager&>>;
};

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `analysis_session::parse()` | 1 TU の解析。 |
| `analysis_session::parse_all()` | 全 TU の解析。 |
| `analysis_session::parse_changed_files()` | 差分解析。 |
| `translation_unit::ast()` | AST view。 |
| `translation_unit::preprocessor()` | macro/include 解析。 |
| `translation_unit::cfg()` | CFG view。 |
| `translation_unit::with_clang()` | Clang raw API 脱出口。 |

### 9.6 `cxxlens-ast`

```cpp
namespace cxxlens {

enum class ast_kind {
  declaration,
  statement,
  expression,
  type,
  template_argument,
  attribute,
  unknown
};

class ast_node {
public:
  ast_kind kind() const;
  std::string clang_kind_name() const;
  source_range spelled_range() const;
  source_range expansion_range() const;
  source_range semantic_range() const;
  bool is_implicit() const;
  bool is_from_main_file() const;
  bool is_from_system_header() const;
  macro_origin macro() const;

  template <class T>
  std::optional<T> as() const;

  template <class Raw>
  Raw& raw();
};

class decl : public ast_node {
public:
  std::string name() const;
  std::string qualified_name() const;
  bool is_definition() const;
  std::optional<decl> canonical_decl() const;
};

class function_decl : public decl {
public:
  type return_type() const;
  std::vector<class param_decl> parameters() const;
  bool is_constexpr() const;
  bool is_noexcept() const;
  bool is_virtual() const;
  bool is_override() const;
  bool is_deleted() const;
  bool is_defaulted() const;
  std::optional<class stmt> body() const;
};

class record_decl : public decl {
public:
  bool is_class() const;
  bool is_struct() const;
  bool is_union() const;
  bool is_abstract() const;
  std::vector<record_decl> bases() const;
  std::vector<function_decl> methods() const;
  std::vector<decl> fields() const;
};

class stmt : public ast_node {};
class expr : public stmt {
public:
  type expression_type() const;
  bool is_value_dependent() const;
};

class type {
public:
  std::string spelling() const;
  std::string canonical_spelling() const;
  bool is_pointer() const;
  bool is_reference() const;
  bool is_const_qualified() const;
  bool is_record_type() const;
  clang::QualType raw() const;
};

class ast_view {
public:
  std::vector<decl> top_level_decls() const;
  result<decl> find_decl_by_qualified_name(std::string_view name) const;

  template <class Visitor>
  void visit(Visitor&& visitor, traversal_policy = traversal_policy::spelled_only);

  clang::ASTContext& raw_context();
};

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `ast_node` | 全 AST wrapper の基底。 |
| `decl` / `function_decl` / `record_decl` | C++ 宣言を意味的に扱う。 |
| `expr` / `stmt` | 式・文を扱う。 |
| `type` | `QualType` の安全 wrapper。 |
| `ast_view::visit()` | RAV を安全に使う高水準 traversal。 |
| `raw<T>()` | 任意 Clang AST node へのアクセス。 |

### 9.7 `cxxlens-query`

```cpp
namespace cxxlens {

enum class traversal_policy {
  spelled_only,
  as_is,
  include_implicit,
  template_patterns,
  instantiations,
  custom
};

class query;
class match;
class match_set;

enum class pattern_syntax {
  exact,
  glob
};

namespace q {

query anything();
query decl();
query named_decl();
query function();
query method();
query constructor();
query destructor();
query record_decl();
query enum_decl();
query field_decl();
query var_decl();
query param_decl();
query typedef_decl();
query namespace_decl();

query stmt();
query expr();
query call_expr();
query member_call_expr();
query constructor_call_expr();
query operator_call_expr();
query return_stmt();
query if_stmt();
query for_stmt();
query while_stmt();
query switch_stmt();
query lambda_expr();
query integer_literal(std::optional<int64_t> value = std::nullopt);
query string_literal();
query null_pointer_literal();

query type();
query pointer_type();
query reference_type();
query record_type();
query template_specialization_type();

query macro_expansion();
query include_directive();

query from_raw(clang::ast_matchers::DeclarationMatcher);
query from_raw(clang::ast_matchers::StatementMatcher);
query from_raw(clang::ast_matchers::TypeMatcher);

} // namespace q

class query {
public:
  query name(std::string_view exact) const;
  query qualified_name(std::string_view exact) const;
  query name_pattern(std::string_view pattern, pattern_syntax syntax = pattern_syntax::glob) const;
  query in_namespace(std::string_view ns) const;
  query from_main_file() const;
  query from_system_header(bool value = true) const;
  query is_definition(bool value = true) const;
  query is_implicit(bool value = true) const;
  query spelled() const;
  query expanded() const;
  query bind(std::string_view id) const;

  query has(query child) const;
  query has_descendant(query child) const;
  query unless(query negated) const;
  query any_of(std::span<const query> alternatives) const;
  query all_of(std::span<const query> requirements) const;

  query traversal(traversal_policy) const;
  std::string explain() const;
  schema schema() const;
};

class match {
public:
  bool contains(std::string_view binding) const;

  template <class T>
  T node(std::string_view binding) const;

  source_range range(std::string_view binding) const;
  std::string explain() const;
};

class match_set {
public:
  size_t size() const;
  bool empty() const;
  std::vector<match> all() const;

  template <class Fn>
  void for_each(Fn&& fn) const;

  std::string to_json() const;
  std::string to_markdown() const;
};

class query_engine {
public:
  static result<match_set> find(translation_unit&, query);
  static result<match_set> find(analysis_session&, query);
  static result<match> find_one(translation_unit&, query);

  static std::string explain_no_match(translation_unit&, query);
};

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `q::function()` 等 | 意図ベースの query builder。 |
| `query::bind()` | match binding。 |
| `query::traversal()` | traversal mode 明示。 |
| `q::from_raw()` | ASTMatchers 直接利用。 |
| `query_engine::find()` | TU/session から検索。 |
| `query_engine::explain_no_match()` | AI/人間向けの失敗説明。 |
| `match::node<T>()` | binding を型付き取得。 |

### 9.8 `cxxlens-analysis`

```cpp
namespace cxxlens {

class diagnostic {
public:
  static diagnostic note(rule_id id, source_range, std::string message);
  static diagnostic warning(rule_id id, source_range, std::string message);
  static diagnostic error(rule_id id, source_range, std::string message);

  diagnostic& category(category);
  diagnostic& confidence(confidence);
  diagnostic& note(std::string message, std::optional<source_range> = std::nullopt);
  diagnostic& fix(class edit);
  diagnostic& fixes(std::vector<class edit>);
  diagnostic& explanation(std::string markdown);
  diagnostic& tag(std::string key, std::string value);

  rule_id id() const;
  severity severity() const;
  source_range primary_range() const;
  std::string message() const;
  std::string to_json() const;
};

class diagnostic_sink {
public:
  void emit(diagnostic);
  std::vector<diagnostic> diagnostics() const;
};

class query_registry {
public:
  void add(std::string name, query);
  bool contains(std::string_view name) const;
};

class match_context {
public:
  match current_match() const;
  translation_unit& tu();
  source_manager_view sources();
  void emit(diagnostic);

  template <class T>
  T node(std::string_view binding) const;
};

class pp_context {
public:
  translation_unit& tu();
  source_manager_view sources();
  void emit(diagnostic);
};

class tu_context {
public:
  translation_unit& tu();
  diagnostic_sink& diagnostics();
};

class option_schema_builder;

class check {
public:
  virtual std::string name() const = 0;
  virtual std::string summary() const = 0;
  virtual void options(option_schema_builder&) {}
  virtual void register_queries(query_registry&) {}
  virtual void on_match(match_context&) {}
  virtual void on_preprocessor(pp_context&) {}
  virtual void on_translation_unit(tu_context&) {}
  virtual ~check() = default;
};

class check_registry {
public:
  void add(std::unique_ptr<check>);
  std::vector<std::string> names() const;
  check* find(std::string_view name) const;
};

struct check_runner_options {
  std::vector<std::string> enabled_checks;
  bool apply_fixes = false;
  bool warnings_as_errors = false;
  bool respect_nolint = true;
  std::optional<path> export_sarif;
};

class check_report {
public:
  std::vector<diagnostic> diagnostics() const;
  edit_plan fixes() const;
  int exit_code() const;
  std::string to_json() const;
  std::string to_sarif() const;
  std::string to_markdown() const;
};

class check_runner {
public:
  check_runner& add_check(std::unique_ptr<check>);
  check_runner& add_registry(const check_registry&);
  result<check_report> run(analysis_session&, check_runner_options = {});
};

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `check` | 独自静的解析 check の基底。 |
| `register_queries()` | AST query 登録。 |
| `on_match()` | match ごとの診断/修正。 |
| `on_preprocessor()` | macro/include 等の PP check。 |
| `diagnostic` | 位置・説明・fix を持つ診断。 |
| `check_runner` | session 上で check 群を実行。 |
| `check_report` | JSON/SARIF/Markdown/exit code 出力。 |

### 9.9 `cxxlens-dataflow`

```cpp
namespace cxxlens {

class cfg_block;
class program_point;
class cfg_view {
public:
  result<std::vector<cfg_block>> blocks(function_decl) const;
  std::string to_dot(function_decl) const;
};

template <class Lattice>
concept lattice = requires(Lattice a, Lattice b) {
  { Lattice::join(a, b) } -> std::same_as<Lattice>;
  { a == b } -> std::convertible_to<bool>;
};

template <lattice Lattice>
class dataflow_analysis {
public:
  virtual Lattice initial(function_decl) = 0;
  virtual Lattice transfer(program_point, const Lattice&) = 0;
  virtual void diagnose(program_point,
                        const Lattice&,
                        diagnostic_sink&) {}
  virtual ~dataflow_analysis() = default;
};

template <lattice Lattice>
class dataflow_result {
public:
  Lattice state_before(program_point) const;
  Lattice state_after(program_point) const;
  std::vector<diagnostic> diagnostics() const;
  std::string trace(program_point) const;
};

struct dataflow_options {
  uint32_t max_iterations = 10000;
  bool include_exception_edges = false;
  bool interprocedural = false;
};

template <lattice Lattice>
result<dataflow_result<Lattice>> run_dataflow(
  function_decl,
  dataflow_analysis<Lattice>&,
  dataflow_options = {});

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `lattice` concept | dataflow lattice の型制約。 |
| `dataflow_analysis<Lattice>` | transfer/join/diagnose を実装する基底。 |
| `run_dataflow()` | function 単位 dataflow 実行。 |
| `dataflow_result::trace()` | なぜその状態になったか説明。 |

### 9.10 `cxxlens-transform`

```cpp
namespace cxxlens {

enum class edit_kind {
  insert,
  replace,
  remove,
  add_include,
  remove_include
};

enum class macro_policy {
  reject_macro_expansions,
  allow_spelling_location,
  allow_expansion_location,
  allow_macro_definition_edit,
  custom
};

class edit {
public:
  static edit insert(source_location, std::string text);
  static edit replace(source_range, std::string text);
  static edit remove(source_range);
  static edit add_include(path file, std::string header);
  static edit remove_include(path file, std::string header);

  edit_kind kind() const;
  source_range range() const;
  std::string text() const;
  edit& macro_policy(macro_policy);
  std::string explain() const;
};

class atomic_edit {
public:
  atomic_edit(std::string key);
  atomic_edit& add(edit);
  atomic_edit& metadata(std::string key, std::string value);
  std::vector<edit> edits() const;
};

class edit_plan {
public:
  edit_plan& add(atomic_edit);
  edit_plan& add(edit);
  edit_plan& format(class format_style);
  edit_plan& include_cleanup(bool enabled = true);
  edit_plan& verify_reparse(bool enabled = true);

  result<void> validate() const;
  result<std::string> preview_unified_diff() const;
  result<void> apply(enum class apply_mode mode) const;

  std::vector<atomic_edit> atomic_edits() const;
  std::string to_json() const;
  std::string to_yaml_atomic_changes() const;
  clang::tooling::Replacements to_replacements() const;
};

class rewrite_context {
public:
  match current_match() const;
  translation_unit& tu();
  source_manager_view sources();
};

using edit_generator = std::function<result<edit_plan>(rewrite_context&)>;

class rewrite_rule {
public:
  static rewrite_rule make(std::string name, query selector, edit_generator);
  static rewrite_rule from_raw_transformer_rule(/* clang transformer rule */);

  rewrite_rule& diagnostic(std::function<diagnostic(rewrite_context&)>);
  rewrite_rule& macro_policy(macro_policy);
  rewrite_rule& format(bool enabled = true);
  std::string name() const;
};

class transformer {
public:
  transformer& add_rule(rewrite_rule);
  result<edit_plan> run(analysis_session&);
  result<edit_plan> run(translation_unit&);
};

enum class apply_mode {
  dry_run,
  write_files,
  in_memory
};

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `edit` | 挿入/置換/削除/include 変更。 |
| `atomic_edit` | 意味単位の編集グループ。 |
| `edit_plan` | 複数ファイル編集の検証・整形・適用。 |
| `rewrite_rule` | query + edit generator。 |
| `transformer` | rewrite_rule 群を実行。 |
| `apply_mode` | dry-run/write/in-memory を明示。 |

### 9.11 `cxxlens-format`

```cpp
namespace cxxlens {

enum class builtin_style {
  llvm,
  google,
  chromium,
  mozilla,
  webkit,
  gnu,
  microsoft,
  file
};

class format_style {
public:
  static result<format_style> from_file(path source_or_directory);
  static format_style builtin(builtin_style);
  static format_style project_default();

  std::string name() const;
  clang::format::FormatStyle& raw();
  const clang::format::FormatStyle& raw() const;
};

class formatter {
public:
  static result<edit_plan> format_ranges(path file,
                                         std::vector<source_range>,
                                         format_style);
  static result<edit_plan> format_changed_ranges(edit_plan,
                                                 format_style);
};

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `format_style::from_file()` | `.clang-format` 解決。 |
| `formatter::format_changed_ranges()` | 変更箇所のみ整形。 |
| `raw()` | `clang::format::FormatStyle` 直接操作。 |

### 9.12 `cxxlens-index`

```cpp
namespace cxxlens {

struct symbol_id {
  std::string opaque;
  friend bool operator==(symbol_id, symbol_id) = default;
};

class symbol {
public:
  symbol_id id() const;
  std::string name() const;
  std::string qualified_name() const;
  source_range declaration_range() const;
  std::optional<source_range> definition_range() const;
  ast_kind kind() const;
};

class reference {
public:
  symbol_id target() const;
  source_range range() const;
  bool is_write() const;
  bool is_call() const;
};

class workspace_index {
public:
  static result<workspace_index> build(analysis_session&);
  static result<workspace_index> load(path cache_dir);

  result<void> save(path cache_dir) const;
  std::vector<symbol> symbols(std::string_view query) const;
  std::vector<reference> references(symbol_id) const;
  std::vector<symbol> definitions(std::string_view qualified_name) const;
  std::vector<symbol> callers(symbol_id function) const;
  std::vector<symbol> callees(symbol_id function) const;

  std::string to_json() const;
};

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `workspace_index::build()` | 全コードベース fact 抽出。 |
| `symbols()` | symbol 検索。 |
| `references()` | 参照検索。 |
| `callers()` / `callees()` | call graph 取得。 |
| `save()` / `load()` | cache。 |

### 9.13 `cxxlens-include`

```cpp
namespace cxxlens {

class include_directive {
public:
  std::string header() const;
  path resolved_path() const;
  source_range range() const;
  bool is_angled() const;
  bool is_system() const;
};

class include_graph {
public:
  std::vector<include_directive> includes_of(path file) const;
  std::vector<path> transitive_includes(path file) const;
  std::string to_dot() const;
};

class include_report {
public:
  std::vector<include_directive> unused_includes() const;
  std::vector<std::string> missing_includes() const;
  edit_plan fixes() const;
  std::string to_json() const;
};

class include_analyzer {
public:
  static result<include_report> analyze(translation_unit&);
  static result<include_graph> build_graph(analysis_session&);
};

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `include_directive` | include 文の semantic wrapper。 |
| `include_graph` | include 依存関係。 |
| `include_analyzer::analyze()` | 未使用/不足 include の検出。 |
| `include_report::fixes()` | include 自動修正。 |

### 9.14 `cxxlens-mock`

```cpp
namespace cxxlens {

enum class mock_framework {
  gmock,
  trompeloeil,
  fakeit,
  minimal_custom
};

enum class method_policy {
  virtual_public_only,
  all_public_nonstatic,
  explicitly_selected
};

enum class namespace_strategy {
  mirror_original,
  wrap_in_test_namespace,
  flat
};

enum class include_strategy {
  minimal_public,
  preserve_original,
  forward_declare_where_possible
};

enum class odr_policy {
  reject_odr_sensitive_definitions,
  allow_inline_definitions,
  allow_templates,
  allow_all_with_warning
};

struct mock_generation_options {
  mock_framework framework = mock_framework::gmock;
  method_policy methods = method_policy::virtual_public_only;
  namespace_strategy namespaces = namespace_strategy::mirror_original;
  include_strategy includes = include_strategy::minimal_public;
  bool generate_fake_defaults = false;
  bool preserve_comments = true;
  bool verify_reparse = true;
};

class public_surface {
public:
  record_decl source_record() const;
  std::vector<function_decl> methods() const;
  std::vector<decl> required_types() const;
  std::vector<include_directive> required_includes() const;
  std::string explain() const;
};

class semantic_copy_options {
public:
  semantic_copy_options& include_dependencies(bool);
  semantic_copy_options& copy_comments(bool);
  semantic_copy_options& macro_policy(macro_policy);
  semantic_copy_options& odr_policy(cxxlens::odr_policy);
  semantic_copy_options& namespace_strategy(namespace_strategy);
};

class copy_plan {
public:
  edit_plan edits() const;
  std::vector<symbol> copied_symbols() const;
  std::vector<diagnostic> warnings() const;
  std::string explain() const;
};

class semantic_copier {
public:
  static result<public_surface> extract_public_surface(record_decl);
  static result<copy_plan> copy_symbols(std::vector<symbol_id>,
                                        workspace_index&,
                                        semantic_copy_options);
};

class mock_generator {
public:
  static mock_generator for_record(record_decl);
  mock_generator& framework(mock_framework);
  mock_generator& method_policy(method_policy);
  mock_generator& include_strategy(include_strategy);
  mock_generator& namespace_strategy(namespace_strategy);
  mock_generator& output(path);
  result<edit_plan> generate();
};

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `semantic_copier::extract_public_surface()` | mock/fake に必要な公開面を抽出。 |
| `semantic_copier::copy_symbols()` | 依存関係付き安全コピー。 |
| `mock_generator` | gMock 等の mock/fake 生成。 |
| `mock_generation_options` | 生成 policy。 |
| `copy_plan::explain()` | なぜその symbol/include が必要か説明。 |

### 9.15 `cxxlens-dynamic`

```cpp
namespace cxxlens {

enum class sanitizer_kind {
  address,
  thread,
  memory,
  undefined_behavior,
  leak,
  dataflow,
  type,
  realtime,
  cfi
};

enum class coverage_mode {
  none,
  sanitizer_coverage,
  source_based
};

class sanitizer_profile {
public:
  static sanitizer_profile address();
  static sanitizer_profile thread();
  static sanitizer_profile memory();
  static sanitizer_profile undefined_behavior();
  static sanitizer_profile leak();
  static sanitizer_profile realtime();

  sanitizer_profile& enable(sanitizer_kind);
  sanitizer_profile& with_undefined_behavior();
  sanitizer_profile& with_leak();
  sanitizer_profile& with_coverage(coverage_mode);
  sanitizer_profile& extra_compile_arg(std::string);
  sanitizer_profile& extra_link_arg(std::string);
  sanitizer_profile& env(std::string key, std::string value);

  std::vector<std::string> compile_args() const;
  std::vector<std::string> link_args() const;
};

class dynamic_build_plan {
public:
  result<void> configure() const;
  result<void> build() const;
  std::string compile_commands_preview() const;
};

class dynamic_run_result {
public:
  std::vector<diagnostic> diagnostics() const;
  std::string stdout_text() const;
  std::string stderr_text() const;
  std::string to_sarif() const;
  std::string to_markdown() const;
};

class dynamic_runner {
public:
  static dynamic_runner for_project(project);
  dynamic_runner& profile(sanitizer_profile);
  dynamic_runner& target(std::string build_target);
  dynamic_runner& build_directory(path);
  dynamic_runner& test_command(std::vector<std::string> argv);
  dynamic_runner& timeout(std::chrono::seconds);

  result<dynamic_build_plan> configure();
  result<dynamic_run_result> run_tests();
};

class fuzz_harness_generator {
public:
  static fuzz_harness_generator for_function(function_decl);
  fuzz_harness_generator& max_input_size(size_t);
  fuzz_harness_generator& corpus_dir(path);
  result<edit_plan> generate();
};

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `sanitizer_profile` | sanitizer/coverage/CFI のビルド flag を宣言。 |
| `dynamic_runner` | instrumented build と tests 実行。 |
| `dynamic_run_result` | sanitizer/fuzzer/coverage 結果を診断化。 |
| `fuzz_harness_generator` | libFuzzer harness 生成。 |

### 9.16 `cxxlens-ir`

```cpp
namespace cxxlens {

class ir_module {
public:
  static result<ir_module> parse_bitcode(path);
  static result<ir_module> parse_ir(path);

  result<void> verify() const;
  std::vector<std::string> functions() const;
  llvm::Module& raw();
};

class pass_pipeline {
public:
  static result<pass_pipeline> from_text(std::string pipeline);
  pass_pipeline& add_default_optimization_pipeline(int opt_level);
  pass_pipeline& add_verifier();
  result<void> run(ir_module&);
};

class jit_engine {
public:
  static result<jit_engine> create();
  result<void> add_module(ir_module);
  result<uint64_t> lookup_address(std::string symbol);
};

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `ir_module` | LLVM IR/bitcode wrapper。 |
| `pass_pipeline` | New Pass Manager pipeline 実行。 |
| `jit_engine` | ORC JIT の簡易 adapter。 |
| `raw()` | LLVM `Module` 直操作。 |

### 9.17 `cxxlens-testing`

```cpp
namespace cxxlens {

class code_fixture {
public:
  static code_fixture cpp(std::string code);
  code_fixture& filename(path);
  code_fixture& arg(std::string);
  code_fixture& standard(std::string std_flag);
  code_fixture& virtual_file(path, std::string content);

  result<translation_unit> parse();
  result<check_report> run_check(std::unique_ptr<check>);
  result<edit_plan> run_rewrite(rewrite_rule);
};

class golden_test {
public:
  static golden_test from_fixture(code_fixture);
  golden_test& expect_diagnostic(rule_id, std::string contains);
  golden_test& expect_no_diagnostics();
  golden_test& expect_rewrite(std::string expected_code);
  result<void> run();
};

class property_test {
public:
  property_test& generate_random_cpp_snippets(size_t count);
  property_test& require_reparse_success();
  property_test& require_idempotent_rewrite();
  result<void> run(rewrite_rule);
};

} // namespace cxxlens
```

#### API 一覧

| API | 目的 |
|---|---|
| `code_fixture` | `runToolOnCode` 相当の高水準テスト。 |
| `golden_test` | diagnostics/rewrite の golden test。 |
| `property_test` | 変換の idempotence/reparse property test。 |

---

## 10. CLI 設計

### 10.1 コマンド一覧

```text
cxxlens query    --compile-db build --query file.json --format json
cxxlens check    --compile-db build --checks modernize.*,company.* --sarif report.sarif
cxxlens rewrite  --compile-db build --rule rules/use-nullptr.yml --dry-run
cxxlens rewrite  --compile-db build --rule rules/use-nullptr.yml --apply
cxxlens index    --compile-db build --cache .cxxlens/index
cxxlens include  --compile-db build --fix
cxxlens mock     --compile-db build --symbol ::net::Client --framework gmock --out tests/mock_client.hpp
cxxlens copy     --compile-db build --symbol ::foo::Bar --out sandbox/bar_copy.hpp
cxxlens dynamic  --compile-db build --target tests --sanitize address,undefined --sarif dyn.sarif
cxxlens fuzz     --compile-db build --symbol ::parse_packet --out fuzz/parse_packet_fuzz.cpp
cxxlens ir       --input module.bc --passes default<O2>,verify
cxxlens explain  --diagnostic report.json --id company.rule-123
```

### 10.2 CLI 出力原則

- 既定は人間可読 text。
- `--format json` は stable schema。
- `--sarif` は code scanning 用。
- `--dry-run` は必ず unified diff と summary を出す。
- `--explain` は query/check/fix の判断理由を出す。
- `--fail-on warning,error` のような CI exit policy を提供する。

---

## 11. モック/フェイク/安全コピーの詳細設計

### 11.1 課題

C++ のコードコピーや mock 生成は危険である。

- include が足りない、または過剰。
- dependent type、nested type、using alias、default template argument を見落とす。
- private/protected/public、virtual、override、noexcept、ref-qualifier、cv-qualifier を落とす。
- macro による宣言、platform ifdef、attribute、ABI annotation を壊す。
- inline function / template / explicit specialization / ODR の扱いを誤る。
- source location が generated code や system header を指す。

### 11.2 方針

`cxxlens-mock` は「それっぽい文字列生成」をしない。必ず AST から public surface を抽出する。

#### public surface extraction

対象 `record_decl` から以下を抽出する。

- class/struct 名、namespace、template parameter。
- public virtual methods。
- return type / parameter type / cv/ref/noexcept/override/final。
- default argument の有無。ただし mock 生成では基本的に default argument を再定義しない。
- required include / forward declaration 候補。
- attribute、calling convention、visibility。
- macro origin と conditional compilation 情報。

#### safety gates

- system header 由来の宣言を生成対象にするときは明示 opt-in。
- macro 展開由来の宣言は default で warning。
- unresolved dependent type がある場合は generation を fail または warning にする policy を選択。
- 生成後に仮想ファイルとして再パース。
- `static_assert` や `std::is_base_of_v` などの検証コードを optional 生成。

### 11.3 生成例

入力:

```cpp
namespace net {
class Client {
public:
  virtual ~Client() = default;
  virtual Response send(Request const& request) noexcept = 0;
};
}
```

出力方針:

```cpp
#pragma once

#include <gmock/gmock.h>
#include "net/client.hpp"

namespace net::test {

class MockClient final : public ::net::Client {
public:
  MOCK_METHOD(::net::Response, send,
              (::net::Request const& request),
              (noexcept, override));
};

} // namespace net::test
```

`cxxlens` はこの出力に至る依存 reasoning を `copy_plan::explain()` で出す。

---

## 12. 静的解析設計

### 12.1 三段階の解析モデル

| 段階 | 用途 | API |
|---|---|---|
| Pattern check | AST 形状で検出できる規則。 | `query` + `check::on_match()` |
| Local semantic/dataflow | 関数内 CFG と状態遷移が必要な規則。 | `dataflow_analysis<Lattice>` |
| Path-sensitive/interprocedural | symbolic execution や複数関数経路が必要。 | `cxxlens-csa` adapter / CSA 連携 |

### 12.2 check metadata

すべての check は次を持つ。

```cpp
struct check_metadata {
  rule_id id;
  std::string name;
  std::string summary;
  std::string rationale_markdown;
  std::vector<std::string> categories;
  severity default_severity;
  confidence default_confidence;
  std::vector<std::string> references;
  bool has_auto_fix;
  bool is_semantics_preserving_fix;
};
```

### 12.3 抑制コメント

clang-tidy の `NOLINT` 互換に加え、独自抑制も提供する。

```cpp
// CXXLENS-NOLINT(company.rule-id): reason
// CXXLENS-NOLINTNEXTLINE(company.rule-id): reason
```

抑制は report に残す。AI/監査用に「検出されたが抑制された」情報を JSON に出す。

---

## 13. 動的解析設計

### 13.1 build profile

```cpp
struct dynamic_profile_schema {
  std::vector<sanitizer_kind> sanitizers;
  coverage_mode coverage;
  bool use_lto_for_cfi;
  std::vector<std::string> compile_args;
  std::vector<std::string> link_args;
  std::map<std::string, std::string> environment;
};
```

### 13.2 レポート正規化

sanitizer/fuzzer の stderr はツールごとに形式が異なる。`cxxlens` は parser を持ち、次に正規化する。

```json
{
  "tool": "asan",
  "rule_id": "dynamic.address.use-after-free",
  "severity": "error",
  "message": "heap-use-after-free",
  "primary_location": {
    "file": "src/foo.cpp",
    "line": 42,
    "column": 7
  },
  "stack": [
    { "function": "foo", "file": "src/foo.cpp", "line": 42 },
    { "function": "main", "file": "tests/foo_test.cpp", "line": 10 }
  ]
}
```

### 13.3 fuzz harness 生成

fuzz harness は安全に生成する必要がある。

- 対象関数の parameter type を AST から取得。
- `std::span<const std::byte>` / `std::string_view` / pointer+size などのよくある形を認識。
- 変換不能な型は generator が失敗理由を説明。
- sanitizer profile と coverage を併用する。

---

## 14. AI エージェント向け設計

### 14.1 AI が失敗しやすい点

- `SourceLocation` の spelling/expansion を混同する。
- implicit cast / implicit constructor call を意図せず match する。
- compile command なしで parse し、include/define 不足を誤診断する。
- `Replacements` の衝突を見落とす。
- `ASTContext` より長く AST node pointer を保持する。
- macro 展開内を直接置換して壊す。
- テンプレート pattern と instantiation を混同する。
- system header の診断をユーザーコードの診断として出す。

### 14.2 AI 用 API

```cpp
class agent_introspection {
public:
  static std::string list_modules_json();
  static std::string list_queries_json();
  static std::string list_checks_json();
  static std::string query_schema_json();
  static std::string edit_plan_schema_json();
  static std::string examples_markdown(std::string_view topic);
};
```

### 14.3 explain API

- `query.explain()` は「何を match するか」を自然言語と ASTMatcher 相当で説明。
- `query_engine::explain_no_match()` は「compile command 不足」「traversal が spelled_only」「名前が qualified でない」などを提案。
- `edit_plan::preview_unified_diff()` は適用前 diff。
- `diagnostic::explanation()` は規則の意図と修正方針。
- `copy_plan::explain()` は依存 closure と生成理由。

### 14.4 stable JSON schema

AI には C++ API だけでなく JSON schema を提示できるようにする。

```json
{
  "query": {
    "kind": "call_expr",
    "callee": { "kind": "function", "qualified_name": "::malloc" },
    "bind": "call",
    "traversal": "spelled_only"
  }
}
```

これにより AI は C++ DSL と JSON DSL のどちらでも rule を組み立てられる。

---

## 15. セキュリティ・信頼性ポリシー

- 外部 compiler driver 実行は allowlist が必要。
- plugin/dynamic library load は既定無効。
- sanitizer/fuzzer 実行は timeout、resource limit、working directory isolation を持つ。
- `compile_commands.json` 内の任意コマンド文字列を shell 経由で実行しない。argv として扱う。
- generated code を書き込む前に path traversal を検査する。
- SARIF/JSON には機密パスを相対化する option を提供。
- crash した TU は structured failure として report し、全体処理を継続可能にする。

---

## 16. 性能・スケーラビリティ設計

### 16.1 単一 TU

- preamble reuse。
- diagnostics cache。
- source text lazy loading。
- query plan compilation。

### 16.2 全コードベース

- TU 並列処理。ただし AST は thread-local。
- immutable fact extraction。
- background index cache。
- changed files + reverse include graph による affected TU 推定。
- system header suppression。
- memory budget に基づく LRU cache。

### 16.3 変換

- edit conflict detection をファイル単位で行う。
- formatting は changed ranges のみに限定。
- reparse verification は affected TU のみから始め、必要に応じて依存 TU へ広げる。

---

## 17. エラー処理と診断設計

### 17.1 エラー分類

| code prefix | 意味 |
|---|---|
| `project.*` | compile DB/toolchain/project 設定。 |
| `parse.*` | Clang parse failure。 |
| `query.*` | query 構築/実行失敗。 |
| `check.*` | check 実行失敗。 |
| `edit.*` | edit conflict/macro policy/reparse failure。 |
| `dynamic.*` | build/run/sanitizer parser 失敗。 |
| `ir.*` | IR parse/pass/verify/JIT 失敗。 |
| `raw.*` | raw corridor 使用上の lifetime/threading 違反。 |

### 17.2 診断例

```json
{
  "rule_id": "edit.macro-expansion-rejected",
  "severity": "error",
  "message": "Refusing to edit a token produced by macro expansion",
  "location": { "file": "src/a.cpp", "line": 12, "column": 5 },
  "explanation": "The selected expression is not spelled directly in the file. Editing the expansion could corrupt unrelated macro uses.",
  "suggestions": [
    "Use macro_policy::allow_spelling_location if you intend to edit the macro definition.",
    "Emit a diagnostic without fix-it instead."
  ]
}
```

---

## 18. 実装ロードマップ

### Phase 0: Spike

- LLVM/Clang 1 major version に固定。
- `project` / `session` / `translation_unit` 最小実装。
- `q::function()` / `q::call_expr()` / raw matcher bridge。
- `edit_plan` と clang-format。
- `code_fixture`。

### Phase 1: MVP

- check framework。
- JSON/SARIF report。
- CLI `query/check/rewrite`。
- macro policy、traversal policy。
- reparse verification。
- docs/examples。

### Phase 2: 実用化

- workspace index。
- include analyzer。
- clang-tidy adapter。
- mock generator。
- sanitizer runner。
- diff mode。
- parallel scheduler。

### Phase 3: 高度化

- dataflow framework。
- fuzz harness generator。
- declarative rule schema。
- CSA adapter。
- plugin adapter。
- IR pass/JIT utilities。

### Phase 4: エコシステム化

- rule registry。
- company-specific checks templates。
- LSP/IDE integration。
- AI agent SDK。
- multi-LLVM compatibility matrix。

---

## 19. 直呼び libTooling と比較した利点

| 項目 | libTooling 直呼び | `cxxlens` |
|---|---|---|
| 初期設定 | `CommonOptionsParser`、`ClangTool`、`FrontendAction` を理解する必要。 | `project::open()` → `analysis_session::create()`。 |
| AST 検索 | ASTMatchers 名と traversal を手探り。 | 意図ベース query DSL、spelled_only 既定、explain。 |
| マクロ | `SourceLocation` を自力処理。 | macro_origin と macro_policy。 |
| 編集 | `Replacements` 衝突、AtomicChange、format を自力統合。 | `edit_plan` が衝突検出・format・再パース検証。 |
| check | clang-tidy boilerplate。 | `check` 基底 + query registry + adapter。 |
| AI 利用 | エラーが C++ template/Clang 内部寄り。 | schema、examples、structured error。 |
| 大規模解析 | cache/index/scheduler を自作。 | session/index/diff/parallel を標準化。 |
| 未対応 | 自力で全部書く。 | raw corridor から公式 API へ降りる。 |

---

## 20. 仕様上の重要な約束

1. **`cxxlens` は Clang を隠して忘れさせるものではない。** Clang を安全に使うための足場である。
2. **未対応機能は「作れない」ではなく「raw corridor で作る」状態にする。** 高水準 API の不足がプロジェクト停止理由にならないようにする。
3. **編集は必ず検証可能にする。** 文字列置換を即適用する API は提供しない。
4. **標準設定は保守的にする。** 便利さより誤変換防止を優先する。
5. **AI のために説明責任を持つ。** すべての query/check/edit/report に `explain()` と schema を持たせる。
6. **LLVM/Clang の進化を前提にする。** version shim と feature gate を設計に含める。

---

## 21. 参考資料

[^clang-index]: [Clang documentation index: Using Clang as a Library](https://clang.llvm.org/docs/)。LibTooling、LibClang、LibFormat、Plugins、RecursiveASTVisitor、LibASTMatchers、Transformer、ASTImporter、JSON Compilation Database、Refactoring Engine などが列挙されている。
[^clang-interfaces]: [Choosing the Right Interface for Your Application](https://clang.llvm.org/docs/Tooling.html)。LibClang、Clang Plugins、LibTooling の用途と制約。
[^libtooling]: [LibTooling](https://clang.llvm.org/docs/LibTooling.html)。`runToolOnCode`、`CompilationDatabase`、`CommonOptionsParser`、`ClangTool`、`FrontendActionFactory`。
[^libclang]: [Introduction to the Clang AST / libclang tutorial](https://clang.llvm.org/docs/LibClang.html)。`CXIndex`、`CXTranslationUnit`、`CXCursor`、cursor traversal。
[^clang-ast]: [Introduction to the Clang AST](https://clang.llvm.org/docs/IntroductionToTheClangAST.html)。AST の構造、`ASTContext`、`RecursiveASTVisitor`。
[^astmatchers]: [Matching the Clang AST](https://clang.llvm.org/docs/LibASTMatchers.html)。LibASTMatchers の DSL と用途。
[^matcher-reference]: [AST Matcher Reference](https://clang.llvm.org/docs/LibASTMatchersReference.html)。traversal mode、`TK_IgnoreUnlessSpelledInSource`、`bind()` など。
[^transformer]: [Clang Transformer Tutorial](https://clang.llvm.org/docs/ClangTransformerTutorial.html)。AST matcher + edits による diagnostics/transformations。
[^refactoring-engine]: [Refactoring Engine](https://clang.llvm.org/docs/RefactoringEngine.html)。refactoring action/rule、source replacements、AtomicChanges。
[^atomic-change]: [clang::tooling::AtomicChange Class Reference](https://clang.llvm.org/doxygen/classclang_1_1tooling_1_1AtomicChange.html)。source edits と header changes を atomic にまとめる。
[^libformat]: [LibFormat](https://clang.llvm.org/docs/LibFormat.html)。`reformat()`、`tooling::Replacements`、FormatStyle。
[^json-cdb]: [JSON Compilation Database Format Specification](https://clang.llvm.org/docs/JSONCompilationDatabase.html)。compile command の再現形式。
[^clangd-compile]: [clangd: Compile commands](https://clangd.llvm.org/design/compile-commands)。解析に必要な compile command と fallback/header inference。
[^clang-tidy]: [clang-tidy](https://clang.llvm.org/extra/clang-tidy/)。Clang ベースの C++ linter、checks、parallel、diff、NOLINT。
[^clang-tidy-dev]: [Contributing to clang-tidy](https://clang.llvm.org/extra/clang-tidy/Contributing.html)。`ClangTidyCheck`、`registerMatchers()`、`check()`、PP callbacks、options。
[^csa]: [Clang Static Analyzer](https://clang-analyzer.llvm.org/)。path-sensitive inter-procedural symbolic execution。
[^csa-checker]: [Checker Developer Manual](https://clang-analyzer.llvm.org/checker_dev_manual.html)。ExplodedGraph、ProgramState、checker callbacks。
[^dataflow]: [Data flow analysis: an informal introduction](https://clang.llvm.org/docs/DataFlowAnalysisIntro.html)。CFG と fixpoint propagation。
[^clangd-index]: [clangd index design](https://clangd.llvm.org/design/indexing)。SymbolIndex、FileIndex、BackgroundIndex。
[^include-cleaner]: [Include Cleaner design](https://clangd.llvm.org/design/include-cleaner)。AST を使った include 利用判定と IWYU pragmas。
[^clangd-threading]: [clangd threading model](https://clangd.llvm.org/design/threads)。preamble は共有可能、AST は thread-safe ではない。
[^llvm-ir]: [LLVM Language Reference Manual](https://llvm.org/docs/LangRef.html)。SSA-based、type-safe、low-level IR。
[^llvm-pass]: [Using the New Pass Manager](https://llvm.org/docs/NewPassManager.html) および [Writing an LLVM New PM Pass](https://llvm.org/docs/WritingAnLLVMNewPMPass.html)。PassBuilder、Module/Function/Loop managers。
[^orc]: [ORCv2 Design and Usage](https://llvm.org/docs/ORCv2.html)。LLJIT/LLLazyJIT。
[^asan]: [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html)。memory bug detector、`-fsanitize=address`。
[^tsan]: [ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html)。data race detector。
[^msan]: [MemorySanitizer](https://clang.llvm.org/docs/MemorySanitizer.html)。uninitialized memory use detector。
[^ubsan]: [UndefinedBehaviorSanitizer](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html)。undefined behavior detector。
[^lsan]: [LeakSanitizer](https://clang.llvm.org/docs/LeakSanitizer.html)。leak detector。
[^dfsan]: [DataFlowSanitizer](https://clang.llvm.org/docs/DataFlowSanitizer.html)。dynamic data flow analysis。
[^tysan]: [TypeSanitizer](https://clang.llvm.org/docs/TypeSanitizer.html)。strict type aliasing violation detector。
[^rtsan]: [RealtimeSanitizer](https://clang.llvm.org/docs/RealtimeSanitizer.html)。realtime violation detector。
[^function-effect]: [Function Effect Analysis](https://clang.llvm.org/docs/FunctionEffectAnalysis.html)。`nonblocking` / `nonallocating` の compile-time diagnostics。
[^sancov]: [SanitizerCoverage](https://clang.llvm.org/docs/SanitizerCoverage.html)。coverage instrumentation。
[^source-coverage]: [Source-based Code Coverage](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html)。AST/preprocessor 情報を用いた coverage。
[^cfi]: [Control Flow Integrity](https://clang.llvm.org/docs/ControlFlowIntegrity.html)。control-flow integrity sanitizer。
[^libfuzzer]: [libFuzzer](https://llvm.org/docs/LibFuzzer.html)。in-process coverage-guided evolutionary fuzzing。
[^clang-internals]: [Clang Internals Manual](https://clang.llvm.org/docs/InternalsManual.html)。Basic、Diagnostics、SourceManager、Lexer、Parser、AST、Sema、CFG など。
[^llvm-error]: [LLVM Programmer’s Manual: Error handling](https://llvm.org/docs/ProgrammersManual.html)。`Expected<T>` / `Error`。
[^tablegen]: [TableGen Overview](https://llvm.org/docs/TableGen/) 。domain-specific records の保守と重複削減。
[^clang-cxx-standards]: [Clang Users Manual: C++ Language Features](https://clang.llvm.org/docs/UsersManual.html#c-language-features)。C++ 標準モード。

---

## 22. 付録: 最小ヘッダ構成案

```text
include/cxxlens/core/result.hpp
include/cxxlens/core/error.hpp
include/cxxlens/core/diagnostic.hpp
include/cxxlens/core/schema.hpp

include/cxxlens/source/location.hpp
include/cxxlens/source/range.hpp
include/cxxlens/source/source_manager_view.hpp

include/cxxlens/project/project.hpp
include/cxxlens/project/project_config.hpp
include/cxxlens/project/compile_command.hpp
include/cxxlens/project/toolchain.hpp

include/cxxlens/session/analysis_session.hpp
include/cxxlens/session/translation_unit.hpp

include/cxxlens/ast/ast_node.hpp
include/cxxlens/ast/decl.hpp
include/cxxlens/ast/stmt.hpp
include/cxxlens/ast/expr.hpp
include/cxxlens/ast/type.hpp
include/cxxlens/ast/visitor.hpp

include/cxxlens/query/query.hpp
include/cxxlens/query/builders.hpp
include/cxxlens/query/match.hpp
include/cxxlens/query/query_engine.hpp
include/cxxlens/query/raw_matcher.hpp

include/cxxlens/analysis/check.hpp
include/cxxlens/analysis/check_runner.hpp
include/cxxlens/analysis/diagnostic_sink.hpp
include/cxxlens/analysis/report.hpp

include/cxxlens/dataflow/dataflow_analysis.hpp
include/cxxlens/dataflow/cfg.hpp
include/cxxlens/dataflow/lattice.hpp

include/cxxlens/transform/edit.hpp
include/cxxlens/transform/edit_plan.hpp
include/cxxlens/transform/rewrite_rule.hpp
include/cxxlens/transform/transformer.hpp

include/cxxlens/format/format_style.hpp
include/cxxlens/format/formatter.hpp

include/cxxlens/index/workspace_index.hpp
include/cxxlens/index/symbol.hpp
include/cxxlens/index/reference.hpp

include/cxxlens/include/include_graph.hpp
include/cxxlens/include/include_analyzer.hpp

include/cxxlens/mock/public_surface.hpp
include/cxxlens/mock/semantic_copier.hpp
include/cxxlens/mock/mock_generator.hpp

include/cxxlens/dynamic/sanitizer_profile.hpp
include/cxxlens/dynamic/dynamic_runner.hpp
include/cxxlens/dynamic/fuzz_harness_generator.hpp

include/cxxlens/ir/ir_module.hpp
include/cxxlens/ir/pass_pipeline.hpp
include/cxxlens/ir/jit_engine.hpp

include/cxxlens/testing/code_fixture.hpp
include/cxxlens/testing/golden_test.hpp
include/cxxlens/testing/property_test.hpp

include/cxxlens/all.hpp
```

---

## 23. 付録: 最小 CMake 構成案

```cmake
cmake_minimum_required(VERSION 3.28)
project(cxxlens LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

add_library(cxxlens-core ...)
add_library(cxxlens-source ...)
add_library(cxxlens-project ...)
add_library(cxxlens-session ...)
add_library(cxxlens-ast ...)
add_library(cxxlens-query ...)
add_library(cxxlens-analysis ...)
add_library(cxxlens-transform ...)
add_library(cxxlens-format ...)
add_library(cxxlens-index ...)
add_library(cxxlens-include ...)
add_library(cxxlens-mock ...)
add_library(cxxlens-dynamic ...)
add_library(cxxlens-ir ...)
add_library(cxxlens-testing ...)

add_executable(cxxlens tools/cxxlens/main.cpp)
```

---

## 24. 結論

`cxxlens` は、LLVM/Clang の能力を「使いやすくする」だけでなく、**正確な C++ 意味解析を日常的な品質保証ワークフローに持ち込むための抽象レイヤ**である。

この設計の中心は、次の二重構造である。

- 初心者・AI エージェント向けには、短く、一貫し、安全で、説明可能な API を提供する。
- 熟練者・高度用途向けには、Clang/LLVM 公式 API へ降りる raw corridor を標準仕様として提供する。

この二重構造により、`cxxlens` は「便利だが閉じたラッパ」ではなく、**Clang/LLVM の力を保ったまま、誤りを減らし、意図を明瞭にし、保守可能性を高める開発基盤**になる。
