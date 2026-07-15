---
cxxlens_document_status: archived
cxxlens_authority: non-normative
cxxlens_replacement: docs/design/cxxlens_next_generation_integrated_design_ja.md
cxxlens_removal_issue: "#72"
---
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

## Phase B package Contract Candidates (#43–#51)

- candidate group: 9
- candidate packages: `configuration`, `copy`, `core`, `explain`, `facts`, `flow`, `fuzz`, `generate`, `graph`, `interop`, `method_harness`, `mock`, `models`, `qa`, `report`, `review`, `rules`, `search`, `select`, `testing`, `transform`, `workspace`（22）
- assigned API: 124、exact declaration: 124、unresolved declaration: 0
- implementation state: `conformant` 47、`unimplemented` 77（変更なし）
- policy record: 47、各 record の result outcome 6種: 完全
- positive / negative / ambiguous acceptance: 各 API 1件、計372件
- public type 230 / shared component 66 / provider subject 26 / schema 83: owner 一意、dangling 0
- registry owner and entry overlap: 0
- candidate C++23 syntax usage: 10/10 PASS
- package candidate validator/test: 17/17 PASS
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
- candidate fingerprint: `sha256:12ff14d27a96a78edb062e7263f4221f8b602e7347dc556c549715d3cd12cb4c`

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
- candidate fingerprint: `sha256:11b9a004c59d3895821ecf302cfcd08d7ec2ef5cbcb1fffaf722e7843c058a5c`

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
- candidate fingerprint: `sha256:9bb0cb68c4121996a9dd550df67e070c1d0c04d3609e1399d777c2705b5f7008`

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
- candidate fingerprint: `sha256:6993968a9393a4f465aa9038b8ff58d236a44f0a5d7d3aa438882fdfa6a400e2`

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
- candidate fingerprint: `sha256:139632fb5016e56fbf7085ca6a93e7cc23e40bdfd0063ce18b2910e5e742c956`

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
- candidate fingerprint: `sha256:7f18fade87f89da2014f84a0dc9725e6225f67cca3c044bebd5c44f557158038`

### Issue #49 generation・mock・method harness・copy・fuzz

- candidate packages: `copy`, `fuzz`, `generate`, `method_harness`, `mock`（5）
- assigned API: 15、exact declaration: 15、unresolved declaration: 0
- implementation state: `unimplemented` 15（変更なし）
- policy record: 6、各 record の result outcome 6種: 完全
- positive / negative / ambiguous acceptance: 各 API 1件、計45件
- exhaustive surface census、required-axis decision、artifact identity/order/collision/coverage: 完全
- present/publishable/usable/link-ready/listed/quarantined 独立状態、preview budget: 完全
- #48 default dry-run/one-writer transaction/rollback reuse と artifact lifecycle 境界: 完全
- mock 四軸、exact method cv/ref/noexcept resolution、copy ODR/type closure、fuzz finite inference: 完全
- #50 optional model・#52 spike・#53 integration 境界、production implementation 非変更: 明示
- candidate C++23 syntax usage: 1/1 PASS
- candidate fingerprint: `sha256:55a96e5b61bfc360a321c21b65e6a1908e7fecc5f7ef470fa80985636832a007`

### Issue #50 flow・models

- candidate packages: `flow`, `models`（2）
- assigned API: 12、exact declaration: 12、unresolved declaration: 0
- implementation state: `unimplemented` 12（変更なし）
- policy record: 6、各 record の result outcome 6種: 完全
- positive / negative / ambiguous acceptance: 各 API 1件、計36件
- CFG available/absent/unsupported/failed/partial/stale/variant-divergent capability/provider: 完全
- source/sink/barrier value path、taint lattice/path/budget、coverage/guarantee: 完全
- resource state/alias/RAII/counterexample と effect SCC/fixpoint/widening/cache identity: 完全
- model pack exact binding/merge precedence/trust/version/migration/bounded I/O: 完全
- #46 graph・#49 generation・#52 spike・#53 integration 境界、production implementation 非変更: 明示
- candidate C++23 syntax usage: 1/1 PASS
- candidate fingerprint: `sha256:ff9d40744eeff0c0d6d866875fb78fb40a359df61b38ed5c7f0742a6b715ba29`

### Issue #51 review・qa

- candidate packages: `qa`, `review`（2）
- assigned API: 9、exact declaration: 9、unresolved declaration: 0
- implementation state: `unimplemented` 9（変更なし）
- policy record: 4、各 record の result outcome 6種: 完全
- positive / negative / ambiguous acceptance: 各 API 1件、計27件
- diff base/head/dirty/path/range/rename/binary と baseline exact/equivalent/changed/new/resolved/ambiguous: 完全
- gate pass/warn/fail/indeterminate、partial/unsupported/budget/cancel/provider failure の fail-closed 集約: 完全
- argv-only process root/env/redaction/timeout/output/exit/signal/crash と required/optional QA step: 完全
- coverage format/build/path mismatch/merge と finding/test/artifact many-to-many association: 完全
- #43 findings/coverage・#47 rules/report・runtime ports・#52 spike・#53 integration 境界: 明示
- candidate C++23 syntax usage: 1/1 PASS
- candidate fingerprint: `sha256:34e502b3aa40015c99bbb1f351831fe4d0f7313f6f2e1ce6ecb4121ec3e59b79`

## Phase B high-risk validation（Issue #52）

- current candidate snapshot: #43〜#51 の9 fingerprint、drift 0
- validation domain: graph、flow/resource、transform transaction、generation surface、review/gate、QA process/coverage、interop extractor（7/7 `validated`）
- fixture: 各領域 positive / negative / ambiguous、計21件
- boundedness: 全領域に time / memory / path / state / output 上限あり
- graph: 6 node中5 materialize、1 omitted、cycle/open-world/truncation明示
- flow/resource: fixpoint 3/8 iteration、non-convergence 3 iteration、4-step counterexample
- transform: 2-file mid-write failureを全rollback、rollback failure recovery row 1件
- generation: requested/accounted surface 6/6、ambiguous 1、unsupported 1、type closure 3 node
- review/gate: baseline state 6/6、diagnostic 2/2 budget、partial は `indeterminate`
- QA: argv/timeout/output/unavailable/signal 5 case、coverage state 5種、ambiguous/unmatched association各1
- interop: lifecycle 4 state、fact 3→2 deterministic reduction、thread/schema/exception isolation
- evidence semantic digest: `sha256:c9336e6a2bd18ffc319088b6870c5f3761f2b0d6240175f8767a6d411d393922`
- candidate contract change / reopen: 0
- production implementation / public API / installed target / export へのspike混入: 0
- high-risk gate test: 7/7 PASS、#53 fail-closed dependency: 設定済み
