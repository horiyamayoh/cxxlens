# Agent-driven public API development goal

この文書は、Codex の `/goal` と複数のコーディングエージェントを使って、cxxlens の公開 API
全体を継続開発するための実行契約である。次の短い goal からこの文書を参照する。

```text
/goal docs/development/agent-api-development-goal.md を実行契約として、本リポジトリの実質すべての公開 API を contract-driven、issue-tracked、evidence-closed で完成させ、production qualification 可能な状態にしてください。必要に応じて複数のサブエージェントを自律的に編成し、roadmap 作成から実装、issue 単位の commit/push、最終 SHA の CI green まで継続してください。
```

## Mission

本リポジトリの実質すべての公開 API を、次世代統合設計、release profile、Public API Catalog、
Relation Registry、Provider Protocol、Acceptance Manifest、Security Profile に従って完成させ、
production qualification 可能な状態にする。

ここで「すべての公開 API」とは、無制限に新しい API を考案することではない。対象は次の集合とする。

- 現行 Public API Catalog と Relation Registry に登録された API
- accepted design、ADR、release profile が要求する API
- deferred / planned gate の達成に必要な API
- 独立 consumer の vertical slice から不可避と確認された API
- contract audit で、既存 API の安全性・完全性を満たすために必要と判明した API

API 数を増やすこと自体を成果にしない。重複 API、誤った abstraction、根拠のない convenience API は
追加しない。既存 API の統合、縮約、置換、削除が contract と consumer evidence に照らして適切なら、
それも完成に向けた正当な作業とする。

開発原則は次とする。

> contract-driven, issue-tracked, evidence-closed, learning-fed

- ドキュメントは「現在、何が正しい契約か」を保持する。
- GitHub issue は「次に、何を変更するか」を追跡する。
- CI evidence は「その commit が契約を満たしたか」を証明する。
- implementation learning は「現場の反証とより良いモデルを、次の契約へどう還流するか」を保持する。

## Consumer の定義

consumer は人間の人数ではなく、公開 API を利用する独立したコードベースを指す。

独立 consumer として数えられるものは、API 実装とは別に変更・リリースされる application、provider、
recipe、analysis module、package である。同一リポジトリ内で実装変更に追従するだけの unit test、fixture、
example は、qualification evidence にはなるが独立 consumer には数えない。

stable API は、二つ以上の独立 consumer、または不可避な foundational invariant、実装、acceptance fixture、
error/partial semantics、lifetime/threading/order、versioning、performance characteristics、lower-level escape
path、experimental period が揃うまで stable と宣言しない。

新規 API は原則として experimental または versioned から開始する。implemented、stable、
production-supported を混同しない。

## Authority

作業開始時に repository root の `AGENTS.md` を読み、常に次の authority 順序を守る。

1. `docs/design/cxxlens_next_generation_integrated_design_ja.md` の原則と invariant
2. Relation Registry、Provider Protocol、Public C++ API Catalog、Acceptance Manifest、Security Profile、
   release bundle
3. accepted ADR と担当 GitHub issue の exact contract
4. acceptance fixture と実装
5. `docs/archive/` の履歴資料

archive、旧124 API catalog、旧 freeze、過去の実装都合を新規 API の authority にしない。上位 contract を
テストや現行実装に合わせて縮小してはならない。

core abstraction、identity、condition、truth、closure、protocol major、snapshot format、native lifetime、
sandbox、mutation、determinism を変更する場合は、実装前に ADR を作成する。

## Implementation Learning and Design Feedback

normative document は、明示的に置換されるまでは現在の正しい契約である。agent は文書に盲従して evidence を捨ててはならないが、
より良いと思う設計へ silent deviation してもならない。実装事実と contract の矛盾、hidden assumption、再利用可能な mental model、
public contract の有力な改善案を発見した場合は
`docs/development/implementation-learning/README.md` に従う。

- 専用 GitHub issue と non-normative design feedback record に observation、working model、evidence、alternatives を保存する。
- correctness、security、invariant、compatibility、不可逆な変更は解決まで対象実装を block する。
- local/reversible change は self review、high-risk normative change は著者と異なる reviewer の反証 review を要求する。
- accepted record 自体を authority にせず、ADR/contract/catalog/test/traceability を先に更新する。
- issue 完了時に `Learning checkpoint: none` または関連 DF ID を evidence に残す。

raw work log を全 issue に義務化しない。material な知見だけを record にし、reusable な accepted insight は curated mental model へ
反映する。未解決 blocking record を持つ implementation issue は閉じない。

## Initial Roadmap Phase

実装を始める前に、現在の API と未完成領域を棚卸しする。最低限、次を調査する。

- Public API Catalog の全 entry、symbol、status、stability、evidence
- Relation Registry の全 descriptor、identity、reference、evolution policy
- Acceptance Manifest の implemented / deferred / planned gate
- release bundle の NG0〜NG3、G0〜G8、GR、R0〜R7
- provider support matrix の qualification と pending field
- public header と実装、test、example、Doxygen の対応
- closed issue と deferred / planned contract の不整合
- unchecked Definition of Done、未解決 Open Decision、追跡 issue の欠落
- 重複 API、過剰 API、欠落 API、lower-level escape path の不足

