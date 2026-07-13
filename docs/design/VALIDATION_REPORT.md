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

## Phase B package Contract Candidates (#43–#48)

- candidate group: 6
- candidate packages: `configuration`, `core`, `explain`, `facts`, `graph`, `interop`, `report`, `rules`, `search`, `select`, `testing`, `transform`, `workspace`（13）
- assigned API: 88、exact declaration: 88、unresolved declaration: 0
- implementation state: `conformant` 47、`unimplemented` 41（変更なし）
- policy record: 31、各 record の result outcome 6種: 完全
- positive / negative / ambiguous acceptance: 各 API 1件、計264件
- public type 205 / shared component 66 / provider subject 24 / schema 68: owner 一意、dangling 0
- registry owner and entry overlap: 0
- candidate C++23 syntax usage: 7/7 PASS
- package candidate validator/test: 14/14 PASS
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

### Issue #45 select・search・explain

- candidate packages: `explain`, `search`, `select`（3）
- assigned API: 25、exact declaration: 25、unresolved declaration: 0
- implementation state: `conformant` 14、`unimplemented` 11（変更なし）
- policy record: 5、各 record の result outcome 6種: 完全
- positive / negative / ambiguous acceptance: 各 API 1件、計75件
- typed selector domain 9種、normalization/type erasure/requirements/reason-code registry: 完全
- search snapshot/options/row/open-world/variant/limit/order/dedup/coverage decision table: 完全
- explanation/rejection trace/task card/catalog bounded pure projection: 完全
- selector predicate/reason-code unique fixture、search result kind 8種、options schema: PASS
- candidate C++23 syntax usage: 1/1 PASS
- candidate fingerprint: `sha256:c2a99bdb8fbb47fd06d021f5d1bf8418b29b24ebe82120b50d3688afef720024`

### Issue #46 graph

- candidate packages: `graph`（1）
- assigned API: 6、exact declaration: 6、unresolved declaration: 0
- implementation state: `unimplemented` 6（変更なし）
- policy record: 3、各 record の result outcome 6種: 完全
- positive / negative / ambiguous acceptance: 各 API 1件、計18件
- typed graph-local node/edge ID、parallel/self/conflict/variant identity/order/dedup: 完全
- hierarchy/override/call/include open-world・unknown target・cycle/SCC semantics: 完全
- impact relation/direction/path policy と finite depth/node/edge/path budget: 完全
- JSON authoritative schema / deterministic DOT pure projection / subgraph boundary: 完全
- #44 facts/workspace、#45 select/search、#50 flow owner/provider boundary: dangling 0
- candidate C++23 syntax usage: 1/1 PASS
- candidate fingerprint: `sha256:3c8f4fec0bb55eae81842b2dc4dec84c5efcfc236621f4b9e73888736de9f940`

### Issue #47 rules・report

- candidate packages: `report`, `rules`（2）
- assigned API: 9、exact declaration: 9、unresolved declaration: 0
- implementation state: `unimplemented` 9（変更なし）
- policy record: 4、各 record の result outcome 6種: 完全
- positive / negative / ambiguous acceptance: 各 API 1件、計27件
- immutable rule identity/version/builder/requirements/shared execution/reduction: 完全
- suppression typed key/precedence/provenance/expiry/unused/accounting: 完全
- finding/edit/generation/review authoritative owner compatibility boundary: dangling 0
- report format/options/envelope、redaction、UTF-8/LF、record-boundary budget truncation: 完全
- candidate C++23 syntax usage: 1/1 PASS
- candidate fingerprint: `sha256:a7d7d2ef4dc907b517ead76a54d36ba067fcc0f1aa5da405eebef30e1d6d8143`

### Issue #48 transform

- candidate packages: `transform`（1）
- assigned API: 9、exact declaration: 9、unresolved declaration: 0
- implementation state: `unimplemented` 9（変更なし）
- policy record: 4、各 record の result outcome 6種: 完全
- positive / negative / ambiguous acceptance: 各 API 1件、計27件
- immutable codemod/edit plan、canonical edit ID/order/dedup、precondition と stale digest: 完全
- default dry-run、macro spelling origin、variant agreement、format/reparse fail-closed: 完全
- multi-file prepare/commit/rollback、partial commit 禁止、rollback failure/recovery evidence: 完全
- #49 writer reuse、#52 spike 境界、production implementation 非変更: 明示
- candidate C++23 syntax usage: 1/1 PASS
- candidate fingerprint: `sha256:6d209dd7b50a01718969b32c75fa1536ef946ccd9a6c71f603878fc13dc67d75`
