# ADR 0094: `/goal` delegationを risk-tiered standing authorization に束縛する

- Status: Accepted
- Date: 2026-07-19
- Issue: #176
- Design feedback: DF-0177 / #177
- Depends on: ADR 0088, ADR 0089, ADR 0093

## Context

cxxlens の API-development goal は、issue 単位の実装、CI 修復、commit、push、issue/PR 更新、CI 監視を
統括 agent へ委任する。しかし、その委任が後続 skill の approval gate をどの範囲で満たすか、通常の質問でも有効になるか、
どの操作で fresh user approval が必要かを定義していない。このため、同一 issue 内の可逆な CI fixture 修復でも重複した
会話承認が発生し得る。

同時に goal 文書には完了 issue を `main` へ push する古い記述があり、現在の protected-main 運用に必要な PR、exact-head
check、review resolution、merge、merged-main qualification を表現していない。自律性を高めるには、許可範囲を曖昧に広げるのではなく、
通常 unit work と high-impact boundary を同じ durable contract で分離する必要がある。

## Decision

repository 限定の policy `CXXLENS_AGENT_AUTHORIZATION_V1` を採用する。この policy は、`/goal` が
`docs/development/agent-api-development-goal.md` を実行契約として policy ID とともに明示参照した実行中だけ有効とする。
通常の質問、診断依頼、read-only review から暗黙に起動しない。ユーザーは実行中でも authorization を revoke または narrow できる。

操作を次の五区分へ分ける。

1. **Standing authorization**: read-only audit、active unit 内の編集・生成・test/build、同一 issue の CI 根本修正、
   unit branch/commit/push、canonical cxxlens repository 上の active issue/PR に限定した更新・check rerun・review 対応、
   exact-head gate を満たした active PR の merge、および merged-main qualification と learning checkpoint を満たした active issue の
   close は再承認不要とする。無関係な issue、PR、branch の mutable state は含めない。
2. **Notify and continue**: 当初想定外の supporting test/file が必要でも、同一 contract・同一 issue 内で可逆なら、原因、追加 scope、
   検証方法を commentary で通知して継続する。これは approval gate ではない。
3. **Fresh user approval**: destructive operation/history rewrite、branch protection 変更、secret/permission 追加、課金、外部 production
   deploy、canonical cxxlens repository の active issue/PR workflow 外にいる顧客・第三者への連絡、ユーザー変更との解消不能な競合、
   authority で決められない重大な public semantics は停止する。対象、effect、不可逆性または rollback 方法を開示し、その exact
   target/effect に限定した明示承認を得る。別 target や後続 effect へ categorical に流用しない。
4. **External blocker**: 必須 reviewer、toolchain、service、permission を取得できない場合は、証拠と選択肢を示して停止する。
5. **Platform approval**: sandbox、system、host platform が要求する権限確認は standing authorization で迂回しない。

skill が一般的な explicit approval を要求しても、その操作が active policy の standing-authorization 範囲に列挙されていれば、
goal 開始時の承認で満たされたものとする。skill が要求する診断、focused plan、結果報告は省略せず、approval のためだけに会話を
停止しない。skill がより具体的な安全条件を持つ場合や操作が列挙範囲外の場合は、その条件または fresh-approval gate を維持する。

protected `main` への変更は、unit branch、PR、exact-head required checks、未解決 review の解消、merge、exact merged-main
Foundation/Wave 0/G5/release qualification の順で行う。直接 main push を durable workflow として認可しない。merge は branch
protection、exact-head required checks、review、single-active-unit invariant を満たした場合に限る。issue close はさらに exact
merged-main qualification、completion evidence、learning checkpoint を満たした後に限る。

`AGENTS.md` と goal document は policy ID をそれぞれ exactly once 参照し、API-development readiness checker は activation と
通常会話での non-activation、standing authorization の active-unit scope、notification、target/effect-specific fresh approval、
external blocker、platform carve-out、protected-main workflow、revoke/narrow、および skill compatibility の binding を fail closed に
検証する。旧 direct-main workflow の再導入も拒否する。

## Consequences

- 同一 issue・同一 contract 内の通常実装と CI 根本修正は、診断報告後に重複承認なしで継続できる。
- supporting file の追加は scope drift として隠さず通知するが、可逆な unit 内変更だけなら会話停止を要求しない。
- destructive または外部影響を持つ操作、重大な意味決定、platform permission は standing authorization から明示的に除外される。
- goal 外の通常会話は従来どおり request scope に従い、この policy から追加 authority を得ない。
- personal skill と curated plugin cache は変更せず、repository contract 側で compatibility を定義する。
- protected-main workflow が execution contract と checker の両方に固定される。

## Verification

API-development readiness unit test は完全な policy を positive fixture として受理し、policy ID binding、platform carve-out、
fresh-approval tier、external-blocker rule、通常会話での non-activation の各欠落と direct-main 文言の再導入を個別に拒否する。
scenario review は PR CI の局所 fixture 修正が notify-and-continue であること、force-push・branch protection 変更・production deploy が
target/effect-specific fresh approval であること、通常の質問で standing authorization が起動しないことを反証する。

変更 PR は exact-head required checks、独立 reviewer の design-feedback comment、review resolution を満たしてから merge し、
merged-main SHA で Foundation、Wave 0、G5、release qualification を完了する。
