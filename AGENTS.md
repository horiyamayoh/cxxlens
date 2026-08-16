# cxxlens agent contract

## Authority and reading order

判断が衝突する場合は、次の順序を優先する。

1. `docs/design/cxxlens_next_generation_integrated_design_ja.md` の原則と invariant
2. `schemas/cxxlens_ng_relation_registry.yaml`、Provider Protocol、Public C++ API Catalog、
   Acceptance Manifest、Security Profile
3. accepted ADR と担当 GitHub issue の exact contract
4. acceptance fixture と実装
5. `docs/archive/` の履歴資料

実装前に次世代統合設計書の 0、2、5〜9、11、14、15、17、20、26〜28 章と、担当 relation/API/provider の
catalog entry または移行 issue を読む。旧124 API catalog/freezeは新規実装を認可しない。
`schemas/cxxlens_asset_migration_ledger.json` の archived entry は履歴であり、active authority ではない。

authority を読んだ後、実装前に `docs/development/implementation-learning/README.md`、担当 scope の curated mental model、
および同じ contract/path を参照する未解決 design feedback record を読む。mental model と feedback record は non-normative であり、
上記 authority を上書きしない。

## Product direction and demand closure

製品方向は次の一文で固定する。

> cxxlens は、コンパイラの観測結果を、再現可能・問い合わせ可能・証拠付きの意味知識へ変換し、
> 分からない場合には何が足りないかまで返す基盤である。

新しい public surface、relation、provider、analysis、model、recipe を提案・実装する前に、次を exact に宣言する。

- 独立 consumer と、その consumer が答えたい質問または実行したい use case
- 既存 capability graph で満たせる部分と、足りない capability
- `proved`、`disproved`、`unknown`、`partial`、`conflicting` のどの結果を返すか
- coverage、closure、unresolved、conflict、differential disagreement、guarantee、provenance の保持方法
- `unknown` の原因と、結果を強化する dependency-ordered completion plan
- 追加 surface が orphan にならない根拠、または需要側 tracked gap
- exact contract ID、authority、write scope、evidence、support/stability disposition

公開 API 数、relation 数、green test 数だけを製品完成の根拠にしない。供給側 inventory に orphan surface がなく、
admitted use case に executable capability path または明示 tracked gap があり、最終的に両側の orphan がゼロであることを要求する。

高リスク contract は acceptance 前に constructibility を反証する。public semantics、identity、protocol、persistence、
不可逆 effect、resource bound を変更する場合は、executable state machine、field availability by phase、
phase-authentic outcome union、minimal witness、bounded resource witness、crash/effect matrix、
independent counterexample review を揃える。実装時に存在しない値を report へ要求したり、失敗 phase より後の identity を
捏造したりしてはならない。

coding agent は authority 全体から作業契約を推測しない。goal/use case、capability gap、最小 reading set、exact contract ID、
allowed write paths、required evidence、既知 design feedback、forbidden shortcuts、completion commands を持つ
最小 context を生成または issue に固定してから書き込みを開始する。

## Issue completion and qualification boundary

通常の implementation issue の既定完了クラスは **bounded implementation completion** とする。
issue を閉じるために distribution 全体の production qualification を再実行・再証明してはならない。

bounded implementation completion は、担当 issue の exact contract と明示 scope に対して次を要求する。

- 宣言した実装範囲が完成し、scope 内に placeholder、silent fallback、既知の correctness/security/invariant blocker が残っていない
- 変更した振る舞いと直接 dependency closure に対する positive/negative test、必要な determinism/resource/error evidence が成功する
- 変更した public contract、schema、catalog、Doxygen、example、生成 inventory のうち直接影響するものが整合する
- scope 外の native/platform/static/shared/install/consumer/Nightly/release evidence は、必要なら別 issue または tracked gap に
  owner、依存順、完了条件とともに残す
- PR と issue close evidence が、実装完了、support/stability、production qualification を混同せず、
  `production qualification: not claimed` または issue が所有する限定的な qualification claim を明示する
- Learning checkpoint を `none` または関連 DF ID として記録する

通常の issue には、全 static/shared matrix、installed consumer 全件、native toolchain/platform matrix、`full`/`stress`、
Nightly、release evaluation、terminal production-scope closure、無関係な issue/gate の完了を要求しない。
それらは merged `main`、Nightly/release workflow、または exact contract と label で明示された
`integration-gate` / `readiness-gate` / qualification issue が所有する。

後続の統合 gate が失敗した場合は、まず統合 failure を owner issue に記録する。閉じた implementation issue を再度開くのは、
その failure が当該 issue の bounded acceptance を誤りと証明した場合、または当該 scope に regression がある場合だけとする。
単に製品全体が未認定であることは reopen 理由にしない。

この節は、下位の development prose にある「各 implementation issue の close に exact merged-main production
qualification が必要」という一般化を置換する。protected-main と最終 release qualification 自体は弱めず、
責任を個別実装から統合／release gate へ移す。

