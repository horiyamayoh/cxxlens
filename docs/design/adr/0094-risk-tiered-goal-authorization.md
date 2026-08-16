# ADR 0094: `/goal` delegationを risk-tiered standing authorization に束縛する

- Status: Accepted
- Date: 2026-07-19
- Issue: #176
- Design feedback: DF-0177 / #177
- Depends on: ADR 0088, ADR 0089, ADR 0093
- Amended by: ADR 0095; workflow amendment #263; completion-policy amendment #291

## Context

cxxlens の API-development goal は、issue 単位の実装、CI 修復、commit、push、issue/PR 更新、CI 監視を
統括 agent へ委任する。しかし、その委任が後続 skill の approval gate をどの範囲で満たすか、通常の質問でも有効になるか、
どの操作で fresh user approval が必要かを定義していない。このため、同一 issue 内の可逆な CI fixture 修復でも重複した
会話承認が発生し得る。

同時に goal 文書には完了 issue を `main` へ push する古い記述があり、現在の protected-main 運用に必要な PR、exact-head
check、review resolution、merge、merged-main qualification を表現していない。自律性を高めるには、許可範囲を曖昧に広げるのではなく、
通常 unit work と high-impact boundary を同じ durable contract で分離する必要がある。

運用開始後、別の過剰結合が明らかになった。個別 implementation issue の完了条件へ Foundation、Wave 0、G5、install/native matrix、
Nightly、release evaluation、terminal production-scope closure まで取り込むと、担当 scope の実装と直接品質証拠が完成しても、
製品全体の未認定や無関係な lane の失敗だけで issue を閉じられない。これは final correctness を強くするのではなく、
同じ production qualification を各 issue が重複所有することで throughput と責任境界を悪化させる。

## Decision

repository 限定の policy `CXXLENS_AGENT_AUTHORIZATION_V1` を採用する。この policy は、`/goal` が
`docs/development/agent-api-development-goal.md` を実行契約として policy ID とともに明示参照した実行中だけ有効とする。
通常の質問、診断依頼、read-only review から暗黙に起動しない。ユーザーは実行中でも authorization を revoke または narrow できる。

操作を次の五区分へ分ける。

1. **Standing authorization**: read-only audit、active unit 内の編集・生成・test/build、同一 issue の CI 根本修正、
   unit branch/commit/push、canonical cxxlens repository 上の active issue/PR に限定した更新・check rerun・review 対応、
   exact-head gate を満たした active PR の merge、および merge 済み bounded implementation completion evidence と
   learning checkpoint を満たした active issue の close は再承認不要とする。明示的な integration/readiness/qualification issue は、
   自身が所有する qualification evidence も満たしてから close する。無関係な issue、PR、branch の mutable state は含めない。
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
integration evaluation の順で行う。直接 main push を durable workflow として認可しない。merge は branch protection、
exact-head required checks、review、bounded conflict-scoped active-unit invariant を満たした場合に限る。

通常の implementation issue close は、merge 済みであること、担当 scope の bounded implementation completion evidence、
残余 gap の明示 ownership、completion evidence、learning checkpoint を要求するが、Foundation/Wave 0/G5/Nightly/release または
terminal production-scope qualification を当該 issue が明示的に所有しない限り待たない。protected-main の repository guard と
issue の semantic completion claim は区別する。

`AGENTS.md` と goal document は policy ID をそれぞれ exactly once 参照し、API-development readiness checker は activation と
通常会話での non-activation、standing authorization の active-unit scope、notification、target/effect-specific fresh approval、
external blocker、platform carve-out、protected-main workflow、revoke/narrow、および skill compatibility の binding を fail closed に
検証する。旧 direct-main workflow の再導入も拒否する。

### Workflow amendment #263 — bounded conflict-scoped parallel units

repository 全体を一つの write lock とする運用は、contract と path が独立した Nightly 修復、provider hardening、runtime 実装まで
不必要に直列化した。active write unit は最大四つまで許可する。ただし各 unit は issue、contract ID、repository-relative write path を
宣言し、異なる unit 間で contract ID が一致する場合、または write path が同一・祖先・子孫関係にある場合は conflict として
fail closed にする。shared authority を変更する work は共通 contract ID/path を宣言するため引き続き直列になる。

standing authorization は各 unit の境界を越えない。並列 unit を理由に別 issue/branch の mutable state を変更する権限は生じず、
各 PR は独立に exact-head checks と review resolution を満たす。merged-main integration evaluation はその gate owner が集約し、
ordinary implementation issue が同じ production qualification を重複所有しない。最大数は throughput の上限であり、
依存関係または evidence cost が直列化を要求する場合に四レーンを使い切る義務はない。

