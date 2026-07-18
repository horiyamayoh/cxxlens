# ADR 0084: macro call occurrence discriminator を spelling origin から導出する

- Status: Accepted
- Date: 2026-07-18
- Issue: #153

## Context

macro body内の複数 `CallExpr` は、同じ invocation へ展開されると同じ expansion `source_span_id` を持つ。同じ caller と
direct calleeなら call semantic key も同じになる。Clang worker は各 token の spelling range を ordered
`source_origin_chain` として保持していたが、native observation の dedup key は kind、semantic key、primary span だけだったため、
同一展開内の2件目以降を canonicalization 前に破棄した。

global AST traversal ordinal をそのまま identity にすると、unrelated declaration/call や visitor implementation の変更が既存 ID を
変える。public source span を spelling locationへ戻すと macro expansion occurrence semantics を壊す。

## Decision

pre-normalization dedup は versioned canonical observation fieldsから `observation_dedup_key()` を導出し、kind、semantic key、
expansion source span、ordered source origin chainを長さ付き canonical encodingで bindする。同じ expansion/source/calleeでも
spelling occurrenceが異なれば別 observation として保持する。完全に同じ origin chain の重複 observationだけを dedupする。

最終 `cc.call_site` identity は従来どおり public expansion span と source-local ordinalを使う。ordinal は同じ
compile-unit/source/kind/caller class内で canonical observation formをsortして付与するため、source origin chain が occurrence
discriminatorになる一方、入力順やglobal traversal順には依存しない。origin path/offsetそのものは最終 IDへ入らず、同一展開内の
spelling orderが保存される whitespace/comment shift や checkout root relocation では call IDsを維持する。

## Consequences

- `TWICE()` 1展開は同じcalleeへの2 callを保持し、distinct call IDsを得る。
- 2展開では expansion spanごとにstable ordinalが付与され、合計4 callを保持する。
- 同一展開内の異なるcalleeも各1件を保持する。
- observation input permutationは canonical batch と call IDsを変えない。
- provider-owned origin chain は evidenceとして保持され、public source span は expansion siteのままである。

## Verification

Clang normalizer test は dedup key差、1展開2件、2展開4件、異callee、spelling offset shift、checkout relocation、入力反転を検証する。
provider quality checker は visitorが共有 `observation_dedup_key()` を使用し、same-expansion regression markersが存在することを固定する。
