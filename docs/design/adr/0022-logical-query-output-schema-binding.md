# ADR 0022: Logical Query IR の output schema は typed node shape に exact bind する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: logical-query / query-runtime
- Decision issue: #79
- Depends on: ADR 0003, ADR 0007, ADR 0014, ADR 0018

## Context

`logical_query_ir::validate()` は各 node の shape を column ID の集合としてだけ伝播していた。最終
`output_schema` も relation requirement に descriptor ID があることと column ID 集合だけを照合したため、public IR
を直接構築する caller は scalar kind、typed ID parameter、optionality を改変しても validation を通過できた。
descriptor ID と別 descriptor の column ID を組み合わせることもでき、IR digest が宣言する型と executor の cell 型が
一致しなかった。

## Decision

validator の node shape を `column ID -> exact value_type` の canonical map とする。

- scan は validated relation descriptor の各 column ID と exact type を shape に入れる。
- filter/restriction/distinct/order/limit は typed shape を保持する。
- inner/semi join は typed input shape を規則どおり合成する。
- union は column ID だけでなく exact type を含む shape equality を要求する。
- project は各 source column の type を output alias へ伝播し、重複 alias または source 不在を拒否する。

最終 `output_schema` の各 `column_ref` は、指定 descriptor 自身に同じ column ID が存在し、scalar、parameter、
optionality を含む `value_type` が完全一致しなければならない。その typed output map と root typed shape も完全一致させ、
重複 output column を拒否する。違反は `sdk.query-output-schema-mismatch`、未知 descriptor は従来どおり
`sdk.query-output-column-foreign` とする。

execution boundary では、advertised output column の type を published snapshot descriptor の同一 column と再照合する。
compatible minor は未参照の additive optional column の欠落を許すが、output として参照する column の欠落や型変更を
許さない。自己整合した外部 IR であっても snapshot の cell type と違えば
`sdk.query-snapshot-schema-mismatch` とする。

## Consequences

- Logical Query IR digest が宣言する result type と executor が返す cell type は一致する。
- typed/dynamic builder の既存 IR と digest は変わらない。
- descriptor ID、column ID、type の一部だけを差し替えて builder validation を迂回できない。
- node shape は ordered map であり、unordered iteration は validation や identity に影響しない。

## Verification

`tests/unit/sdk/query_runtime_test.cpp` は unprojected output の scalar、typed ID parameter、optionality、
descriptor/column pair の各改変と、projected output type 改変を個別に拒否する。builder 生成 scan/project は引き続き
validate/execute できる。さらに relation requirement と output schema を同じ不正型へ揃えた自己整合 IR を作り、実
snapshot descriptor との照合で execution が `sdk.query-snapshot-schema-mismatch` になることを検証する。
