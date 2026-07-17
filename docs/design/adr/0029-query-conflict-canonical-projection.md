# ADR 0029: query conflict は public semantic fields を lossless canonical object にする

- Status: Accepted
- Date: 2026-07-17
- Decision owner: query-runtime
- Decision issue: #86
- Depends on: ADR 0014, ADR 0020, ADR 0028

## Context

`query_result::conflicts()` が返す `claim_conflict` は relation、semantic key、interpretation、overlap fragments、assertions、
contents を持つ。一方 canonical result は relation/key/interpretation/assertions だけを連結した string を出力し、authoritative
payload identity と condition overlap を欠落させていた。異なる public diagnosis が同じ canonical bytes になり、schema も
`conflicts` item shape を拘束していなかった。

## Decision

claim kernel と query runtime は同じ internal canonical conflict projection を使用する。各 conflict の
`overlap_fragments`、`assertions`、`contents` は UTF-8 lexical orderで sort/dedupし、conflict collection は全6 field の tuple
で sort/dedupする。

canonical JSON は string 連結ではなく、次の exact object とする。

- `assertions`
- `contents`
- `interpretation`
- `overlap_fragments`
- `relation`
- `semantic_key`

object key は lexical canonical order、三つの collection は sorted unique array である。query execution result schema は
additional property を拒否し、全 field を required にする。

## Consequences

- `query_result::conflicts()` で観測できる semantic detail は canonical result から lossless に再現できる。
- contents または overlap だけが異なる conflict は異なる canonical bytes を持つ。
- collection insertion order と duplicate は canonical identity に影響しない。
- memory/SQLite conformance と監査 export は conflict payload/condition の差を検出できる。

## Verification

`tests/unit/sdk/query_runtime_test.cpp` は contents-only difference と overlap-only difference が canonical conflict JSON を変え、
三つの collection の insertion order だけを反転しても bytes が同一になることを検証する。実 query result の structured
conflict object に public API の全 overlap/assertion/content identity が含まれることも検証する。
