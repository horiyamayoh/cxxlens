# ADR 0066: query empty summary は zero-row proof から構成する

- Status: Accepted
- Date: 2026-07-18
- Decision owner: query-runtime
- Decision issue: #135
- Depends on: ADR 0008, ADR 0014, ADR 0021, ADR 0060, ADR 0065

## Context

ADR 0060 は summary を最終 result に寄与した fragment の conservative meet としたが、実装は最終 row がゼロの場合に限り
scan 済みの全 annotation を fallback として再集約していた。filter、condition restriction、interpretation restriction、join、
semi join が全 row を除外すると、選択外の guarantee、condition、interpretation、assumption が summary に復活した。この fragment
は zero-row の根拠ではなく scan した事実を表し、空と非空の境界で operator composition law を不連続にした。

## Decision

空結果 summary は source annotation を再利用しない。Logical Query IR を root から scan へたどり、condition restriction と
interpretation restriction を伝播した canonical zero-row proof context を生成する。filter / join / semi join の個々の不一致 claim は
result contributor にせず、選択 restriction がない軸は synthetic `query-empty` partition とする。

input と execution が complete で、全 reachable input range に applicable relation-key-enumeration closure が揃い、blocking unresolved、
truncation、limit がない場合だけ、zero-row proof を `exact / query-empty / assumptions:none` として closure ID に束縛する。それ以外は
同じ選択 condition / interpretation context を保持した `unknown / query-empty / assumptions:unknown` fragment に保守化する。
failed-before-result、zero-row truncation、cancellation は scan annotation から exact または under-approximation へ昇格しない。

## Verification

全 row を除外する filter が source guarantee を復活させないこと、空の condition / interpretation restriction が選択 partition だけを
公開すること、open-world absence は unknown、closure が全入力に揃う complete absence だけが exact になることを runtime test で
検証する。zero scan budget と zero output budget、最後の一 row を除外する非空/空境界、filtered-out weak empty conformance vector を
固定し、memory / SQLite と canonical fragment-set digest の決定性を既存 backend matrix で維持する。
