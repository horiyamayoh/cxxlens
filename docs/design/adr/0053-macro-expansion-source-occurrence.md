# ADR 0053: macro call occurrence は expansion span を authority にする

- Status: Accepted
- Date: 2026-07-17
- Issue: #110

## Context

Clang native source normalization は range の両端を常に ultimate spelling location へ変換していた。同じ macro definition を
同じ function 内で複数回展開すると、caller、callee、spelling span がすべて一致し、call observation dedup が実在する occurrence を
一件へ縮約した。header macro の spelling path が caller snapshot の span authority と混同される問題もあった。

## Decision

`normalize_source()` の primary half-open span は `SourceManager::getExpansionRange()` が返す ultimate expansion range とし、
caller task が渡した exact source snapshot/file authority と組み合わせる。logical path は display/evidence に限定し、span ID には
含めない。macro location は primary span を read-only として明示し、macro token への直接 edit を認可しない。

macro の spelling/origin は ordered `detached_source_origin` chain として primary span から分離する。各 layer は
`getImmediateExpansionRange()` を外側へ辿り、その layer の spelling file、half-open offsets、read-only state を保持する。
origin logical path は host absolute pathを含み得る evidence であり、standard span/call identity authority に使用しない。
provider-owned observation row は canonical origin chain bytes を保持する。authoritative snapshot/file binding のない header origin を
standard `source.origin` として捏造せず、将来の source provider が標準化できる情報を lossless に残す。

## Consequences

同一 macro definition の異なる expansion offsets は異なる call occurrence/ID になる。nested expansion の順序と spelling range は
provider observation に残り、root relocation で origin display path が変わっても standard span/call ID は変わらない。
macro なし range の既存 half-open token semantics は維持される。

## Verification

normalizer test は同じ origin chain を持つ異なる expansion offsets が二 call/two direct targets になること、nested chain bytes、
origin path relocation identity invariance、macro origin read-only validation、non-macro half-open identity を検証する。native branch は
Exact Clang build と利用可能な Clang header に対する構文コンパイルで expansion API を確認する。
