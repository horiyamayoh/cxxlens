# ADR 0027: node arguments は typed decode 後に canonical JSON へ再 encode する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: logical-query
- Decision issue: #84
- Depends on: ADR 0007, ADR 0022, ADR 0026

## Context

`decode_arguments()` は operator ごとの exact JSON object を typed variant へ変換していたが、Logical IR canonical form は
original `ir_node::arguments` text をそのまま埋め込んでいた。そのため member order、token 間 whitespace、Unicode/solidus
escape など、decoder 後の値が同じ表記差で digest が分岐した。

## Decision

recursive node canonicalization は raw argument text を使用しない。各 node を `decode_arguments()` で
`operator_arguments` variant に変換し、operator variant ごとの単一 encoder で canonical JSON object を生成する。

encoder は object key を UTF-8 code-unit lexical order、string/control escape と integerを単一表現にし、column
availability、typed literal scalar、order cell states を明示 tag から生成する。projection/order sequence は意味的順序を
保持する。condition alternatives は decoder で sort/dedup し、`and` / `or` operand は再帰 canonical child digest と
canonical text の tie-break で sort して exact duplicate を除く。union input の既存 canonical child ordering は維持する。

leading/trailing/token whitespace を含む noncanonical JSON は exact typed shape なら受理し、decode→normalize する。unknown
key、duplicate object key、invalid escape、型違いは従来どおり fail closed である。canonical form と digest は validation
成功 IR にだけ semantic authority を持つ。

## Consequences

- static builder、dynamic deserialize、外部 serializer が同じ typed IR を表せば同じ digest になる。
- JSON表記差を cache、署名、監査 identity に持ち込まない。
- condition と commutative predicate の set semantics が runtime canonicalization にも適用される。
- typed argument が実際に異なる場合は canonical JSON と digest が変わる。

## Verification

`tests/unit/sdk/query_runtime_test.cpp` は scan member order/whitespace/Unicode escape、interpretation solidus escape、condition
alternative order/duplicate、commutative predicate order/duplicateを変えても canonical form/digest が一致することを検証
する。interpretation typed value を変更した場合は digest が変わり、canonical form の反復 encoding は byte-identical で
ある。
