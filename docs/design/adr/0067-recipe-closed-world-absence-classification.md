# ADR 0067: recipe empty_complete は closed-world absence proof を要求する

- Status: Accepted
- Date: 2026-07-18
- Decision owner: recipes
- Decision issue: #136
- Depends on: ADR 0021, ADR 0047, ADR 0060, ADR 0066

## Context

`calls_to_function` recipe は complete execution の zero row を input coverage だけで `empty_complete` に分類していた。
coverage completeness は観測対象を処理したことを表すが、未観測の call が存在しない closed-world proof ではない。query result は
execution、inputs completeness、applicable closure、summary guarantee を独立に公開しており、recipe の absence state だけがこの境界を
失っていた。

## Decision

recipe semantics を `cxxlens.recipes.calls_to_function-1.2.0` とする。`empty_complete` は次をすべて満たす場合だけ返す。

- execution status が `complete`
- result row がゼロ
- `inputs_complete()` が true
- `closed()` が true
- query が適用した `closure_ids()` が nonempty
- `summary_guarantee().approximation` が `exact`

closure applicability の relation、condition、interpretation、partition content、producer semantics の判定は query runtime の共有 verdict を
使用し、recipe 独自の certificate 探索を行わない。条件を満たさない complete zero-row result は `empty_incomplete` とする。
実在 row に基づく `matched` と `ambiguous` は existential witness なので input completeness や closure を要求しない。
`truncated` / `cancelled_with_partial` は `partial`、`failed_before_result` は `failed` という ADR 0047 の境界を維持する。

## Verification

complete coverage だが closure なしの zero row、unrelated relation closure だけの zero row は `empty_incomplete` とし、全 required relation に
applicable closure が揃い query summary が exact の場合だけ `empty_complete` とする。found row は closure なしでも `matched` / `ambiguous`
を維持し、truncated、cancelled、failed は closure の有無で確定 state へ昇格しない。integration matrix は recipe state と query
`closed()` / closure IDs / summary approximation の整合を同時に assert する。
