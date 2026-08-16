# ADR 0094: `/goal` delegation と direct-to-main integration を risk-tiered に束縛する

- Status: Accepted
- Date: 2026-07-19
- Last amended: 2026-08-16
- Issue: #176
- Design feedback: DF-0177 / #177
- Depends on: ADR 0088, ADR 0089, ADR 0093
- Amended by: ADR 0095; workflow amendment #263; completion-policy amendment #291; direct-to-main amendment 2026-08-16

## Context

cxxlens は issue 単位の実装、検証、commit、push、CI 修復、issue 更新をコーディングエージェントへ委任する。
従来は全変更を branch/PR/merge に通し、個別 issue と全体 integration の証拠を同じ搬送路へ結合していた。この方式は
exact-head review には強い一方、bounded implementation completion を採用した現在の開発では待ち行列と重複確認を生み、
独立した小さな変更まで PR gate に滞留させる。

必要なのは品質証拠の削除ではなく、そのタイミングの変更である。局所品質は push 前に affected checks で確認し、全体品質は
push 後の exact `main` SHA で fail closed に評価する。履歴改変や競合の押し込みを許さず、失敗時に修正または revert するなら、
direct-to-main は bounded issue throughput と aggregate qualification を両立できる。

## Decision

repository 限定 policy `CXXLENS_AGENT_AUTHORIZATION_V1` を採用し続ける。この policy は
`docs/development/agent-api-development-goal.md` と policy ID を明示参照した goal の実行中だけ有効で、通常の質問や read-only
review から暗黙に起動しない。

通常の active unit は次の順で処理する。

1. issue、contract ID、repository-relative write path を宣言し、他 unit との contract/path conflict を排除する。
2. 最新の `origin/main` から作業し、affected positive/negative test、必要な quality check、self-review を実行する。
3. unrelated change を含めない issue-scoped commit を作る。
4. remote head を再確認し、fast-forward で `main` へ直接 push する。
5. push された exact main SHA の CI を監視し、失敗時は根本修正 commit または明示的 revert を追加する。
6. bounded implementation evidence、残余 gap ownership、learning checkpoint を issue に記録する。

force-push、history rewrite、未解決 conflict の押し込み、複数 issue の無関係な差分の同梱は禁止する。remote `main` が進んだ場合は
最新 head へ安全に載せ直し、affected checks を再実行する。

PR は high-risk public contract の independent counterexample review、外部 contributor、またはユーザーの明示要求に使える任意の
review mechanism とする。通常 unit の `main` 反映や close の必須 gate にはしない。high-risk change に必要な review は PR の有無ではなく、
review evidence と constructibility gate で判定する。

standing authorization は active unit 内の可逆な編集、build/test、CI 根本修正、issue-scoped commit、fast-forward main push、
active issue 更新、exact-main CI 監視、bounded close を含む。destructive operation/history rewrite、branch protection、
secret/permission、課金、外部 production deploy、第三者連絡、重大な未決 public semantics は fresh user approval を要求する。
platform approval は迂回しない。

binding marker は次とする。

- `activation: explicit-goal-contract-reference`
- `non-activation: ordinary-request`
- `standing-scope: canonical-repository-active-unit`
- `direct-main: issue-scoped-fast-forward-push-post-push-integration`
- `pull-request: optional-for-risk-review-or-external-contribution`
- `history-rewrite: prohibited`
- `platform-approval: never-bypass`

通常の implementation issue は bounded implementation completion を所有し、full/stress/Nightly/release/production-scope qualification は
exact main SHA の integration/readiness/release owner が一度だけ所有する。後続 failure で issue を reopen するのは、bounded acceptance
の誤りまたは当該 scope の regression が証明された場合だけとする。

## Consequences

- PR 待ちを通常変更のクリティカルパスから外し、独立 issue の throughput を上げられる。
- push 前の affected checks と push 後の exact-main CI の責務が分離される。
- main CI failure は隠さず、修正 commit または revert により履歴上で追跡できる。
- force-push と history rewrite は引き続き禁止され、fast-forward の線形履歴を維持する。
- PR と独立 review は必要な高リスク変更・外部 contribution で引き続き利用できる。
- production qualification の強度は維持しつつ、個別 issue への重複要求を避ける。

## Verification

readiness checker は policy ID の exact binding、direct-main marker、PR optional marker、fast-forward/post-push integration contract、
fresh approval、platform carve-out を fail closed に検証する。旧 PR-mandatory marker または direct-main prohibition の再導入を拒否する。

scenario test は、通常 unit の fast-forward main pushが許可されること、remote head 変化時に再同期・再検証すること、CI failure が
修正または revert に至ること、force-push・history rewrite・branch protection 変更が standing authorization に含まれないことを確認する。
