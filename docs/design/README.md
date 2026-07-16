# cxxlens next-generation design

判断が衝突する場合は次の順序を使用します。

1. [次世代統合設計](cxxlens_next_generation_integrated_design_ja.md) の invariant
2. [次世代 catalog/registry](catalogs/README.md)
3. [accepted ADR](adr/README.md)
4. acceptance fixture と実装

旧 124 API freeze、fact/profile/selector architecture、M0/M1/M2 gate は active tree から削除済みです。
[Asset migration ledger](../../schemas/cxxlens_asset_migration_ledger.json) は active、archived、generated の
terminal state だけを持ちます。

公開 API を変更する場合は Public API catalog、Doxygen、positive/negative test、設計 traceability を同時に更新します。
