# ADR 0090: G5 の absence・incrementality・bounded closure を同一認定単位にする

- Status: Accepted
- Date: 2026-07-18
- Issue: #166

## Context

positive row の存在は不完全入力でも安全に返せるが、row が存在しないという結論は coverage だけでは導けない。設計 14、15、20、26 が要求する
closure certificate は relation、partition content、coverage、key domain、condition、interpretation、assumption、producer semantics、evidence に exact bind
する必要がある。また再利用は source だけでなく dependency、invocation、toolchain、condition universe、environment、provider binary/semantics、relation
descriptor、normalizer、model、assumption、precision の全てに一致しなければならない。これらを個別に「実装済み」と主張すると、absence、warm-zero、R4
performance が同一 revision で成立した根拠が残らない。

## Decision

`query.anti_join.v1` を NG1 の非単調 operator として実装する。右 subtree の coverage が complete で blocking unresolved がなく、各対象 partition に
applicable `relation-key-enumeration` certificate がある場合だけ、witness のない左 occurrence を返す。前提が欠ける場合は absence row を一件も返さず、
`sdk.query-closure-missing` を返す。left multiplicity、condition、interpretation、evidence、total order は保持し、applied certificate ID を result side
channel に含める。

`cxxlens::sdk::incremental` は exact invalidation input を全て値として受け、partition ごとに reuse/recompute と stable reason code を canonical order で返す。
全 partition が exact reuse の場合だけ `warm_zero=true` かつ frontend provider execution count を 0 とする。corruption、coverage、closure の差分も再利用を
禁止する。

bounded transitive closure は condition intersection と exact interpretation を保つ deterministic least fixpoint とする。iteration/edge budget を独立に
指定し、budget exhaustion でも既に証明した positive row と evidence は保持するが `closure_certified` は false、原因は structured unresolved とする。
partition store が永続化する certificate は `relation-key-enumeration` に限定する。bounded closure API は
`call-target-set`、`inheritance-subtype-set`、`include-provider-set` のいずれかを request/result に exact bind し、
永続化用 kind、未登録 kind、kind のすり替えを拒否する。

G5 と R4 は `cxxlens_ng_g5_qualification.yaml` を authority とし、correctness test、memory/SQLite parity、provider hardening contract、固定 fixture・clock・反復数・
budget・compiler/OS/architecture・performance envelope、clean main exact-SHA report が全て揃った場合だけ `gate.g5` を implemented とする。G5 は distribution 1.0 の production support
宣言ではなく、release gate #167 は独立に deferred のままとする。

## Consequences

- incomplete/open world の空結果を absence として扱えない。
- invalidation input を追加する変更は public value、catalog、test、qualification manifest の同時更新を要する。
- recursion の部分結果は利用できるが、closure を必要とする下流判断には使用できない。
- G5 性能値は環境差を含むため、compiler/OS/architecture を evidence に保存し、同一 fixture/method/budget と十分な上限を契約に固定して、exact 値そのものを互換性にしない。
