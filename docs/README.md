# cxxlens documentation

`cxxlens` は、コンパイラの観測結果を、再現可能・問い合わせ可能な意味知識へ変換し、
分からない場合には不足理由まで返す基盤です。

## Normative design

- [次世代統合設計](design/cxxlens_next_generation_integrated_design_ja.md)
- [Catalog / registry index](design/catalogs/README.md)
- [ADR index](design/adr/README.md)
- [Development architecture](development/architecture.md)
- [Extending the platform](development/extending-platform.md)
- [Agent-driven public API development goal](development/agent-api-development-goal.md)
- [Build and test](development/build-and-test.md)
- [Support matrix](support-matrix.md)
- [Tutorials](tutorials/README.md)

開発・release の判定は [ADR 0106](design/adr/0106-test-only-development-and-release-policy.md) に従います。
変更固有試験と main の全決定的 CTest が開発完了条件であり、release は release workflow の全重検査だけで判定します。
運用証跡は生成・保存しません。

Machine-readable product authority は `schemas/cxxlens_ng_*` と
`schemas/cxxlens_support_matrix.yaml` にあります。claim/provenance、coverage、unknown、
materialization report、SQLite/source-closure receipt、provider trust は製品機能として維持します。

[Archive](archive/README.md) は非規範の履歴資料です。
