# ADR 0047: recipe state を query execution completeness へ bind する

- Status: Accepted
- Date: 2026-07-17
- Issue: #104

## Context

`calls_to_function` は query result の row と input coverage だけから state を分類していた。このため、
execution 前 cancellation は complete input 上で `empty_complete` となり、output truncation や partial
cancellation は返された canonical prefix だけから `matched` または `ambiguous` となり得た。これは
`INV-PARTIAL-001` と、query execution、input completeness、closure を独立軸として保持する契約に反する。

## Decision

`call_search_state` に `partial` と `failed` を追加し、recipe semantics を
`cxxlens.recipes.calls_to_function-1.1.0` とする。

- `query::execution_status::complete` の場合だけ row と input coverage から `matched`、
  `empty_complete`、`empty_incomplete`、`ambiguous` を分類する。
- `truncated` と `cancelled_with_partial` は常に `partial` とする。
- `failed_before_result` は常に `failed` とする。
- partial row から match の確定、ambiguity 不在、absence を推論しない。
- report は元の query result を引き続き所有し、exact execution status、coverage、unresolved、evidence を保持する。

## Consequences

既存の complete execution の分類は変わらない。runtime budget または cancellation を使用する consumer は、
部分結果を確定結果として扱えず、state と `query_result::execution()` の双方から同じ completeness boundary を
確認できる。enum 追加と semantics 修正は recipe axis の minor version として追跡する。

## Verification

`tests/integration/r2_vertical_slice_test.cpp` は execution 前 cancellation、scan/intermediate budget failure、
ambiguous result の output truncation、sealed prefix 後 cancellation、および全 complete state を検証する。
SDK quality gate は public catalog owner、全 execution status branch、`partial`/`failed` surface を検査する。
