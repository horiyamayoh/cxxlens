# ADR 0026: Logical Query IR を root-closed DAG に限定する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: logical-query / query-runtime
- Decision issue: #83
- Depends on: ADR 0007, ADR 0014, ADR 0023

## Context

Logical IR canonical form は `root` から input edge を逆向きに辿った node だけを serialize する一方、validator は全 node の
root reachability を要求せず、reference executor は `nodes` vector 全体を評価していた。したがって digest に含まれない
scan/filter/project component が budget、source annotation、side channel、physical work を変えられた。

relation requirement も scan から未使用の descriptor を許していた。これは canonical digest には含まれるため hidden work
ではないが、rooted plan と requirement set の対応が曖昧だった。

## Decision

Logical Query IR v1 は一つの root を持つ root-closed DAG である。既存の unique node ID、topological input、operator arity
検証後、validator は root から input edge を反復的に辿り、到達集合が `nodes` 全集合と完全一致することを要求する。
到達不能 node が一つでもあれば `sdk.query-unreachable-node` を返す。再帰 DFS は使わず、untrusted node count で call
stack に依存しない。

全 relation requirement は少なくとも一つの reachable `query.scan.v1` が descriptor ID を参照しなければならない。
未使用 requirement は `sdk.query-unused-relation-requirement` とする。validator 成功後は canonical root traversal、executor
evaluation、schema compatibility reference collection が同一 node 集合を扱う。

## Consequences

- digest/署名の外に hidden scan や budget work を追加できない。
- disconnected component と root より後ろの unused node は fail closed になる。
- requirement set は reachable scan set の canonical descriptor projectionになる。
- executor は validated `nodes` を順番に評価してよく、その集合は canonical identity と一致する。

## Verification

`tests/unit/sdk/query_runtime_test.cpp` は root 非到達 scan/filter/project、二つ目の disconnected component、未使用 relation
requirement を専用 reason code で拒否する。非到達 scan を追加して rooted digest が変わらない旧 exploit も validation
failure に固定する。全 valid runtime matrix で physical `node=` evaluation 数と IR node 数が一致する。
