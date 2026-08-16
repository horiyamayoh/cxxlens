# cxxlens documentation

## Product direction

`cxxlens` は、コンパイラの観測結果を、再現可能・問い合わせ可能・証拠付きの意味知識へ変換し、
分からない場合には何が足りないかまで返す基盤です。

この方向は、公開 surface を数える **供給側閉包**と、独立 consumer の質問から必要 capability・provider・
relation・analysis・evidence まで到達する **需要側閉包**を分け、双方に orphan がない場合だけ完成とみなします。
機械可読な開発 readiness は
[API development readiness](../schemas/cxxlens_ng_api_development_readiness.yaml) にあります。

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

Machine-readable normative authority は `schemas/cxxlens_ng_*` にあります。上の product direction と development
readiness は、個別 relation、provider protocol、identity、query、store、mutation の意味契約を上書きしません。
それらの public semantics は統合設計、catalog/registry、accepted ADR が所有します。

tracked asset の移行完了状態は
[asset migration ledger](../schemas/cxxlens_asset_migration_ledger.json) が exactly-once で記録します。
NG foundation の完了条件は
`schemas/cxxlens_ng_foundation_completion_manifest.yaml`、最終 commit/tree の証拠は CI が生成する
`cxxlens-ng-foundation-completion-report.json` が authority です。

[Archive](archive/README.md) は非規範の履歴資料であり、新規設計や実装判断の根拠には使用しません。
