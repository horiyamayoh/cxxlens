# ADR 0034: closure validator は完全な immutable partition subject を要求する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: store-kernel
- Decision issue: #91
- Depends on: ADR 0009, ADR 0021, ADR 0033

## Context

`make_closure_certificate(partition_manifest, closure_candidate)` は relation、partition/content、coverage の4 field だけを
subject と照合できた。partition manifest は condition、interpretation、assumption set、producer semantics を保持しないため、
standalone API は別 domain の candidate に正規 closure ID を発行できた。writer だけが手元の draft を使って追加検証し、公開
validator と accept/reject 集合が異なっていた。

## Decision

`partition_certificate_subject` を immutable value として導入する。この型は `partition_manifest` と
`snapshot_partition_binding` を所有する。`make_partition_certificate_subject()` は次を独立検証してから subject を生成する。

- relation、partition ID、producer input basis が manifest/binding で exact match
- binding 全 field から partition ID を再計算
- partition ID、claim-set digest、coverage digest から partition content digest を再計算
- condition、scope、interpretation、producer semantics、precision、assumption の局所 validity

`make_closure_certificate()` は完全 subject だけを受け取り、candidate の relation、partition content、coverage、condition、
interpretation、assumption set、producer semantics を exact match する。key-domain と evidence は digest、NG0 closure kind は
`relation-key-enumeration` のみを許可する。不完全な manifest-only overload は削除する。standalone、writer、SQLite reopen は
すべて同じ二段 validator を使用する。

## Consequences

- validator success は closure identity の全 partition-derived field が subject と一致することを意味する。
- 別 condition/interpretation/assumption/producer の forged certificate は ID 発行前に拒否される。
- payload v4/v5 の persisted binding と certificate body も同じ public validator で再検証される。
- key-domain/evidence の値は certificate 固有だが、型/形式と closure kind の許可規則は subject contract に固定される。

## Verification

`tests/unit/sdk/store_test.cpp` は exact candidate の deterministic ID と writer parity、condition、interpretation、assumption、
producer、key-domain、closure kind、evidence の各 mismatch に対する standalone/writer の同一 reject 集合を検証する。
