# ADR 0058: query column identity は scan occurrence で修飾する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: logical-query / query-runtime
- Decision issue: #115
- Depends on: ADR 0007, ADR 0014, ADR 0022, ADR 0023

## Context

`query.scan.v1` は alias を canonical IR と digest に保持していたが、predicate、node shape、runtime row は stable
column ID だけを key にしていた。同じ relation を二度 scan すると左右の列が衝突し、`combine()` は値が異なる pair を
捨て、同値なら一列へ collapse した。このため parent-child self-join を表現できず、alias は execution semantics を持たない
dead field になっていた。

## Decision

relation descriptor の column ID は schema authority、scan alias は query-local occurrence identity として分離する。
Logical Query IR の全 `column_ref` は `source_alias`、stable `column_id`、`availability` を持つ。validated node shape と
reference runtime の intermediate row key は `(source_alias, stable column ID)` とし、`output.*` への rename は明示または
implicit terminal projection でだけ行う。

typed/dynamic builder は単一 occurrence の従来の unqualified `column_ref` を一意に bind して canonical IR へ alias を記録する。
同じ stable column が複数 occurrence にある場合は `query::qualify()` による明示 bind を要求し、unqualified reference は
`sdk.query-column-ambiguous`、validated IR の missing occurrence は `sdk.query-column-occurrence-missing` とする。alias は
semantic field であり、変更すれば canonical IR digest も変わる。

duplicate leaf output は source alias prefix を使う。したがって `left.key` と `right.key` はそれぞれ
`output.left_key` と `output.right_key` になり、stable ID が同じでも cell、condition、contributors、provenance を lossless に
保持する。snapshot schema bind は従来どおり stable descriptor column authority に対して行い、query-local alias を schema
identity に混入させない。

## Verification

same relation の `left.parent == right.key` self-join で `key:b -> key:a` と `key:c -> key:a` だけを返し、左右の key cell を
別 output に保持する。qualified filter、order、project、unqualified ambiguity、alias による digest 差を同じ regression で固定する。
memory と SQLite snapshot の canonical rows を比較し、different-relation join の既存 matrix も維持する。
