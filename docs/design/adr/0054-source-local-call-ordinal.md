# ADR 0054: call ordinal は source-local equivalence class 内で決める

- Status: Accepted
- Date: 2026-07-17
- Issue: #111

## Context

Clang normalizer は batch 全体の observation 配列位置を `cc.call_site.ordinal` に使用していた。permutation、無関係な call の
追加、entity/type observation の挿入で既存 call ID が変わり、incremental snapshot と cache を不必要に無効化した。

## Decision

call occurrence class は compile unit、canonical source span、call kind、caller の canonical tuple とする。通常の一意な
occurrence の ordinal は常に0とし、同じ class に複数 observation がある場合だけ canonical observation form の順で
0..N-1 を割り当てる。batch 全体の先行要素数は使用しない。

normalizer は entity/type/call observation を自ら canonical sort する。call は occurrence class、canonical form の順、entity は
canonical declaration group、type/raw observation は canonical form 順で出力し、input permutation から独立した byte sequence を
返す。accepted `cc.call_site` domain projection の ordinal はこの source-local tie-break の意味に固定する。

## Consequences

無関係な source occurrence の追加削除、非 call observation の挿入、jobs/batch collection order は既存 call ID を変えない。
same-span duplicate だけが local canonical ordinal を持ち、その class 内の変化だけが影響する。

## Verification

normalizer test は observation permutation の canonical batch byte equality、無関係な先行 call 追加後の既存 ID、entity/type
position independence、same-span duplicate tie-break、descriptor projection による ID 再計算を検証する。
