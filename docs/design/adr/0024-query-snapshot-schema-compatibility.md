# ADR 0024: query execution bind で参照 column の snapshot schema を完全照合する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: logical-query / query-runtime
- Decision issue: #81
- Depends on: ADR 0007, ADR 0014, ADR 0022, ADR 0023

## Context

reference executor は relation requirement と snapshot descriptor の `semantic_major` および minor 下限だけを比較して
いた。IR が参照する column の欠落や型の変化を照合しないため、`cell_for()` が query requirement の型を使って
required column にも tagged `absent` を合成できた。その結果、破壊的 schema drift が通常の semantic absence として
filter、project、order、join、output に伝播した。

## Decision

validated logical IR を実行する前に typed schema compatibility plan を構成する。predicate の再帰 operand、project、
order、join predicate、および implicit terminal project の output schema が参照する全 column を、所有する relation
requirement と availability とともに収集する。同じ column を複数箇所が参照した場合は `require` を優先する。

bind は各参照を snapshot descriptor と照合する。`require` は column の存在を必須とし、存在する column は scalar、
parameter、optionality、required flag、role が query requirement と完全一致しなければ
`sdk.query-snapshot-schema-mismatch` を返す。semantic major は一致、snapshot minor は requirement minor 以上を要求する。

欠落を許可するのは、参照が明示的な `absent_if_schema_missing` で、query descriptor の column が
`required=false` かつ optional type である場合だけである。この場合に限り query descriptor の exact type を持つ tagged
`absent` を合成する。通常参照、required column、型や role の変化を semantic absence に変換しない。未参照の additive
optional column は実行互換性に影響しない。

## Consequences

- minor 番号だけでは破壊された required shape を隠せない。
- `is_absent` は schema 欠落をデータ absence と誤認しない。
- explicit/implicit projection と全 operator reference が同じ fail-closed bind 規則を共有する。
- memory/SQLite や producer の違いによって同じ IR digest の型解釈が変化しない。

## Verification

`tests/unit/sdk/query_runtime_test.cpp` は required column 欠落（snapshot minor が大きい場合を含む）、typed ID parameter
不一致、optional column の通常参照を拒否する。明示 `absent_if_schema_missing` は exact optional type の tagged absent を
返し、未参照 optional column を追加した compatible minor は実行に成功する。
