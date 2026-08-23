# cxxlens documentation

`cxxlens` は、コンパイラの観測結果を、再現可能・問い合わせ可能な意味知識へ変換し、
分からない場合には不足理由まで返す基盤です。

## Normative design

- [次世代統合設計](design/cxxlens_next_generation_integrated_design_ja.md)
- [Catalog / registry index](design/catalogs/README.md)
- [ADR index](design/adr/README.md)
- [Development architecture](development/architecture.md)
- [Extending the platform](development/extending-platform.md)
- [Build and test](development/build-and-test.md)
- [Support matrix](support-matrix.md)
- [Tutorials](tutorials/README.md)

開発・release の判定は変更固有試験、main の全決定的 CTest、release workflow の終了コードで行います。

Machine-readable product authority は `schemas/cxxlens_ng_*` と
`schemas/cxxlens_support_matrix.yaml` にあります。claim/provenance、coverage、unknown、
materialization report、SQLite/source-closure receipt、provider trust は製品機能として維持します。

[Archive](archive/README.md) は非規範の履歴資料です。