## Goal standing authorization

Repository policy `CXXLENS_AGENT_AUTHORIZATION_V1` を明示参照する goal は、この repository の active unit に限って
standing authorization を有効化します。

- `activation: explicit-goal-contract-reference`
- `non-activation: ordinary-request`
- `standing-scope: canonical-repository-active-unit`
- `platform-approval: never-bypass`
- `direct-main: issue-scoped-fast-forward-push-post-push-integration`

active unit 内の可逆な実装・生成・build/test、同一 issue の CI 根本修正、issue-scoped commit、最新 `main` への
fast-forward push、active issue の更新、push 後の exact-main CI 監視と修正は再承認不要です。通常の質問、診断、read-only
review、または policy を明示しない依頼から、この権限を暗黙に取得してはなりません。

PR は既定の搬送路ではありません。high-risk contract の独立反証 review、外部 contributor、またはユーザーが明示した場合に限る
任意の review mechanism とします。PR を使う場合も、active unit と bounded completion の責務境界は変えません。

`main` 更新前に最新の remote head を取得し、active unit の contract/path 非競合、対象差分、affected build/test を確認します。
`main` は fast-forward だけで更新し、force-push、history rewrite、未確認の unrelated change の同梱を禁止します。push 後は
その exact `main` SHA の CI を監視し、失敗時は根本修正 commit または revert を直ちに追加して issue に記録します。

destructive operation/history rewrite、branch protection・secret・permission の変更、課金、外部 production deploy、顧客・第三者への
連絡、解消不能なユーザー変更との競合、authority から決められない重大な public semantics は、対象と effect を開示した fresh user
approval を要求します。sandbox/system/host platform の approval gate はこの policy で迂回しません。
## Required implementation rules

- C++23 を使用し、公開 namespace/type/function は設計書の lower snake case に従う。
- 通常の public header に `clang::*`、`llvm::*` または LLVM/Clang header を露出しない。
- schema-first の順序は semantics/invariants、identity、value types、schema、validator、tests、service とする。
- filesystem、process、time、hash は port 越しに扱う。
- AST pointer を保存、所有、別スレッドへ移送しない。raw owning pointer を導入しない。
- unordered container の iteration order を serialization や ID に使用しない。
- read result は empty と unresolved を区別し、evidence/coverage/guarantee を落とさない。
- `unknown` は不足 input/capability/model/evidence と completion plan を失わない。
- mutation/generation は plan、独立 validator、dry-run、transaction の順を崩さない。
- public API/relation/provider を変更したら次世代 catalog/registry、Doxygen、acceptance test、設計
  traceability を更新する。旧124 API catalogへ新規surfaceを追加しない。
- 実装事実が contract と矛盾する、hidden assumption が見つかる、または reusable な設計知見を得た場合は materiality を判定し、
  `docs/development/implementation-learning/` の workflow で記録する。correctness/security/invariant/compatibility/不可逆変更は
  解決まで対象実装を block する。
- implementation issue の完了時に learning checkpoint を行い、`none` または関連 DF ID を evidence に残す。

## Forbidden shortcuts

- name や pretty type string だけによる semantic identity
- compile command や variant の silent fallback/first-wins
- macro expansion range への直接 edit
- conflict、stale digest、variant、reparse failure の無視
- unsupported surface、consumer gap、unresolved capability の omission
- API 数や relation 数だけによる completion claim
- actionable な不足理由を持たない generic `unknown`
- diagnostic prose substring による制御
- shell command の文字列連結
- test に合わせた上位 contract の縮小
- design feedback を記録しない silent contract deviation、または non-normative record の authority 扱い

## Commands and completion

通常の issue は、issue に固定した affected target/test/checker を実行する。代表例は次のとおり。

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang --target <affected-targets>
ctest --preset dev-clang -R '<affected-tests>' --output-on-failure
python3 tools/quality/run_gate.py fast --preset dev-clang \
  --report build/dev-clang/fast-report.json
```

public contract/schema/documentation を変更した場合は、その変更に直接対応する validator/checker を追加する。
`cmake --build --preset dev-clang --target cxxlens-quality`、`run_gate.py check|full|stress`、install/native matrix、
Nightly/release command は、その checker または qualification surface を issue が明示的に所有する場合だけ issue 完了条件に含める。
それ以外の全体 evidence は `main` と統合／release gate が所有する。

公開 API は header/signature/ownership、error/unresolved/coverage、ID/order、schema/invariant、
positive/negative test、example、catalog ID に加え、consumer/use-case trace、需要側 closure disposition、
actionable unknown/completion plan、必要な constructibility witness が **当該 issue の宣言範囲内で** 揃えば
bounded implementation completion として完成扱いにできる。production-supported または release-qualified の宣言は、
対応する独立 qualification gate が成功するまで行わない。