この結果から machine-readable な API roadmap と GitHub tracking issue graph を作る。roadmap の各項目には
次を持たせる。

- API domain と exact contract ID
- consumer / use case
- profile、gate、stability tier
- authority と依存 API
- semantics、identity、partiality、versioning
- implementation state と qualification state
- required tests / examples / evidence
- owner tracking issue
- completion criteria

G5、GR、production provider qualification、real-project/performance qualification など、deferred / planned のまま
open tracking issue が存在しない項目には issue を作成する。

roadmap を作成しただけで goal を完了してはならない。roadmap に従って API を end-to-end に実装・認定する
ところまで継続する。

## Multi-agent Operation

必要に応じて複数のサブエージェントを起動し、調査、設計、実装、検証、セキュリティレビュー、CI 解析を
委任する。

人数、役割、作業分割、モデル、並列化、起動・終了のタイミングは固定しない。依存関係、変更競合、リスク、
コンテキスト効率、検証コストを考慮して、統括エージェントが自律的に決定する。

並列作業による競合や不整合を防ぐ。

- 同じ checkout または同じファイルを複数エージェントに同時編集させない。
- 並列書き込みは所有範囲が完全に分離している場合に限る。
- 必要なら agent ごとに独立 worktree / branch を使用する。
- read-heavy な調査・監査・レビューは積極的に並列化する。
- 依存する実装や shared contract の変更は直列化する。
- 関連するサブエージェントの結果を待ち、矛盾を解消してから統合する。

最終的な contract 判断、差分統合、issue state、commit、push、CI 判定には統括エージェントが責任を持つ。

## Issue Workflow

実装作業は GitHub issue で追跡する。大規模な API domain は tracking issue と bounded child issue に分割する。

```text
Tracking issue
├── Contract / ADR
├── Schema / identity / validator
├── Runtime implementation
├── Provider / store / query integration
├── Examples / consumers
└── Qualification / release evidence
```

各 implementation issue は独立して検証できる vertical slice とする。単なるファイル単位や層単位に分割し、
利用可能な結果が長期間存在しない状態を避ける。

各 issue には次を記載する。

- consumer / use case
- exact scope と non-goals
- authority、ADR、contract ID
- semantics と invariants
- identity、partiality、versioning
- public API surface
- positive / negative acceptance cases
- Definition of Done
- dependency issue
- deferred 項目と follow-up issue
- design feedback ID または `Learning checkpoint: none`

次を禁止する。

- unchecked DoD を残したまま completed とする
- deferred / planned 作業を follow-up issue なしで閉じる
- closed issue を未実装 gate の実行 owner とする
- issue コメントだけを永続的な仕様 authority にする
- design feedback を記録せず contract から逸脱する、または non-normative record を authority として実装する
- test に合わせて上位 contract を縮小する
- issue を閉じるためだけに unsupported surface を omission する

tracking issue は全 child issue と最終 evidence が揃うまで閉じない。

## API Development Lifecycle

各 API は次の状態遷移を通す。

```text
proposed
→ experimental
→ versioned / implemented
→ qualified
→ stable
```

各 API または API domain を次の順で開発する。

1. authority、relevant mental model、unresolved design feedback を確認する
2. consumer / use case を確定する
3. semantics、invariants、non-goals を定義する
4. identity、value types、condition、interpretation、partiality を定義する
5. schema、registry、catalog、version evolution を定義する
6. implementation と独立した validator を実装する
7. positive / negative / property / fault test を実装する
8. public API と production implementation を実装する
9. provider → claim → store → query → recipe の vertical path を接続する
10. typed / dynamic、memory / SQLite、in-process / process parity を検証する
11. example、negative example、install consumer、Doxygen を追加する
12. support matrix と qualification evidence を更新する
13. learning checkpoint と unresolved blocker の不在を確認する
14. experimental period と独立 consumer evidence を経て stable admission を判断する

schema-first の順序を崩さない。

```text
semantics / invariants
→ identity / value types
→ schema / registry
→ validator
→ tests
→ service / runtime
→ public API
→ integration / qualification
```

## Implementation Rules

- C++23 を使用する。
- 公開 namespace、type、function は lower snake case に従う。
- 通常の public header に `clang::*`、`llvm::*`、LLVM/Clang header を露出しない。
- compiler-native object、pointer、reference、address を provider 境界外へ出さない。
- AST pointer を保存、所有、別 thread へ移送しない。
- raw owning pointer を導入しない。
- filesystem、process、time、hash は port 越しに扱う。
- unordered container の iteration order を serialization、ID、公開順序に使わない。
- name や pretty type string だけを semantic identity に使わない。
- empty、unresolved、unsupported、unavailable、failed、truncated、stale を区別する。
- coverage、guarantee、condition、provenance を後段で失わない。
- compile command、variant、provider、version の silent fallback / first-wins を禁止する。
- conflict、stale digest、reparse failure、unknown value を無視しない。
- diagnostic prose substring を制御に使わない。
- shell command を文字列連結して実行しない。
- mutation / generation は plan、独立 validator、dry-run、transaction の順を守る。
- macro expansion range を直接 edit しない。
- 公開 API、relation、provider を変更したら catalog / registry、Doxygen、acceptance test、設計
  traceability を更新する。
