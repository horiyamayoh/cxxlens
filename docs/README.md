# cxxlens documentation

## Normative design

- [次世代統合設計](design/cxxlens_next_generation_integrated_design_ja.md)
- [Catalog / registry index](design/catalogs/README.md)
- [ADR index](design/adr/README.md)
- [Development architecture](development/architecture.md)
- [Extending the platform](development/extending-platform.md)
- [Agent-driven public API development goal](development/agent-api-development-goal.md)
- [Implementation learning and design feedback](development/implementation-learning/README.md)
- [Build and test](development/build-and-test.md)
- [Support matrix](support-matrix.md)
- [Tutorials](tutorials/README.md)

Machine-readable authority は `schemas/cxxlens_ng_*` にあります。tracked asset の移行完了状態は
[asset migration ledger](../schemas/cxxlens_asset_migration_ledger.json) が exactly-once で記録します。
NG foundation の完了条件は
`schemas/cxxlens_ng_foundation_completion_manifest.yaml`、最終 commit/tree の証拠は CI が生成する
`cxxlens-ng-foundation-completion-report.json` が authority です。

[Archive](archive/README.md) は非規範の履歴資料であり、新規設計や実装判断の根拠には使用しません。
