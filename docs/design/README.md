# cxxlens 次世代設計パッケージ

Authority transition: Issue #57
対象: 次世代 `cxxlens` Semantic Relation Platform
規範文書 ID: `CXXLENS-NG-SRAD-002`
設計版: `0.3.0-normative`

## 内容

- `cxxlens_next_generation_integrated_design_ja.md`
  - 製品境界、relation/claim/snapshot/query/providerの安定核、invariant、release profile、移行、acceptanceを
    定める最上位規範設計書。
- `adr/0002-semantic-relation-platform.md`
  - Semantic Relation Platformへの製品再定義。
- `adr/0003-versioned-relation-kernel.md`
  - Versioned Relation Kernelを安定核にする決定。
- `adr/0004-legacy-contract-reset.md`
  - 旧124 API freezeの失効、baseline化、移行・除去方針。
- `../../schemas/cxxlens_ng_authority_transition.yaml`
  - active authority、superseded authority、legacy dispatch拒否、移行baselineを結ぶ機械可読契約。
- `../../schemas/cxxlens_legacy_api_baseline.yaml`
  - 旧47 conformant / 77 unimplemented APIとM0/M1/M2 evidenceの移行baseline。新規実装authorityではない。
- `cxxlens_integrated_design_ja.md` と `../../schemas/cxxlens_public_api_contract.yaml`
  - Issue #57でsupersededとなった旧設計・旧124 API catalog。移行provenanceとしてのみ保持する。
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
- `package_contract_48.md`
  - Issue #48 が所有する transform の規範的 Contract Candidate と decision/state machine。
- `package_contract_49.md`
  - Issue #49 が所有する generation・mock・method harness・copy・fuzz の規範的 Contract Candidate。
- `package_contract_50.md`
  - Issue #50 が所有する flow・models の規範的 Contract Candidate と availability/fixpoint 決定表。
- `package_contract_51.md`
  - Issue #51 が所有する review・qa の規範的 Contract Candidate と diff/baseline/gate/process/coverage 決定表。
- `high_risk_contract_validation.md`
  - Issue #52 の7領域 technical spike、candidate fingerprint binding、bounded evidence、#53 fail-closed gate。
- `../../schemas/cxxlens_package_contract_candidates.yaml`
  - #43〜#51 の package candidate、exact declaration、意味契約、owner、acceptance の機械可読 authority。
- `SHA256SUMS`
  - 本パッケージ内ファイルの SHA-256。

旧統合設計、旧Phase B package/freeze/readiness資料、旧二層構想は移行provenanceである。新規実装判断では
次世代統合設計、次世代catalog/registry、accepted ADR、担当issue、acceptance manifestの順に参照し、
履歴資料から契約を復活させない。

## 重要な設計判断

1. 安定核をProject Catalog、Condition Universe、Versioned Relation Schema、Semantic Claim、Immutable
   Snapshot、Logical Query、Provider Contractとする。
2. Recipe、typed query、dynamic relation、portable provider、native providerを第一級の開発経路とする。
3. 新relation/provider/recipe追加に中央enum、switch、registry source list変更を要求しない。
4. 通常公開headerからcompiler-native型を排除し、major-specific worker/SDKへ閉じ込める。
5. evidence、coverage、closure、unresolved、guarantee、condition、interpretationを失わない。
6. identity、serialization、provider selection、query resultをroot/jobs/order/backend非依存にする。
7. mutation/artifact effectはplan、validator、dry-run、journaled transactionを経由する。
8. 旧124 APIとM0/M1/M2資産はbaselineから明示移行し、Issue #72でlegacy pathを完全除去する。

## 実装開始時の読み順

1. 次世代統合設計書の0、2、5〜9、11、14、15、17、20、26〜28章。
2. accepted ADRと担当GitHub issue。
3. 担当relation/API/providerの次世代catalog/registry entry。
4. identity、condition、coverage/closure、lifetime/thread/order/error/partiality contract。
5. 対象vertical sliceのacceptance manifestとpositive/negative/perturbation fixture。
6. 旧資産を触る場合はlegacy baselineとasset migration ledger。