- 旧124 API catalog に新しい surface を追加しない。
- ユーザーの既存変更と無関係な差分を上書きしない。

## Verification

各 issue について変更リスクに応じた targeted test を先に実行し、完了前に以下の full gate を実行する。

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang --output-on-failure
cmake --build --preset dev-clang --target cxxlens-quality
```

対象に応じて次を追加検証する。

- static / dynamic descriptor・IR parity
- memory / SQLite semantic parity
- in-process / process provider surface parity
- root relocation
- jobs 1 / 2 / 8
- insertion order、task order、seeded shuffle
- cold / warm / no-cache
- static / shared install consumer
- GCC / Clang public-header compatibility
- malformed、truncated、oversized、checksum failure
- stale、unknown、conflict、corruption
- timeout、cancel、budget exhaustion、crash
- prior snapshot survival
- ASan / UBSan、TSan
- clang-tidy
- clean no-cache stress
- real-project qualification
- scale / performance manifest
- provider exact binary / support tuple qualification

テスト失敗時は原因を特定し、contract を弱めずに修正する。flaky test、環境差、既知警告として根拠なく
無視しない。

固定 fixture と contract checker だけで production qualification を宣言しない。実行時 test、negative test、
independent consumer、real-project evidence を組み合わせる。

## Commit, Push, Issue Closure

1つの GitHub issue を完了するごとに、対象差分だけを commit し、`main` へ push する。

複数 issue の無関係な変更を1 commit にまとめない。1 issue に複数 commit が必要な場合も、issue 完了時点の
exact commit set を記録する。

issue には完了前に次の evidence をコメントする。

- commit SHA
- 変更した authority / contract / catalog ID
- production implementation
- positive / negative tests
- error / unresolved / coverage / guarantee の扱い
- 実行した build / test / quality command
- CI run URL
- deferred 項目と follow-up issue
- learning checkpoint (`none` または関連 DF ID) と `issue-ready` の結果
- 完了判定の根拠

DoD を満たした場合だけ `completed` として閉じる。未実装、未検証、未認定の作業を `not planned` で隠さない。

## CI Monitoring

全対象 issue の完了後、最終 `main` SHA の required CI を監視する。

失敗した場合は job、step、log、artifact を調査し、根本原因を修正して commit / push する。新しい SHA の
全 required CI が緑になるまで継続する。

通常 quality gate だけでなく、最終 SHA について可能な限り次を確認する。

- static / shared build-test
- install consumers
- public headers
- contract / quality evidence
- foundation completion
- ASan / UBSan
- TSan
- clang-tidy
- clean no-cache stress

過去 SHA の成功を最終 SHA の evidence として流用しない。

## Progress Reporting

作業中は定期的に日本語で簡潔に報告する。報告には次を含める。

- 現在の API domain / issue
- 完了したこと
- 確認した根拠
- 実行中の検証
- 残作業
- blocker または contract decision
- 新しい design feedback と implementation disposition

生の長大な log を貼るのではなく、結論、失敗箇所、根拠を要約する。

意味契約を変更する複数の妥当な選択肢があり、authority から決定できない場合だけユーザーへ確認する。
それ以外は安全で reversible な判断を行い、自律的に進める。

## Final Completion Criteria

次をすべて満たすまで goal を完了しない。

- authority が要求する公開 API が roadmap 上で exactly once に分類されている
- required API が implementation と public surface を持つ
- semantics、identity、partiality、versioning が明示されている
- schema、validator、positive / negative test が存在する
- typed / dynamic、backend、provider surface の必要 parity が成立する
- example、install consumer、Doxygen、catalog、traceability が揃う
- G5 と release qualification を含む mandatory gate が evidence 付きで完了する
- production-supported と宣言する provider tuple が exact digest と qualification を持つ
- deferred / planned 作業が追跡 issue なしで残っていない
- 全 implementation issue が learning checkpoint を持ち、未解決 blocking design feedback を残していない
- stable API が stable admission 条件を満たす
- 全対象 issue が根拠付きで完了している
- final `main` SHA の required CI がすべて成功する
- `HEAD` と `origin/main` が一致する
- worktree が clean
- 全サブエージェントの結果が統合されている
- unsupported / future scope が明示され、実装済みと誤認されない

単に open issue が0件、testがgreen、catalogがimplementedであることだけを全体完成の根拠にしてはならない。

完了時には、API roadmap、実装 API、stability / qualification、issue と commit の対応、test / CI evidence、
support matrix、残存非スコープをまとめた最終レポートを提出する。
