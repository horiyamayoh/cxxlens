# cxxlens agent contract

## Authority and reading order

判断が衝突する場合は、次の順序を優先する。

1. `docs/design/cxxlens_next_generation_integrated_design_ja.md` の原則と invariant
2. `schemas/cxxlens_ng_relation_registry.yaml`、Provider Protocol、Public C++ API Catalog、
   Acceptance Manifest、Security Profile
3. accepted ADR と担当 GitHub issue の exact contract
4. acceptance fixture と実装
5. `docs/archive/` の履歴資料

ただし **repository delivery workflow** に限り、この文書の 2026-08-16 direct-to-main amendment を
現在の運用 authority とする。ADR 0094、goal document、過去の issue/PR prose にある branch/PR 必須の記述と
衝突する場合は、この amendment が優先する。製品 semantics、security、qualification の authority 順は変更しない。

実装前に次世代統合設計書の 0、2、5〜9、11、14、15、17、20、26〜28 章と、担当 relation/API/provider の
catalog entry または移行 issue を読む。旧124 API catalog/freeze は新規実装を認可しない。
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

## Direct-to-main delivery policy

通常の開発経路は **direct-to-main** とする。feature branch と pull request は既定の前提ではない。

1. 作業開始時と書き込み直前に current `main` SHA を確認し、他の active unit と contract/path が競合していないことを確認する。
2. issue の exact scope に限定して編集し、変更を小さく可逆な単位に保つ。
3. 変更箇所に対応する build/test/checker を事前に実行する。実行不能なら理由と未検証範囲を evidence に残す。
4. history rewrite を使わず、`main` 上に atomic commit を作成して canonical repository を fast-forward する。
5. 更新後の exact SHA に対する `main` CI を統合 gate とし、変更起因の失敗は follow-up commit で直す。既存履歴を書き換えない。
6. bounded implementation evidence、commit SHA、残余 gap、Learning checkpoint を issue に残して close 判定を行う。

pull request は、外部 contributor、独立反証 review が必要な high-risk normative/security/public-semantics 変更、
または隔離しなければ安全に並行できない作業に限る任意経路とする。任意 PR を使う場合も、PR の存在そのものを
implementation completion の証拠にせず、最終 `main` SHA の evidence を使用する。

force update、reset、rebase による公開履歴の改変、secret/permission、課金、外部 production deploy はこの方針で認可しない。
branch protection/ruleset が mandatory PR を強制する場合は、明示された repository owner approval の下で設定を
direct-to-main と整合させる。設定変更 capability が利用できない実行環境では、設定を変更したと主張してはならない。

- `delivery-route: main-atomic-commit-post-update-ci`
- `pull-request: optional-risk-or-external-contribution`
- `history-policy: fast-forward-no-rewrite`

## Issue completion and qualification boundary

通常の implementation issue の既定完了クラスは **bounded implementation completion** とする。
issue を閉じるために distribution 全体の production qualification を再実行・再証明してはならない。

bounded implementation completion は、担当 issue の exact contract と明示 scope に対して次を要求する。

- 宣言した実装範囲が完成し、scope 内に placeholder、silent fallback、既知の correctness/security/invariant blocker が残っていない
- 変更した振る舞いと直接 dependency closure に対する positive/negative test、必要な determinism/resource/error evidence が成功する
- 変更した public contract、schema、catalog、Doxygen、example、生成 inventory のうち直接影響するものが整合する
- scope 外の native/platform/static/shared/install/consumer/Nightly/release evidence は、必要なら別 issue または tracked gap に
  owner、依存順、完了条件とともに残す
- commit と issue close evidence が、実装完了、support/stability、production qualification を混同せず、
  `production qualification: not claimed` または issue が所有する限定的な qualification claim を明示する
- Learning checkpoint を `none` または関連 DF ID として記録する

通常の issue には、全 static/shared matrix、installed consumer 全件、native toolchain/platform matrix、`full`/`stress`、
Nightly、release evaluation、terminal production-scope closure、無関係な issue/gate の完了を要求しない。
それらは updated `main`、Nightly/release workflow、または exact contract と label で明示された
`integration-gate` / `readiness-gate` / qualification issue が所有する。

後続の統合 gate が失敗した場合は、まず統合 failure を owner issue に記録する。閉じた implementation issue を再度開くのは、
その failure が当該 issue の bounded acceptance を誤りと証明した場合、または当該 scope に regression がある場合だけとする。
単に製品全体が未認定であることは reopen 理由にしない。

## Goal standing authorization

Repository policy `CXXLENS_AGENT_AUTHORIZATION_V1` は、`/goal` が
`docs/development/agent-api-development-goal.md` を policy ID とともに実行契約として明示参照した実行中だけ有効とする。
通常の質問、診断、read-only review から暗黙に起動せず、ユーザーはいつでも authorization を revoke または narrow できる。

- `activation: explicit-goal-contract-reference`
- `non-activation: ordinary-request`
- `standing-scope: canonical-repository-active-unit`
- `platform-approval: never-bypass`

active unit 内の可逆な実装、検証、同一 issue の CI 根本修正、atomic commit、canonical repository の fast-forward update、
active issue workflow、必要に応じた任意 PR workflow は standing authorization の範囲とする。当初想定外の supporting file が
同一 contract・同一 issue 内で必要なら、原因、追加 scope、検証方法を通知して継続する。

destructive/history rewrite、secret/permission、課金、外部 production deploy、active workflow 外の第三者連絡、
ユーザー変更との解消不能な競合、authority で決められない重大な public semantics は、対象と effect を開示して fresh approval を得る。
sandbox/system/platform の approval は standing authorization で迂回しない。

次の文字列は frozen readiness baseline との互換性だけのために保持する **non-normative legacy checker token** であり、
この文書の delivery policy を表さない。agent は token の語義から branch/PR 必須運用を復活させてはならない。

- `protected-main: unit-branch-pr-exact-head-review-merge-exact-merged-main`

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
- public API/relation/provider を変更したら次世代 catalog/registry、Doxygen、acceptance test、設計 traceability を更新する。
- 実装事実が contract と矛盾する、hidden assumption が見つかる、または reusable な設計知見を得た場合は materiality を判定し、
  `docs/development/implementation-learning/` の workflow で記録する。
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
- routine change に branch/PR を機械的に要求して throughput を落とすこと

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
それ以外の全体 evidence は exact `main` SHA と統合／release gate が所有する。

公開 API は header/signature/ownership、error/unresolved/coverage、ID/order、schema/invariant、
positive/negative test、example、catalog ID に加え、consumer/use-case trace、需要側 closure disposition、
actionable unknown/completion plan、必要な constructibility witness が **当該 issue の宣言範囲内で** 揃えば
bounded implementation completion として完成扱いにできる。production-supported または release-qualified の宣言は、
対応する独立 qualification gate が成功するまで行わない。
