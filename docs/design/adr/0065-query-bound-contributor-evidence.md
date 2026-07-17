# ADR 0065: query contributor evidence は canonical edge として束縛する

- Status: Accepted
- Date: 2026-07-18
- Decision owner: query-runtime
- Decision issue: #134
- Depends on: ADR 0008, ADR 0014, ADR 0037, ADR 0059, ADR 0060

## Context

従来の `annotated_row` は claim contributor、producer contract、provenance、contributor guarantee を独立した
canonical set として保持していた。scan の直後は各集合が一つの claim に由来するため対応を推測できるが、join、semi join、
distinct で集合を個別に union すると、どの producer / provenance / guarantee がどの claim を支えたかを復元できない。
summary fragment は row 全体の集合と各 guarantee を組み合わせるため、false attribution の直積を生成し、無関係な claim の
conflict または unresolved を別の guarantee fragment へ伝播させていた。

## Decision

`query_contributor_edge` を row annotation の正本とし、claim contributor、producer、provenance、guarantee、condition、
interpretation を一つの canonical tuple に束縛する。従来の contributor / producer / provenance / guarantee 集合は互換性と
集約参照のため残すが、edge からのみ導出し、validation は projection の一致を要求する。

scan は observation ごとに一 edge を生成する。inner join は左右の edge を出力 condition intersection へ束縛して union し、
semi join は各 compatible witness の bound edge を materialized witness vector なしに累積する。union と distinct は edge 自体を
canonical union し、filter、project、condition / interpretation restriction は surviving edge だけを保持する。row canonical form と
memory accounting は edge set を含む。

`query_summary_guarantee` は各 bound edge から singleton contributor / producer / provenance の fragment を生成する。
conflict、unresolved、closure applicability はこの singleton attribution に対して評価し、row 全体の独立集合との直積を生成しない。
fragment canonical JSON と fragment-set digest は producer binding も含める。

## Verification

異なる guarantee / producer / provenance を持つ二 claim の join で二つの元 tuple を完全復元できること、片側だけの conflict と
unresolved が他方へ伝播しないこと、複数 witness の semi join と distinct merge が edge を lossless に保持することを runtime
test で固定する。execution-result schema は row edge と fragment producer binding を required にし、schema encode/decode と
canonical JSON round trip、memory / SQLite parity、fragment-set digest parity を acceptance で検証する。
