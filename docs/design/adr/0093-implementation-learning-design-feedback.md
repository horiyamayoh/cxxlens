# ADR 0093: 実装知見を非規範 record から authority へ段階的に還流する

- Status: Accepted
- Date: 2026-07-19
- Issue: #171
- Depends on: ADR 0088, ADR 0089, ADR 0092

## Context

cxxlens は統合設計、machine-readable contract、accepted ADR、GitHub issue、acceptance evidence を持ち、後続 ADR による
amendment と supersede も許している。しかし、実装を始めて初めて得られた反証、暗黙の前提、再利用可能なメンタルモデル、
より良い設計案を、正式決定の前に永続化する共通経路がなかった。

この空白を issue comment や担当エージェントの会話履歴だけで埋めると、知見が checkout から発見できず、別の実装で同じ調査を
繰り返す。一方、未検証の作業メモを normative design と同列に置くと、現在の契約を黙って迂回でき、agent ごとに異なる
authority が生まれる。

## Decision

実装知見を次の三層に分離する。

1. normative authority は統合設計、machine-readable contract、accepted ADR であり、明示的に置換されるまで実装を拘束する。
2. curated mental model は agent 向けの non-normative explanatory document とし、normative anchor、scope、known tension を明示する。
3. design feedback record は material な観察、working mental model、反証、alternatives、evidence、recommendation、disposition を保持する
   durable だが non-normative な記録とする。

design feedback は専用 GitHub issue を先に作成し、その issue 番号から `DF-0171` 形式の stable ID を導出する。record の metadata と
本文 shape は `schemas/cxxlens_ng_design_feedback_record.schema.yaml` と production checker が所有する。GitHub issue は investigation と
作業状態を追跡するが、issue comment だけを永久仕様にしない。record が `accepted` でも、それ自体は authority にならず、正式な変更は
`resolution_refs` が指す ADR、contract、catalog、test、traceability によってのみ成立する。

correctness、security、core invariant、compatibility、不可逆な public contract に対する反証は、解決するまで対象実装を
`blocked` とする。現行契約が健全で、提案が可逆な改善に留まる場合だけ `may-proceed` を許す。local かつ可逆な変更は自己レビューで
処理できるが、public contract、core invariant、security、compatibility、不可逆変更を採用する場合は、著者と異なる reviewer による
反証レビューを必要とする。review metadata は author/reviewer identity を分離し、tracking issue 上の具体的な review comment を参照して、
独立性と対象 record の binding を fail closed に検証する。review comment は canonical `horiyamayoh/cxxlens` repository の tracking
issue に属さなければならない。各 record は発見元 implementation issue を一つ以上列挙し、completion check から暗黙に除外される
空集合を許さない。authority reference は統合設計、accepted ADR、schema/catalog、明示的 execution contract に限定し、archive、
non-normative learning asset、implementation evidence を authority として扱わない。
high-risk impact の active record は `blocked` とし、high-risk acceptance の resolution は filename だけでなく accepted status を持つ
ADR に結び付ける。

全 implementation issue は終了時に learning checkpoint を行い、`none` または関連 DF ID を記録する。未解決の blocking record が
紐付く issue は completed にしない。採用した reusable insight は curated mental model へ反映し、却下、延期、置換も disposition と
参照先を残す。

## Consequences

- normative document は「参考」に格下げされず、反証可能だが置換まで拘束力を持つ現在の契約になる。
- agent は悪いと判断した設計へ黙って従うことも、記録なしに独自設計へ逸脱することもできない。
- raw insight と current mental model と accepted decision を別々に検索できる。
- open/deferred feedback は active tree に残るが、authority reading order を変更しない。
- high-risk design change は独立 reviewer を確保できなければ proposed/blocked のままとなる。
- readiness evidence は mental model、record、index、template、issue form を digest するが、authority と別の learning asset 集合に保持する。

## Verification

production checker は front matter schema、ID/issue binding、必須本文節、repo reference、状態別 resolution、deferred/superseded
binding、risk-proportional review の author/reviewer separation と tracking-comment binding、generated index、mental-model banner を
決定論的に検証する。YAML duplicate key、repository 外の local review ref、別 repository の review comment、non-normative または
archived authority ref は拒否する。`issue-ready` は implementation issue に
紐付く unresolved blocking record を拒否する。positive/negative unit test と `cxxlens-quality` は同じ checker を実行し、API development
readiness contract は handbook、schema、checker、issue template の exact path を検証する。
