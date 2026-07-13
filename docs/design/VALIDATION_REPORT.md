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

## Phase B package Contract Candidates (#43–#44)

- candidate group: 2
- candidate packages: `configuration`, `core`, `facts`, `interop`, `testing`, `workspace`（6）
- assigned API: 39、exact declaration: 39、unresolved declaration: 0
- implementation state: `conformant` 33、`unimplemented` 6（変更なし）
- policy record: 15、各 record の result outcome 6種: 完全
- positive / negative / ambiguous acceptance: 各 API 1件、計117件
- public type 179 / shared component 66 / provider subject 24 / schema 58: owner 一意、dangling 0
- registry owner and entry overlap: 0
- candidate C++23 syntax usage: 3/3 PASS
- package candidate validator/test: 10/10 PASS
- production implementation / public install integration / final freeze: 未主張（#53 / #54 owner を維持）

### Issue #43 core・configuration・testing

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

### Issue #44 workspace・facts・interop

- candidate packages: `facts`, `interop`, `workspace`（3）
- assigned API: 22、exact declaration: 22、unresolved declaration: 0
- implementation state: `conformant` 20、`unimplemented` 2（変更なし）
- policy record: 6、各 record の result outcome 6種: 完全
- positive / negative / ambiguous acceptance: 各 API 1件、計66件
- workspace root/command/variant/scope/snapshot/provisioning decision table: 完全
- fact profile/query/snapshot/coverage conservation: 完全、complete zero-match と unresolved を分離
- borrowed Clang lifetime / custom extractor registration state machine / failure isolation: 完全
- custom fact schema positive/native-pointer/name-only negative fixture: 3/3 PASS
- provider ownership: `custom` は `provider.fact.custom-extractor` の一意 owner、重複0
- candidate C++23 syntax usage: 1/1 PASS
- candidate fingerprint: `sha256:e0a3a1ad4ded7631b87210183992c0b1d4c9c8535ad06664faac3f62d1188601`
