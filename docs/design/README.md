# cxxlens next-generation design

判断が衝突する場合は次の順序を使用します。

1. [次世代統合設計](cxxlens_next_generation_integrated_design_ja.md) の invariant
2. [catalog / registry](catalogs/README.md)
3. [accepted ADR](adr/README.md)
4. product test fixture と実装

開発・release の運用証跡方式は [ADR 0106](adr/0106-test-only-development-and-release-policy.md) が supersede します。
Git 履歴と GitHub Actions の job log は platform 機能として残しますが、repository 側で
review receipt、exact-SHA record、work-unit、checksum、qualification report、Learning
checkpoint を複製しません。

公開 API を変更する場合は Public API catalog、Doxygen、positive/negative/fault test、
設計 traceability を同時に更新します。製品 runtime の provenance、coverage、unknown、
materialization report、SQLite/source-closure receipt、provider trust は削除対象ではありません。
