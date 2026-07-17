# ADR 0051: Clang redeclaration chain を一つの entity observation に正規化する

- Status: Accepted
- Date: 2026-07-17
- Issue: #108

## Context

Clang AST は forward declaration、repeated declaration、definition を個別 `FunctionDecl` として訪問する。worker は
source span を observation dedup key に含めていたため同じ canonical declaration key の全 occurrence を保持し、同じ
entity ID に異なる anchor と structural signature を持つ functional rows を生成していた。

## Decision

worker は entity observation を canonical declaration key ごとに group 化し、standard `cc.entity` には definition、
canonical declaration、canonical observation form の順で deterministic に一件を選ぶ。provider-owned observation row は
occurrence evidence として保持する。compatible 判定は entity kind と canonical type/signature
だけを用い、source anchor、compile unit、arrival order、display name を structural signature に混ぜない。

extractor は `symbol.is_definition` と `symbol.is_canonical_declaration` を detached observation に記録する。direct callee
projection も definition があれば definition、なければ canonical declaration を使用し、same-batch entity row の有無で
target identity を変えない。compatible redeclaration は一 entity rowへ縮約する。kind/signature が不整合な同一 key は
`provider.entity-redeclaration-incompatible` unresolved と equivalence limitation を残し、exact と主張しない。

## Consequences

forward declaration と definition、compatible repeated declaration は functional conflict を作らない。definition anchor を
優先し、definition がない chain は canonical declaration anchor を使う。overload は異なる canonical key のため別 entity
として保持する。provider observation を越えて occurrence を標準表現する将来 provider は `cc.declaration` を使い、
`cc.entity` に複数 anchor を詰めない。

## Verification

Clang normalizer test は declaration+definition、repeated declaration、definition-less chain、arrival-order reversal、anchor
movement、overload、incompatible signature を検証する。quality gate は definition/canonical markers と explicit unresolved
reason を worker sourceに固定する。
