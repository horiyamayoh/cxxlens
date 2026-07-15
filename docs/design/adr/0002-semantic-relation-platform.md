# ADR 0002: Semantic Relation Platform への製品再定義

- Status: Accepted
- Date: 2026-07-15
- Decision owner: architecture
- Decision issue: #57
- Tracking issue: #56

## Context

旧 `CXXLENS-SRAD-001` は、search、rule、transform、generation 等のユースケースから公開 API を
先に固定した。この方式は代表ユースケースを短く表せる一方、組織固有 relation、異なる frontend、
新しい解析 domain、runtime schema を中央 enum と固定 package surface へ追加し続ける必要がある。
124 API freeze の残り77 APIを実装すると、この拡張限界と二重抽象を固定してしまう。

## Decision

`cxxlens` を、実際の C/C++ build context に基づく versioned semantic claim を immutable snapshotへ
公開し、typed/dynamic logical query と analysis module から利用する Semantic Relation Platform として
再定義する。安定核は Project Catalog、Condition Universe、Relation Schema、Claim/Provenance、
Snapshot、Materialization Runtime、Logical Query、Provider Contract である。

Recipe は第一級の利用入口として維持するが、kernel authority にはしない。typed query、dynamic
relation、portable provider、major-specific native provider も第一級の開発経路とする。

## Consequences

- 旧統合設計と124 API catalog/freezeは移行 provenanceとなり、新規実装を認可しない。
- 新しい relation/provider/recipe は中央 enum、switch、registry source list の変更を要求しない。
- 通常 public header は compiler-native type を露出しない。
- 旧実装は一括削除せず、Issue #72 の ledger に従って移行または除去する。

## Verification

Issue #57 の authority consistency gate が、新設計の最上位性、旧 dispatch の拒否、legacy API
surface の固定、移行 baseline を検証する。最終的な legacy-zero 判定は Issue #71 が所有する。
