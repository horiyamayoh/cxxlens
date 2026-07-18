# ADR 0088: Foundation の GitHub blocker は manifest 宣言集合に限定する

- Status: Accepted
- Date: 2026-07-18
- Issue: #165
- Supersedes: ADR 0056 の current GitHub open issue scope

## Context

ADR 0056 は zero audit の literal 値を finding-bound subreport に置き換え、未検査の migration blocker を見逃さないために
repository の全 open issue を Foundation blocker とした。しかし Foundation completion は G0–G4 と移行完了の認定であり、G5 と GR を
要求する distribution 1.0 qualification から分離されている。全 open issue を問い合わせると、Foundation 完了後に作成した G5、GR、
roadmap、通常の継続開発 issue が過去の認定を遡及的に失敗させ、main CI で将来作業を追跡できない。

completion manifest には Foundation 固有の `required_closed_issues`、`authority.gate_issue`、`authority.tracking_issue` が既にあり、
commit-bound report が検証すべき exact issue set を表現できる。

## Decision

Foundation の GitHub blocker authority は、completion manifest が宣言する次の和集合とする。

- `required_closed_issues`
- `authority.gate_issue`
- `authority.tracking_issue`

completion checker はこの集合だけを GitHub API から取得し、各 response が issue かつ state が `open` または `closed` であることを
検証する。network/API/JSON/state failure、宣言 issue の欠落、`closed` 以外の state は fail closed とする。audit subreport の canonical
target set と digest にも同じ宣言集合だけを使用する。本決定を実装する #165 自身を `required_closed_issues` に含める。

宣言集合にない open issue は Foundation audit から除外する。これは blocker の黙殺ではない。新しく判明した Foundation 固有 blocker は
認定前に manifest へ明示的に追加し、G5、GR、roadmap、通常の継続開発 issue はそれぞれの acceptance gate または tracking contract で
扱う。Foundation completion から distribution 1.0 や production support を主張してはならない。

## Consequences

- Foundation の認定は、その後の通常の issue 作成によって遡及的に無効化されない。
- Foundation blocker の追加は manifest/schema/checksum/traceability の reviewable change になる。
- G5 と GR は Foundation issue を再利用せず、独立した owner/contract issue を持つ。
- ADR 0056 の measured subreport、revision/tree binding、zero finding、fail-closed 原則は維持し、全-open scope の決定だけを置き換える。

## Verification

required、gate、tracking issue の open/missing を拒否し、宣言集合外の synthetic open issue が audit target と finding に入らない回帰試験を
維持する。deferred G5/GR が実装済み issue contract を再利用していないことも静的検証する。
