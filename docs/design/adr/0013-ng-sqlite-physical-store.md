# ADR 0013: NG SQLite store は publication journal と canonical payload の hybrid とする

- Status: Accepted
- Date: 2026-07-16
- Decision owner: store-kernel
- Decision issue: #68
- Tracking issue: #56

## Context

ADR 0009 は semantic snapshot identity、publication series、transaction、reader pin を backend 非依存で確定したが、
SQLite の物理 schema は未決定だった。旧 `cxxlens.sqlite-fact-store.v1` は legacy fact record 専用であり、relation claim、
condition、closure、unresolved と exact series selector を格納できない。旧 schema を拡張すると legacy contract と
NG semantic contract の authority が再び混在する。

## Decision

NG SQLite format は `cxxlens.sqlite-semantic-store.v2` とし、次の hybrid を採用する。

- `cxxlens_ng_metadata` は physical format version を保持する。
- `cxxlens_ng_publication` は publication ID、exact series ID、semantic snapshot ID、monotonic sequence、physical
  generation、parent、state、checksum、versioned canonical payload を保持する。
- series/sequence index は exact head lookup のためだけに使い、scan/page order を semantic order にしない。
- payload は pointer-free detached rows と manifest projection を length-prefixed binary で格納し、open 時に full
  SHA-256 checksum と semantic snapshot digest を再計算する。
- publication は WAL、`synchronous=FULL`、`BEGIN IMMEDIATE` により atomic に書く。memory head は database commit
  成功後だけ更新する。
- compaction は payload を新 physical generation へ copy-on-write し、既存 handle が pin する generation は
  shared token の最終解放まで保持する。

旧 v1 fact store は source を変更しない一方向 migration bridge だけを許す。bridge は legacy fact を accepted
relation mapper へ渡した後、通常の claim/partition validator と v2 publication transaction を通す。silent row
copy、v1/v2 dual write、v2 から v1 への downgrade は禁止し、bridge は Issue #72 で撤去する。

## Consequences

- memory と SQLite は同じ canonical identity implementation を使い、backend/path/page order は snapshot ID に
  入らない。
- physical schema は public API に露出せず、format migration は semantic digest が一致した場合だけ成功する。
- corrupted current head は prior head へ fallback せず、明示的な intact prior publication は読める。
- claim payload の query index は semantic kernel 完成後に additive physical index として追加できる。

## Verification

`tests/unit/sdk/store_test.cpp` が memory/SQLite parity、reverse arrival、reopen、staged invisibility、CAS、prior open、
partial closure rejection、cursor lifetime、pinned compaction を検証する。installed consumer は static/shared の両方で
memory と SQLite factory を link/open する。contract conformance は root relocation、forward/reverse/seeded shuffle、
jobs 1/2/8 の 36 通りを比較する。物理契約は
`schemas/cxxlens_ng_sqlite_store_contract.yaml` と schema に固定する。
