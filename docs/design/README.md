# cxxlens next-generation design

判断が衝突する場合は次の順序を使用します。

1. [次世代統合設計](cxxlens_next_generation_integrated_design_ja.md) の invariant
2. [catalog / registry](catalogs/README.md)
3. [accepted ADR](adr/README.md)
4. product test fixture と実装

公開 API を変更する場合は Public API catalog、Doxygen、positive/negative/fault test、
設計 traceability を同時に更新します。製品 runtime の provenance、coverage、unknown、
materialization report、SQLite/source-closure receipt、provider trust は削除対象ではありません。
