# ADR 0025: query intermediate と memory budget を allocation 前に予約する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: query-runtime
- Decision issue: #82
- Depends on: ADR 0007, ADR 0014, ADR 0023, ADR 0024

## Context

reference executor は各 operator が output vector を全件 materialize した後にだけ row 数と canonical byte 数を検査して
いた。join、union、distinct、order、projection は budget 超過後も candidate を保持し続け、semi-join は全 witness を
一時 vector に蓄積した。このため結果は fail closed でも、CPU と memory consumption は budget で制限されなかった。

## Decision

全 operator row retention を一つの budget-aware collector に統一する。collector は次の順序を守る。

1. target collection の row count が `max_intermediate_rows` 未満か検査する。
2. annotated row の deterministic canonical accounting byte 数を checked arithmetic で求める。
3. 全 retained logical payload との合計が `max_memory_bytes` 以下か予約する。
4. 予約後にだけ vector へ row を追加する。

row limit は operator output collection ごとの保持上限であり、zero は最初の row view を copy する前に停止する。memory
limit は execution 全体で保持中の operator row payload、scan source annotation の canonical payload、および distinct key
scratch の合計である。semi-join は witness vector を持たず、一つの selected row へ canonical evidence を逐次 union し、
各 witness 後の candidate size を保持前に照合する。union/order/limit も無制限な vector assignment を使わず collector
経由で copy し、order は budget 済み vector を in-place sort する。implicit terminal projection と distinct row growth も
replacement 前に差分を予約する。

decoded IR arguments、order key metadata、container allocator overhead、in-place sort の control stack、logical completion
後に構成する result side-channel payload は logical memory accounting の対象外とする。これは process RSS の byte-exact
quota ではなく、backend/allocator 非依存な retained semantic payload 上限である。row/canonicalization/container retention
で観測した `std::bad_alloc` は `sdk.query-memory-budget` へ変換する。

budget exhaustion は candidate loop を直ちに停止し、operator graph の残りと conflict/differential side-channel 分類を
実行せず、空 row、`failed_before_result`、structured unresolved を返す。physical explanation は
`peak-logical-bytes` と `peak-intermediate-rows` を記録する。

## Consequences

- join output は上限を超える全積を materialize しない。
- zero budget と byte boundary の挙動が deterministic になる。
- semi-join の resource use は witness 数に比例する一時 row vector を必要としない。
- memory accounting の包含・除外範囲を sandbox の process-level quota と混同しない。

## Verification

`tests/unit/sdk/query_runtime_test.cpp` は zero intermediate budget の peak row が 0 であること、8×8 join が八 row を
保持した時点で停止して 64 row を materialize しないことを検証する。unbounded execution が報告する peak logical bytes と同値および 1 byte 余裕の
budget は成功し、1 byte 不足は memory unresolved になる。semi-join は両 scan 完了時の retained bytes を上限にすると
最初の witness candidate を保持せず semi-join node で停止する。
