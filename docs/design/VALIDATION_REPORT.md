# cxxlens 設計成果物検証レポート

検証日: 2026-07-13

## 統合設計書

- UTF-8 読み込み: PASS
- Markdown 章番号: `0` から `45` まで連続、PASS
- 総章数: 46
- 見出し数: 211
- fenced code block: 60組、開始・終了対応 PASS
- `TODO` / `TBD` / `FIXME` / `???`: 0
- 旧公開 namespace `cxxred::`: 0
- 必須概念の存在: 単一ライブラリ、LLVM 22.1.8、`compile_commands.json`、surface census、dry-run、evidence、coverage、unresolved、raw corridor、plan validator、method harness、mock/fake、virtual candidates、multi-variant、deterministic — PASS

## 公開 API 契約 YAML

- YAML parse: PASS
- schema: `cxxlens.api-catalog.v2`
- package 数: 22
- API entry 数: 124
- package ID 一意性: PASS
- API ID 一意性: PASS
- package 必須フィールド: PASS
- API 必須フィールド: PASS
- phase 分布:
  - M0: 17
  - M1: 21
  - M2: 18
  - M3: 16
  - M4: 16
  - M5: 9
  - M6: 10
  - M7: 8
  - M8: 9
- contract maturity:
  - `contract-defined`: 36
  - `planned`: 74
  - `stable`: 14
- implementation state:
  - `conformant`: 47
  - `implemented`: 0
  - `unimplemented`: 77

## パッケージ別 API 数

| Package | API count |
|---|---:|
| core | 6 |
| workspace | 8 |
| facts | 10 |
| models | 4 |
| select | 11 |
| search | 10 |
| graph | 6 |
| rules | 5 |
| flow | 8 |
| transform | 9 |
| generate | 2 |
| mock | 3 |
| method_harness | 5 |
| copy | 2 |
| fuzz | 3 |
| qa | 4 |
| review | 5 |
| report | 4 |
| explain | 4 |
| testing | 7 |
| interop | 4 |
| configuration | 4 |

## Phase B package Contract Candidate (#43)

- candidate group: 1
- candidate packages: `configuration`, `core`, `testing`（3）
- assigned API: 17、exact declaration: 17、unresolved declaration: 0
- implementation state: `conformant` 13、`unimplemented` 4（変更なし）
- policy record: 9、各 record の result outcome 6種: 完全
- positive / negative / ambiguous acceptance: 各 API 1件、計51件
- public type / shared component / provider / schema owner reference: dangling 0
- registry owner and entry overlap: 0
- candidate C++23 syntax usage: 2/2 PASS
- fail-closed validator fixture: 9/9 PASS
- candidate fingerprint: `sha256:d340846ca120a2adf335ec1ffa4ccb6a5ade3be2142e9212dd4523e34b5e7584`
- production implementation / public install integration / final freeze: 未主張（#53 / #54 owner を維持）
