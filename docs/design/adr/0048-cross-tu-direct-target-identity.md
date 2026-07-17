# ADR 0048: direct target identity を同一 batch entity presence から分離する

- Status: Accepted
- Date: 2026-07-17
- Issue: #105

## Context

Clang の `CallExpr::getDirectCallee()` は header または別 translation unit の宣言にも canonical declaration key を
返す。一方、normalizer は同じ observation batch で生成された entity row の map に key がある場合だけ
`cc.call_direct_target` を生成していた。main-file entity extraction と compile-unit partitioning のため、通常の
cross-TU direct call が provider-local availability に依存して欠落していた。

## Decision

entity row と direct target は、callee semantic key、toolchain context、build variant を入力とする同じ
`entity_identity` projection を使用する。`call.direct_callee` があれば entity row の同一 batch 存在に関係なく
`cc.call_direct_target` を生成する。

target entity が未publicationでも direct row は保持する。`cc.call_direct_target.target` は relation registry で
`soft_semantic` / `on_missing: unresolved` と定義済みであり、claim/store の reference accounting が availability を
扱う。`call.direct_callee` が存在しない場合だけ provider は `provider.direct-target-unresolved` を出す。

## Consequences

header declaration、forward declaration、external declaration、別 TU definition への direct call は同じ
toolchain/variant で同一 target identity を得る。compile-unit、observation order、provider execution order は target
identity に影響しない。indirect call から target を捏造しない。

## Verification

Clang normalizer adapter test は same-TU、call-only cross-TU/external、observation reorder、no-direct-callee を検証する。
R2 vertical slice は entity batch と別 compile-unit の call-only batch を結合し、flagship recipe が cross-TU call を
返すことを検証する。