### ADR 0095 amendment — intermediate evaluation と final GR

ADR 0095 は authorization tier と protected-main branch/PR sequence を変更しない。production scope に tracked gap が残る間の
post-merge evidence だけを、required checks、Foundation、Wave 0、G5、別名の `release-evaluation`、normal terminal scope report に
分離する。green な `release-evaluation: not-qualified` は `gate.release`、GR、production support を満たさない。全 gap 解消後の final
workflow は `release-evaluation: qualified`、`release-qualification` の strict GR v1、final terminal scope report を要求する。

この post-merge integration evaluation は final product claim のために維持するが、completion-policy amendment #291 により、
ordinary implementation issue の close 条件ではない。明示的な integration/readiness/qualification issue だけが、自身の exact contract に
列挙した post-merge evidence を close 条件として所有する。

### Completion-policy amendment #291 — bounded implementation completion

通常の implementation issue の既定 completion class を次に固定する。

```text
completion-class: bounded-implementation
production-qualification: not-claimed
```

bounded implementation completion は担当 issue の宣言 scope に対して次を要求する。

- exact contract の実装が完成し、scope 内に placeholder、silent fallback、既知 correctness/security/invariant blocker がない
- 変更した振る舞いと直接 dependency closure の positive/negative test、および必要な determinism/resource/error evidence が成功する
- 直接影響する public contract、schema、catalog、Doxygen、example、documentation、generated inventory が整合する
- scope 外の integration/native/platform/static/shared/install/consumer/Nightly/release work に別 owner、依存順、完了条件がある
- PR/close evidence が implementation completion、support/stability、production qualification を混同しない
- Learning checkpoint が `none` または関連 DF ID として記録される

通常の issue に全 static/shared matrix、全 installed consumer、native/platform matrix、`full`/`stress`、Nightly、release evaluation、
terminal production-scope closure、無関係な gate/issue の完了を要求しない。それらは merged `main`、Nightly/release workflow、
または exact contract と label で明示された integration/readiness/qualification issue が所有する。

後続 integration failure はまずその gate owner に記録する。閉じた implementation issue を reopen するのは、その failure が
当該 issue の bounded acceptance を誤りと証明した場合、または当該 scope の regression を示した場合だけとする。
製品全体が未認定であること自体は reopen 理由にしない。

この amendment は final release qualification、`production-supported` の意味、fail-closed exact-SHA evidence、protected-main review を
弱めない。資格証拠を削除するのではなく、個別 issue から本来の integration/release owner へ移す。

## Consequences

- 同一 issue・同一 contract 内の通常実装と CI 根本修正は、診断報告後に重複承認なしで継続できる。
- supporting file の追加は scope drift として隠さず通知するが、可逆な unit 内変更だけなら会話停止を要求しない。
- ordinary issue は担当 scope の品質確認に集中し、製品全体の未認定だけで開いたままにならない。
- integration/release gate は clean exact-SHA の全体 evidence を一度だけ所有し、production claim の強度を維持する。
- destructive または外部影響を持つ操作、重大な意味決定、platform permission は standing authorization から明示的に除外される。
- goal 外の通常会話は従来どおり request scope に従い、この policy から追加 authority を得ない。
- personal skill と curated plugin cache は変更せず、repository contract 側で compatibility を定義する。
- protected-main workflow が execution contract と checker の両方に固定される。

## Verification

API-development readiness unit test は完全な policy を positive fixture として受理し、policy ID binding、platform carve-out、
fresh-approval tier、external-blocker rule、通常会話での non-activation の各欠落と direct-main 文言の再導入を個別に拒否する。
scenario review は PR CI の局所 fixture 修正が notify-and-continue であること、force-push・branch protection 変更・production deploy が
target/effect-specific fresh approval であること、通常の質問で standing authorization が起動しないことを反証する。

completion-policy review は ordinary issue が bounded evidence と tracked residual gap で close できること、production qualification を
claim しないこと、explicit integration/readiness/qualification issue だけが full/stress/Nightly/release evidence を所有すること、
integration failure の routing/reopen rule が決定論的であることを検証する。

変更 PR は exact-head required checks、review resolution を満たしてから merge する。merged-main integration evaluation と final
production qualification は対応する gate owner が継続し、ordinary implementation issue の close と混同しない。
