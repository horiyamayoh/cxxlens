# ADR 0049: source.span identity を ingestion と Clang worker で共有する

- Status: Accepted
- Date: 2026-07-17
- Issue: #106

## Context

Clang native SDK は logical path、begin、end の独自3項組から span ID を作っていたが、accepted
`source.span.v1` は source snapshot、file、begin、end、role の5項組を authority とする。独自 ID は base
snapshot の `source.span` と一致せず、`cc.call_site.source` の hard reference を解決できない。

## Decision

LLVM-free SDK に `source_span_identity(snapshot,file,begin,end,role)` を定義し、source ingestion、native SDK、worker
が共有する。domain は `source-span`、encoding は `cxxlens-canonical-tuple-v1` とする。logical path は表示／Clang
file lookup 用であり identity に含めない。

`translation_unit_input` と worker task v2 は exact source snapshot ID と file ID を必須で運ぶ。range role は
`normalize_source` 呼び出しごとに明示し、declaration と expression を区別する。欠落 identity、逆転 range、
表現不能 offset は structured error で fail closed とする。detached span validator は ID を5項組から再計算する。

## Consequences

source bytes/snapshot、file identity、range role の変更は span ID を変更し、logical-root relocation は file identity が
同じなら ID を変更しない。worker call/entity reference は base source relationと同じ ID authorityを使用する。

## Verification

Clang normalizer adapter test は source relation builder parity、snapshot/file/role mutation、root relocation、task v2
round-trip、missing authority rejection を検証する。SDK catalog/quality gate は shared functionとworker v2 fieldsを固定する。
