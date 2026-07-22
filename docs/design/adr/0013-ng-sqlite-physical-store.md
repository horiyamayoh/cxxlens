# ADR 0013: NG SQLite store は publication journal と canonical payload の hybrid とする

- Status: Accepted
- Date: 2026-07-16
- Decision owner: store-kernel
- Decision issue: #68
- Tracking issue: #56
- Current-layout amendment: ADR 0097 / #200

## Context

ADR 0009 は semantic snapshot identity、publication series、transaction、reader pin を backend 非依存で確定したが、
SQLite の物理 schema は未決定だった。relation claim、condition、closure、unresolved、exact series selector を
一つの semantic contract として保存できる専用 format が必要だった。

## Decision

NG SQLite format は `cxxlens.sqlite-semantic-store.v2` とし、次の hybrid を採用する。Issue #69 で physical
minor を 2.1.0 へ進め、payload v2 に query annotation projection を追加した。Issue #73 で minor 2.2.0 /
payload v3 とし、query row/report が exact producer ID と semantic contract を保持できるようにした。
Issue #78 / ADR 0021 で minor 2.3.0 / payload v4 に exact partition identity binding と validated closure
certificate subject を追加し、v1〜v3 の ID-only closure は closed-world proof に使用しない。
Issue #90 / ADR 0033 で minor 2.4.0 / payload v5 に完全な partition claim envelope を追加し、open/compaction 時に
manifest と query-visible row/annotation/coverage projection を bottom-up で再構成する。
Issue #132 で physical minor を 2.5.0 とし、connection/process 間 publication CAS のための durable series headを追加する。

- `cxxlens_ng_metadata` は physical format version を保持する。
- `cxxlens_ng_publication` は publication ID、exact series ID、semantic snapshot ID、monotonic sequence、physical
  generation、parent、state、checksum、versioned canonical payload を保持する。
- `cxxlens_ng_series_head` は series ID ごとの current publication と sequence を保持する。publish は
  `BEGIN IMMEDIATE` 後にこの head と expected parent/sequence を再照合し、publication の immutable `INSERT` と head updateを
  同一 transaction で commit する。不一致は `store.publication-conflict` で rollbackし、publish path は `INSERT OR REPLACE`を使わない。
- series/sequence index は exact head lookup のためだけに使い、scan/page order を semantic order にしない。
- payload は pointer-free detached rows、manifest projection、query 用 claim annotation を length-prefixed binary
  で格納し、open 時に full SHA-256 checksum と semantic snapshot digest を再計算する。payload v1 は row read の
  ために読めるが、condition/interpretation/provenance/guarantee を推測せず query execution を拒否する。payload
  v2 は query annotation を読めるが producer field を持たないため、明示的な legacy-unknown producer として扱う。
- publication は WAL、`synchronous=FULL`、DB head CAS により複数 connection/process 間でも atomic に書く。memory head は
  database commit 成功後だけ更新する。
- compaction は payload を新 physical generation へ copy-on-write し、既存 handle が pin する generation は
  shared token の最終解放まで保持する。

ADR 0097 はこの hybrid と logical payload policy を維持しつつ、current physical layout を
`cxxlens.sqlite-semantic-store.v3` / `3.0.0` の bounded chunk table に置き換える。本 ADR の v2.6.0 schema は
read-only direct-open predecessor と registered migration source としてのみ authority を保つ。新規 DB と write は
v3 を使用し、v2 open は DDL/metadata/PRAGMA write を行わない。v2→v3 は既存 `snapshot_store::compact()` の
single-transaction COW migration だけを許し、open-time migration と新 public migration surface を禁止する。
v3 のlocator/VFS observation、closed format classifier、fresh file+parent durability、rollback/COMMIT terminal recovery、
same-main descendant判定はADR 0097と`cxxlens.sqlite-authority-state.v1`、
`cxxlens.sqlite-authorized-descendant.v1`、`cxxlens.sqlite-terminal-reclassifier.v1`が所有する。本ADRの旧open/compaction記述を
これらのfail-closed境界より優先したり、generic VFS、implicit recovery、diagnostic row rewriteを認可する根拠にしない。

## Consequences

- memory と SQLite は同じ canonical identity implementation を使い、backend/path/page order は snapshot ID に
  入らない。
- physical schema は public API に露出せず、format migration は semantic digest が一致した場合だけ成功する。
- corrupted current head は prior head へ fallback せず、明示的な intact prior publication は読める。
- claim payload の query index は semantic kernel 完成後に additive physical index として追加できる。

## Verification

`tests/unit/sdk/store_test.cpp` が memory/SQLite parity、reverse arrival、reopen、staged invisibility、単一 instance と
複数 SQLite instance の CAS、conflict rollback/retry、prior open、
partial closure rejection、cursor lifetime、pinned compaction を検証する。installed consumer は static/shared の両方で
memory と SQLite factory を link/open する。contract conformance は root relocation、forward/reverse/seeded shuffle、
jobs 1/2/8 の 36 通りを比較する。物理契約は
`schemas/cxxlens_ng_sqlite_store_contract.yaml` と schema に固定する。
