# ADR 0028: union は canonical subtree 順で計画・実行する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: query-runtime
- Decision issue: #85
- Depends on: ADR 0014, ADR 0025, ADR 0027

## Context

Logical IR は `query.union.v1` の child を normalized subtree digest 順に並べ、`A union B` と `B union A` を同じ
identity にしていた。一方 reference executor は `ir.nodes` の格納順で scan/operator を評価していたため、scan、intermediate、
memory budget が先に評価した branch で尽きると、同じ digest から異なる unresolved subject、producer/guarantee、physical explain
が生じ得た。

## Decision

reference planner は validated rooted DAG を実行前に canonical postorder へ lower する。union child は canonical subtree digest
を第一 key、canonical subtree bytes を hash collision の tie-break として sort し、この順序を branch evaluation と union row
addition の両方に使用する。union 以外の入力順は operator semantics の順序を保持する。

budget と cancellation は canonical traversal に対して適用する。physical explain と unresolved subject の node ID は canonical
postorder ordinal (`n0`, `n1`, ...) へ正規化し、入力 IR の任意 node ID や builder branch order を authority にしない。scan
failure subject の relation descriptor ID は維持する。

Logical IR canonicalizer も digest collision 時に raw canonical subtree bytes で tie-break する。これにより identity ordering と
runtime ordering は同じ total order を共有する。

## Consequences

- 同じ logical digest、snapshot、runtime request は union の構築順によらず同じ canonical result と side channel を返す。
- scan/intermediate/memory budget の消費順は再現可能になる。
- physical backend の選択は引き続き physical explain に現れるが、同一 backend 内の branch/node 表記差は正規化される。
- planner は rooted reachable node だけを一度ずつ評価し、shared DAG node を重複実行しない。

## Verification

`tests/unit/sdk/query_runtime_test.cpp` は `A union B` / `B union A` を memory と SQLite の各 backend で実行し、unbounded、scan
budget 0/1、intermediate budget 1 の全 request について logical digest と `query_result::canonical_form()` が一致することを
検証する。既存の全 operator、budget、rooted DAG、backend parity tests も canonical postorder の回帰を拘束する。
