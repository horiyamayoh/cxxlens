# ADR 0032: `set<T>` literal は byte-backed scalar codec で lossless に decode する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: logical-query
- Decision issue: #89
- Depends on: ADR 0007, ADR 0027

## Context

`scalar_kind::set` の runtime value は `std::vector<std::byte>` であり、typed query builder はその canonical bytes を lowercase
hex JSON string として Logical Query IR に encode する。しかし argument decoder は `bytes` だけを hex decode し、`set<T>` は
`std::string` のまま保持していた。executor が type を `set` に戻しても value variant が異なるため、同じ set cell に対する
`equals_present` は常に false になった。

## Decision

typed literal codec は scalar kind の storage category を保存する。

- `bytes` と `set` / `set<T>` は byte-backed category とし、同じ lowercase hex decoder を使用する。
- empty string は empty byte sequence として受理する。
- odd-length、uppercase、non-hex encoding は canonical set encoding ではないため拒否する。
- `set<T>` の完全な canonical type string と nested parameter `T` を保持する。
- predicate validation は column の scalar kind と parameter の完全一致を要求し、mismatch を拒否する。
- decoded byte vector を executor まで保持し、string comparison への fallback は行わない。

## Consequences

- static builder IR の encode/decode は set value variant を lossless に保存する。
- set column の `equals_present` は同じ canonical bytes にだけ一致する。
- malformed dynamic IR と異なる set parameter は validation 境界で fail closed になる。
- `bytes` literal の既存 encoding と受理集合は変わらない。

## Verification

`tests/unit/sdk/query_runtime_test.cpp` は nonempty/empty set literal round-trip、nested type parameter と byte vector の保持、
matching/non-matching executor result、odd-length/uppercase hex の reject、set parameter mismatch の reject を検証する。
