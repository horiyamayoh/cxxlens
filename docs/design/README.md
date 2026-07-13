# cxxlens 統合設計パッケージ

作成日: 2026-07-11
対象: 新しい単一ライブラリ `cxxlens`
文書 ID: `CXXLENS-SRAD-001`
設計版: `1.0.0-draft`

## 内容

- `cxxlens_integrated_design_ja.md`
  - 要求分析、要件定義、ユースケース、公開 API、LLVM/Clang 対応、内部アーキテクチャ、詳細処理、非機能要件、テスト、実装ロードマップを統合した規範設計書。
- `../../schemas/cxxlens_public_api_contract.yaml`
  - 22パッケージ・124公開 API エントリの機械可読カタログ。実装 issue、header skeleton、schema、conformance test の生成元として利用する。
- `VALIDATION_REPORT.md`
  - 構文・構造・重複・必須概念に対する検証結果。
- `package_contract_43.md`
  - Issue #43 が所有する core・configuration・testing の規範的 Contract Candidate と決定表。
- `package_contract_44.md`
  - Issue #44 が所有する workspace・facts・interop の規範的 Contract Candidate と決定表。
- `package_contract_45.md`
  - Issue #45 が所有する select・search・explain の規範的 Contract Candidate と決定表。
- `package_contract_46.md`
  - Issue #46 が所有する graph の規範的 Contract Candidate と決定表。
- `package_contract_47.md`
  - Issue #47 が所有する rules・report の規範的 Contract Candidate と決定表。
- `../../schemas/cxxlens_package_contract_candidates.yaml`
  - #43〜#51 の package candidate、exact declaration、意味契約、owner、acceptance の機械可読 authority。
- `SHA256SUMS`
  - 本パッケージ内ファイルの SHA-256。

旧二層構想の設計書は `archive/` に履歴資料として保存する。現在の実装判断では、統合設計書、
API 契約、accepted ADR の順に参照し、履歴資料から契約を復活させない。

## 重要な設計判断

1. 旧 `cxxlens` / `cxxred` の二層公開モデルを廃止し、`cxxlens::cxxlens` という単一公開ライブラリに統一する。
2. LLVM/Clang の全 API をラップせず、workspace、facts、selectors、search、rules、flow、transform、generation、review というユースケース駆動 API を公開する。
3. 通常公開 header から Clang 型を排除し、`<cxxlens/interop/clang.hpp>` の scoped borrowed interop に限定する。
4. AST は frontend worker 内に閉じ込め、長寿命・並列・キャッシュ用途には immutable semantic facts を使う。
5. すべての結果は evidence、coverage、unresolved、guarantee を保持する。
6. 編集・生成は plan-first、dry-run default、macro/build-variant/conflict/format/reparse 検証を必須とする。
7. mock/fake、method harness、semantic copy、fuzz harness は共通の surface census / decision / generation plan / artifact state 基盤を使う。
8. 初期 LLVM baseline は major 22、検証版 22.1.8 とし、LLVM major ごとの adapter で更新する。

## 実装開始時の読み順

1. 統合設計書の「0. 最終設計判断」「6. 要件」「7. 不変条件」「8. 全体アーキテクチャ」。
2. 「37. 実装ロードマップ」と「39. コーディングエージェント実装契約」。
3. 担当パッケージの詳細設計章。
4. YAML カタログ内の対応 API ID、LLVM components、error/guarantee/phase。
5. 「36. テスト戦略」と対象 vertical slice の acceptance criteria。
6. Phase B では担当 Issue の `package_contract_*.md` と package candidate manifest record。
