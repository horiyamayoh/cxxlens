# ADR 0023: canonical implicit terminal project を reference executor でも実行する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: logical-query / query-runtime
- Decision issue: #80
- Depends on: ADR 0007, ADR 0014, ADR 0022

## Context

builder の scan、filter、join、order、limit、condition/interpretation restriction は、明示 `.project()` がない場合に
source stable column ID を持つ node graph を生成する。`logical_query_ir::canonical_form()` はこの graph を canonical
terminal `query.project.v1` で包み、`output.*` schema とその型を IR digest に含めていた。一方 reference executor は
格納された node だけを実行したため、同じ digest の runtime row は source column ID のままだった。明示 project の
有無で意味的に同じ query の result shape が変わっていた。

## Decision

canonical form の既存規則を authority とし、明示 `query.project.v1` node が一つもない validated IR に対して reference
executor が同じ implicit terminal projection を適用する。

alias 計算は `src/sdk/query_internal.hpp` の単一 `output_aliases()` 実装を canonicalizer と executor で共有する。各
`output_schema` source column は `output.<alias>` へ rename し、terminal projection は row values だけを変更して
multiplicity、condition、interpretation、contributors、producer contracts、provenance、guarantee を保持する。重複する
短縮 column name は stable column ID の `.` / `-` を `_` に置換し、さらに衝突する場合は canonical numeric suffix を
付ける。

implicit projection は root evaluation 後、unordered canonical sort、output budget、cancellation publication より前に
実行する。ordered query の order key も同じ alias へ写像する。明示 project が存在する IR は node executor の既存
projection だけを使用し、二重 projection しない。

## Consequences

- canonical IR、digest、logical explanation、in-process runtime row の column ID が一致する。
- equivalent implicit/explicit project は同じ digest と canonical rows を生成する。
- memory/SQLite backend と remote canonical IR executor が一つの result shape contract を共有できる。
- physical explanation は implicit terminal strategy を診断できるが、logical identity は変わらない。

## Verification

`tests/unit/sdk/query_runtime_test.cpp` は scan-only と filter/order/limit/condition/interpretation restriction の各 row key が
すべて `output.*` で、logical explanation の output schema ID に存在することを検証する。全 left columns の明示
project と scan の暗黙 project は digest と全 canonical row が一致する。join は左右の `key` 衝突を
`output.key` と `output.company_query_right_v1_key` に解決し、全 row の key set を固定する。
