# cxxlens リファクタリング・プログラム

このディレクトリは、cxxlens の Phase 0–1 リファクタリング作業を説明する補助設計を置く。

## Authority

プログラムの順序、scope、変更境界、Phase gate は [全体設計書](../../cxxlens_Refactoring_Program_Overall_Design_2026-08-26.md) が扱う。ただし、製品の意味論・public API・relation・wire・永続形式・security contract は、次の現行 authority が優先される。

- [次世代統合設計](../design/cxxlens_next_generation_integrated_design_ja.md)
- [ADR 0106](../design/adr/0106-test-only-development-and-release-policy.md)
- relation registry、各 machine contract、public API catalog、security profile
- acceptance/contract test と実装

`docs/archive/` は履歴資料であり、新しい実装の authority ではない。

## Phase 0–1 の保存方針

この作業では、製品 runtime が返す claim/provenance、coverage、closure、unknown、conflict、materialization report、SQLite/source-closure の安全 receipt、provider の trust/identity/certification を維持する。

一方、開発・release の運用証跡は作らない。Acceptance Manifest、work-unit、review receipt、exact-SHA memo、checksum、qualification JSON、集約 report、phase checkpoint、独自 tag/milestone は Phase の完了条件でも repository artifact でもない。完了判定は変更固有試験、main の決定的 CTest、workflow の終了コード、通常の Git 履歴で行う。

## 対象範囲

Phase 0–1 の実装対象は cxxlens repository 内に限定する。既知の auto-aha、cxxmonster 等の downstream は read-only census の対象にとどめ、旧 `cxxlensProviderSDK` / `cxxlens::provider_sdk` 依存が残る間は Phase 1 全体 gate を通過させない。downstream 移行は別途明示的に許可された作業として扱う。
