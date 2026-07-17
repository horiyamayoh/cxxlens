# ADR 0056: foundation zero audit は finding-bound subreport からのみ証明する

- Status: Accepted
- Date: 2026-07-17
- Issue: #113

## Context

foundation completion report は九つの audit 名が manifest の固定集合と一致することだけを検査し、各値を literal `0` として出力して
いた。GitHub issue も過去の固定番号だけを問い合わせたため、新規 blocker、authority ownership の欠落、documentation checksum drift
が存在しても passed artifact を生成できた。commit/tree binding は audit を実行した証拠や finding 集合を結合していなかった。

## Decision

専用 `check_ng_foundation_audits.py` は九つの audit を実測し、checker ID、canonical target set、その digest、finding IDs、count、
revision/tree を `cxxlens.ng-foundation-audit-report.v1` として出力する。各 migration surface、current GitHub open issue、authority
ownership、design package checksum を個別 checker identity で測定する。current blocker set は repository の全 open issue から取得し、
API/network/JSON failure は authority set を空とみなさず失敗させる。

completion checker は checker を別 process で実行し、subreport schema、report と entry の revision/tree 一致、
`count == len(finding_ids)` を独立に検証する。finding が一件でもあれば passed report を作らない。completion report の zero audit
entry は同じ再現情報を保持し、schema も count 0 / empty findings を要求する。既に完了した gate/tracking issue は closed を必須とする。

## Verification

clean fixture の九 audit が計算された zero と target digest を持つことに加え、synthetic open issue、unowned authority、checksum drift、
不正 subreport schema、revision/tree mismatch、count/finding mismatch をそれぞれ拒否する回帰試験を維持する。
