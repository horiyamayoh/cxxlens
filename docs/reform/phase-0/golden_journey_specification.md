# Golden Journey Specification

Golden は product semantic behavior を保全する CTest label である。実行ごとの golden
file、timing、log、checksum は作らない。`ctest -L golden` は current configuration で
成立する journey だけを選び、native-only journey は exact capability がない場合に
structured unavailable として扱う。

## Journey と選択可能な試験

| Journey | CTest path |
| --- | --- |
| Generated typed query | `sdk.example-typed_query` |
| Runtime dynamic query | `sdk.example-dynamic_query` |
| Relation/claim batch | `sdk.example-relation_claim_batch` |
| Portable provider | `sdk.example-portable_provider` |
| Clang 22 native provider | `sdk.example-clang22-native`（native available 時のみ `golden`） |
| Snapshot Store | `sdk.example-snapshot_store`、`integration.r2-vertical-slice` |
| Query execution | `sdk.example-query_execution` |
| Flagship recipe / call search | `sdk.example-recipe`、`integration.r2-vertical-slice` |
| Installed consumer | `install.relocation`、`install.sdk-consumer`、`install.examples-consumer` |
| CLI admission | `sdk.cli-usage` と doctor product checks |

`integration.r2-vertical-slice` は複数層の parity を一つの journey として検査する。
`install.prepare` は selected install journey の fixture setup としてのみ追加される。

## Comparator の意味境界

比較するのは semantic identity、relation/claim content、truth/approximation、condition、
interpretation、coverage、closure、structured unresolved reason、conflict、公開された
ordering guarantee である。temporary path、PID、timestamp、timing、diagnostic prose、
unordered iteration order、内部 byte layout は比較対象にしない。

意図的な意味差分は golden 更新で隠さず、変更 class、consumer impact、migration reason
を設計側へ戻す。accepted stable axis の `CH-3` 差分は Phase 1 に取り込まない。
