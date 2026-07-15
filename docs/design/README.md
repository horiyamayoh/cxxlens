# cxxlens 次世代設計パッケージ

この directory は Issue #57 で有効化された次世代 authority と、Issue #58 で正式配置された
machine-readable catalog の入口です。

## Authority

判断が衝突する場合の順序は次のとおりです。

1. [次世代統合設計書](cxxlens_next_generation_integrated_design_ja.md) の invariant
2. [次世代 catalog/registry](catalogs/README.md)
3. [accepted ADR](adr/README.md) と担当 GitHub issue
4. acceptance fixture と実装
5. [asset migration ledger](../../schemas/cxxlens_asset_migration_ledger.json) と legacy baseline

旧 124 API catalog/freeze、package candidate、agent readiness は移行 provenance であり、新規実装を
認可しません。履歴本文は [archive](../archive/README.md) にあります。frozen legacy manifest の参照を
壊さないため `package_contract_*.md` などの元位置には非規範 redirect だけを残しています。

## Current design assets

- `cxxlens_next_generation_integrated_design_ja.md` — `CXXLENS-NG-SRAD-002`、版
  `0.5.0-normative` の最上位設計
- `catalogs/README.md` — Relation Registry、Provider Protocol、Public C++ API Catalog、Acceptance
  Manifest、Security Profile の index
- `adr/README.md` — ADR state と replacement chain の index
- `../../schemas/cxxlens_ng_authority_transition.yaml` — authority transition
- `../../schemas/cxxlens_asset_migration_ledger.json` — 全 tracked asset の exactly-once 会計
- `../../schemas/cxxlens_legacy_api_baseline.yaml` — 旧 47 conformant / 77 unimplemented API の固定 baseline
- `SHA256SUMS` — current design package の generated checksum inventory

## Implementation reading order

1. 統合設計書の 0、2、5〜9、11、14、15、17、20、26〜28 章を読む。
2. 担当 relation/API/provider の catalog entry と owner issue を読む。
3. identity、condition、coverage/closure、lifetime/thread/order/error/partiality を exact contract にする。
4. positive/negative/perturbation fixture と acceptance evidence を定義する。
5. 旧資産を触る場合は ledger の disposition、replacement、removal issue を確認する。

bootstrap catalog は配置と ownership を固定する R0 contract です。owner issue が accepted exact contract へ
昇格していない `contract-pending` entry を実装済み・stable と解釈してはいけません。
