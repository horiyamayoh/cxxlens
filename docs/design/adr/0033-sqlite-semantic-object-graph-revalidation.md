# ADR 0033: SQLite payload は persisted semantic object graph を open 時に再構成する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: store-kernel
- Decision issue: #90
- Depends on: ADR 0009, ADR 0013, ADR 0021

## Context

payload v4 は physical checksum、保存済み manifest からの snapshot ID、partition binding、closure subject を検証したが、
query が読む detached row、claim annotation、coverage から partition manifest を再構成しなかった。DB payload と checksum を
同時に改変すると、同じ snapshot ID のまま annotation row、condition、interpretation、producer、provenance、guarantee、
coverage を差し替えられた。duplicate snapshot 比較用 export も annotation/coverage を含まなかった。

## Decision

SQLite physical minor を 2.4.0、write payload を `cxxlens.ng-snapshot-payload.v5` とする。v5 は partition ごとに publish 時の
完全な claim envelope、coverage、unresolved を canonical order で保存する。

open と compaction の独立 validator は envelope から次を bottom-up に再構成する。

- claim の semantic key、assertion、content identity と row schema
- claim set、coverage、partition content、claim count、complete flag
- partition identity binding と closure certificate subject
- detached rows、query annotations、claim content list、coverage、unresolved projection

再構成結果と manifest/projection が byte-exact に一致しなければ publication を corrupt とし、current fallback と compaction を
禁止する。duplicate snapshot の canonical comparison は annotation、coverage、partition binding、完全 envelope を含む。
physical generation と root locator は semantic comparison に含めない。v1〜v4 は宣言済み legacy read policy を維持するが、v5
だけが完全 object-graph reconstruction guarantee を持つ。

## Consequences

- checksum を再計算した局所 BLOB 改変も open 時に検出される。
- row cursor と query annotation cursor が同じ persisted claim envelope に bind する。
- compaction は corrupt record を新 generation へ複製しない。
- snapshot identity と claim identity の既存 dependency graphは変更しない。

## Verification

`tests/unit/sdk/store_test.cpp` は実 SQLite payload と checksum を更新し、annotation/detached/envelope row、condition、
interpretation、producer、provenance、guarantee、coverage、claim-set/content digest、claim count の各改変が reopen で corrupt、
compaction で validation failure になることを検証する。intact payload の memory/SQLite ID/export parity と再配置後 reopen も維持する。
