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

## Goal standing authorization

Repository policy `CXXLENS_AGENT_AUTHORIZATION_V1` は、`/goal` が
`docs/development/agent-api-development-goal.md` を policy ID とともに実行契約として明示参照した実行中だけ有効とする。
通常の質問、診断、read-only review から暗黙に起動せず、ユーザーはいつでも authorization を revoke または narrow できる。

- `activation: explicit-goal-contract-reference`
- `non-activation: ordinary-request`
- `standing-scope: canonical-repository-active-unit`
- `platform-approval: never-bypass`
- `protected-main: unit-branch-pr-exact-head-review-merge-exact-merged-main`

active unit 内の可逆な実装、検証、同一 issue の CI 根本修正、unit branch/commit/push、canonical repository 上の active
issue/PR workflow は standing authorization の範囲とする。当初想定外の supporting file が同一 contract・同一 issue 内で必要なら、
原因、追加 scope、検証方法を通知して継続する。

destructive/history rewrite、branch protection、secret/permission、課金、外部 production deploy、active issue/PR workflow 外の
第三者連絡、ユーザー変更との解消不能な競合、authority で決められない重大な public semantics は、対象と effect を開示して fresh
approval を得る。sandbox/system/platform の approval は standing authorization で迂回しない。`main` は unit branch、PR、exact-head
required checks、review resolution、merge、exact merged-main qualification の順でのみ更新する。

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

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang
cmake --build --preset dev-clang --target cxxlens-quality
```

公開 API は header/signature/ownership、error/unresolved/coverage、ID/order、schema/invariant、
positive/negative test、example、catalog ID に加え、consumer/use-case trace、需要側 closure disposition、
actionable unknown/completion plan、必要な constructibility witness が揃うまで完成扱いにしない。
