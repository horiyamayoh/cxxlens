# Agent-driven public API development goal

この文書は、Codex の `/goal` と複数のコーディングエージェントを使って、cxxlens を継続開発するための実行契約です。
次の短い goal からこの文書を参照します。

```text
/goal docs/development/agent-api-development-goal.md を実行契約として CXXLENS_AGENT_AUTHORIZATION_V1 を適用し、cxxlens を需要側・供給側の双方で閉じた証拠付き意味知識基盤へ育て、issue 単位の branch/PR/merge と最終 SHA の CI evidence まで継続してください。
```

## Autonomous execution and approval boundary

上の短い `/goal` のように、この文書と policy ID を明示参照した goal の実行中だけ standing authorization を有効にします。
単なる質問、診断、read-only review、またはこの実行契約を参照しない依頼から暗黙に有効化しません。ユーザーは実行中でも
authorization をいつでも revoke または narrow でき、その後の操作は狭められた範囲に従います。

| 区分 | 実行境界 |
| --- | --- |
| Standing authorization | read-only audit、active unit 内の編集・生成・test/build、同一 issue の CI 根本修正、unit branch/commit/push、canonical cxxlens repository 上の active issue/PR に限定した更新・check rerun・review 対応、exact-head gate 後の active PR merge、merged-main qualification と learning checkpoint 後の active issue close は再承認不要 |
| Notify and continue | 当初想定外の supporting test/file が必要でも、同一 contract・同一 issue 内で可逆なら、原因、追加 scope、検証方法を commentary で通知して継続します。これは approval gate ではありません |
| Fresh user approval | destructive operation/history rewrite、branch protection 変更、secret/permission 追加、課金、外部 production deploy、active issue/PR workflow 外の顧客・第三者への連絡、ユーザー変更との解消不能な競合、authority で決められない重大な public semantics は停止します。対象、effect、不可逆性または rollback を開示し、exact target/effect に限定した承認を得ます |
| External blocker | 必須 reviewer、toolchain、service、permission を取得できない場合は、証拠と選択肢を示して停止します |
| Platform approval | sandbox、system、host platform が要求する権限確認は standing authorization で迂回しません |

checker が prose の偶然一致に依存せず境界を固定できるよう、次の binding marker を保持します。

- `activation: explicit-goal-contract-reference`
- `non-activation: ordinary-request`
- `standing-scope: canonical-repository-active-unit`
- `notify-and-continue: reversible-same-contract-issue`
- `fresh-approval: exact-target-effect-after-disclosure`
- `external-blocker: evidence-options-stop`
- `platform-approval: never-bypass`
- `skill-compatibility: prior-goal-authorization-satisfies-generic-approval`
- `protected-main: unit-branch-pr-exact-head-review-merge-exact-merged-main`
- `direct-main: prohibited`
- `fresh-approval-reuse: forbidden`
- `revocation: user-anytime`

skill が一般的な explicit approval を要求しても、操作が active policy の standing-authorization 範囲に明示されていれば、
goal 開始時の承認で満たされたものとします。skill の診断、focused plan、結果報告は実行しますが、その approval のためだけに
会話を停止しません。skill がより具体的な安全条件を持つ場合、または操作が列挙範囲外なら、その条件または fresh-approval gate を
維持します。

standing authorization は repository 内の active unit を越える mutable authority を与えません。canonical repository の active
issue/PR における通常の review 応答と、顧客・第三者への外部連絡を区別します。fresh approval は開示した exact target/effect にだけ
有効で、別 target、別 effect、後続操作へ categorical に流用しません。platform approval も別の capability gate であり、この contract
は迂回しません。

## Mission

cxxlens の製品定義は次です。

> **cxxlens は、コンパイラの観測結果を、再現可能・問い合わせ可能・証拠付きの意味知識へ変換し、
> 分からない場合には何が足りないかまで返す基盤である。**

本 goal は、単に公開 API を実装することではありません。次世代統合設計、release profile、Public API Catalog、
Relation Registry、Provider Protocol、Acceptance Manifest、Security Profile に従い、次の二つを同時に閉じます。

- **供給側閉包**: admitted public API、relation、provider、query/store、analysis、model、recipe、CLI/runtime が
  contract、implementation、validator/test、documentation、consumer、qualification evidence へ exactly once で結び付く。
- **需要側閉包**: admitted use case から必要 input capture、capability、provider、relation、analysis/model、query/recipe、
  result/evidence まで executable dependency path が存在するか、未達なら exact tracked gap と completion plan が存在する。

製品完成は両側の orphan がゼロであることを要求します。API 数、relation 数、green check 数、open issue 数だけを完成の根拠にしません。

## Product result contract

recipe、analysis、inspection、transformation preflight は、少なくとも次を区別します。

```text
proved
disproved
unknown
partial
conflicting
```

結果は必要に応じて、次を損失なく保持します。

```text
coverage
closure
unresolved
conflict
differential disagreement
producer semantic contract
guarantee and assumptions
provenance and evidence
logical and physical explanation
```

`unknown` は generic failure または terminal prose ではありません。少なくとも次を machine-readable に返します。

```text
why unknown
missing input
missing capability
missing model or assumption
missing qualification evidence
dependency-ordered completion plan
```

completion plan は「何を capture、追加、設定、認定、再実行すれば、どの result axis が強化されるか」を示します。
不足を empty success、safe、no finding、unsupported omission へ畳みません。

## Consumer and use-case authority

consumer は人間の人数ではなく、公開 contract を利用する独立コードベースです。独立 consumer として数えられるものは、
API 実装とは別に変更・リリースされる application、provider、recipe、analysis module、model pack、package です。
同一 repository 内で実装変更に追従する unit test、fixture、example は qualification evidence にはなりますが、独立 consumer には
数えません。

新しい capability/API を開始する前に、次を machine-readable roadmap または issue exact contract に記録します。

- use-case ID、consumer、答える質問、期待 result states
- input/capture requirements と target build variants
- required relation/provider/query/analysis/model/recipe capability
- coverage/closure/guarantee/partiality requirements
- 現状の satisfied / missing / blocked / tracked-gap
- exact authority、contract IDs、dependencies、write scope
- positive/negative/real-project acceptance evidence
- support/stability tier と owner issue
- actionable unknown と completion plan

stable API は、二つ以上の独立 consumer、または不可避な foundational invariant、実装、acceptance fixture、
error/partial semantics、lifetime/threading/order、versioning、performance characteristics、lower-level escape path、
experimental period が揃うまで stable と宣言しません。新規 API は原則として experimental または versioned から開始し、
implemented、stable、production-supported を混同しません。

## Capability roadmap

開発は kernel surface の増加ではなく、再利用可能な versioned capability pack を垂直に完成させます。優先順位は次です。

1. source closure / capture / replay による real-project substrate
2. declaration/reference/include/macro/inheritance/override/template を含む semantic graph
3. CFG と control exit
4. use-def と value flow
5. alias、read/write/escape、lifetime/invalidation effect
6. interprocedural summary と versioned model/assumption packs
7. proof obligation、immutable plan、overlay verification、journaled transaction を持つ transformation/artifact
8. Clang/GCC/LLVM IR/object/binary 間の cross-provider semantic consensus

これは個別 issue の public semantics を先取りしません。各能力は relation/provider/analysis/model/recipe の versioned contract として
authority-first に導入し、core enum/switch や opaque payload へ用途固有意味を押し込みません。

## Authority

作業開始時に repository root の `AGENTS.md` を読み、常に次の authority 順序を守ります。

1. `docs/design/cxxlens_next_generation_integrated_design_ja.md` の原則と invariant
2. Relation Registry、Provider Protocol、Public C++ API Catalog、Acceptance Manifest、Security Profile、release bundle
3. accepted ADR と担当 GitHub issue の exact contract
4. acceptance fixture と実装
5. `docs/archive/` の履歴資料

archive、旧124 API catalog、旧 freeze、過去の実装都合を新規 API の authority にしません。上位 contract を test や現行実装に
合わせて縮小しません。core abstraction、identity、condition、truth、closure、protocol major、snapshot format、native lifetime、
sandbox、mutation、determinism を変更する場合は、実装前に ADR を作成します。

## Implementation Learning and Design Feedback

normative document は明示的に置換されるまでは現在の正しい契約です。agent は文書に盲従して evidence を捨ててはなりませんが、
より良いと思う設計へ silent deviation してもなりません。実装事実と contract の矛盾、hidden assumption、再利用可能な mental model、
public contract の有力な改善案を発見した場合は
`docs/development/implementation-learning/README.md` に従います。

- 専用 GitHub issue と non-normative design feedback record に observation、working model、evidence、alternatives を保存する。
- correctness、security、invariant、compatibility、不可逆な変更は解決まで対象実装を block する。
- local/reversible change は self review、high-risk normative change は著者と異なる reviewer の反証 review を要求する。
- accepted record 自体を authority にせず、ADR/contract/catalog/test/traceability を先に更新する。
- issue 完了時に `Learning checkpoint: none` または関連 DF ID を evidence に残す。

raw work log を全 issue に義務化しません。material な知見だけを record にし、reusable な accepted insight は curated mental model へ
反映します。未解決 blocking record を持つ implementation issue は閉じません。

## Constructibility gate

public semantics、identity、protocol、persistence、不可逆 effect、resource bound を変更する high-risk contract は、acceptance 前に
次の witness を要求します。

1. executable state machine
2. field availability by phase
3. phase-authentic success/failure outcome union
4. minimal witness implementation
5. finite retained-memory/I/O/open-file/time resource witness
6. crash/effect/recovery matrix
7. implementation author と異なる reviewer の counterexample review

report field はその phase で観測可能な値だけを要求します。pre-decode failure に decoded identity、pre-publication report に
post-commit receipt を要求するなど、時間的に構成不能な contract を受理しません。fixture sentinel、fabricated identity、prose parsing、
silent fallback で構成不能性を隠しません。

## Initial roadmap and demand-closure audit

実装を始める前に、現在の supply inventory と demand inventory を棚卸しします。

### Supply inventory

- Public API Catalog の全 entry、symbol、status、stability、evidence
- Relation Registry の全 descriptor、identity、reference、evolution policy
- Acceptance Manifest の implemented / deferred / planned gate
- release bundle の NG0〜NG3、G0〜G8、GR、R0〜R7
- provider support matrix の qualification と pending field
- public header と実装、test、example、Doxygen の対応
- unchecked Definition of Done、未解決 Open Decision、追跡 issue の欠落
- 重複 API、過剰 API、欠落 API、lower-level escape path の不足

### Demand inventory

- 独立 consumer とその flagship question
- use-case family と exact expected outcomes
- capture/input、variant、toolchain、model/assumption requirements
- use case から relation/provider/query/analysis/recipe/evidence への dependency graph
- consumer が独自に複製している extraction/normalization/qualification
- real-project で初めて露呈する representational gap
- unknown result を強化するために不足する capability
- use case に使われない public surface と、surface を持たない admitted use case

この結果から machine-readable roadmap と GitHub tracking issue graph を作ります。roadmap の各項目には次を持たせます。

- use-case ID、consumer、question、result states
- capability domain と exact contract ID
- profile、gate、stability tier
- authority と依存 API/relation/provider/model
- semantics、identity、partiality、versioning
- implementation state と qualification state
- required tests/examples/real-project evidence
- owner tracking issue、completion plan、completion criteria

deferred/planned のまま open tracking issue がない項目には issue を作成します。roadmap を作成しただけで goal を完了せず、
dependency order に従って end-to-end に実装・認定します。

## Agent context compiler contract

coding agent は repository 全体から task contract を推測しません。各 active unit は、生成または machine-validated された最小 context を
持ちます。

```text
goal and admitted use case
capability gap and dependency path
exact contract IDs
minimum authority reading set
allowed repository-relative write paths
required positive/negative/real-project evidence
known design feedback and disposition
forbidden shortcuts
completion and qualification commands
```

context は catalog/registry/readiness/issue から導出し、手書き要約を shadow authority にしません。必須 field が解決できない場合は
agent に補完させず、stable blocked reason を返します。

## Multi-agent operation

必要に応じて複数のサブエージェントを起動し、調査、設計、実装、検証、セキュリティレビュー、CI 解析を委任します。
人数、役割、作業分割、モデル、並列化、起動・終了のタイミングは固定しません。依存関係、変更競合、リスク、
コンテキスト効率、検証コストを考慮して統括エージェントが決定します。

- 同じ checkout または同じファイルを複数エージェントに同時編集させない。
- 並列書き込みは最大四 unit とし、contract ID と repository-relative write path が完全に分離している場合に限る。
- 必要なら agent ごとに独立 worktree / branch を使用する。
- read-heavy な調査・監査・レビューは積極的に並列化する。
- 依存する実装や shared contract の変更は直列化する。
- 関連する結果を待ち、矛盾を解消してから統合する。

最終的な contract 判断、差分統合、issue state、commit、push、CI 判定には統括エージェントが責任を持ちます。

## Issue workflow

実装作業は GitHub issue で追跡します。大規模 domain は tracking issue と bounded child issue に分割します。

```text
Tracking issue
├── Use case / demand closure
├── Contract / ADR / constructibility
├── Schema / identity / validator
├── Runtime implementation
├── Provider / store / query / analysis integration
├── Examples / independent consumers
└── Qualification / release evidence
```

各 implementation issue は独立して検証できる vertical slice とし、単なるファイル単位や層単位に分割して利用可能な結果が
長期間存在しない状態を避けます。

各 issue には次を記載します。

- consumer / use case / expected result states
- exact scope と non-goals
- authority、ADR、contract ID
- capability dependency path と demand-closure disposition
- semantics、identity、partiality、versioning
- public API surface
- actionable unknown と completion plan
- constructibility witness（該当する場合）
- positive / negative / real-project acceptance cases
- Definition of Done
- dependency issue、deferred 項目、follow-up issue
- design feedback ID または `Learning checkpoint: none`

次を禁止します。

- unchecked DoD を残したまま completed とする
- deferred/planned 作業を follow-up issue なしで閉じる
- closed issue を未実装 gate の実行 owner とする
- issue コメントだけを永続的な仕様 authority にする
- design feedback を記録せず contract から逸脱する
- test に合わせて上位 contract を縮小する
- unsupported surface や consumer gap を omission する
- public surface count を completion の代用にする

tracking issue は全 child issue と最終 evidence が揃うまで閉じません。

## API and capability development lifecycle

各 capability/API は次の状態遷移を通します。

```text
proposed
→ experimental
→ versioned / implemented
→ qualified
→ stable
```

開発順序は次です。

1. authority、mental model、unresolved design feedback を確認する
2. consumer/use case と capability dependency path を確定する
3. demand-closure gap と expected result states を定義する
4. semantics、invariants、non-goals を定義する
5. identity、value types、condition、interpretation、partiality を定義する
6. high-risk なら constructibility witness を反証する
7. schema、registry、catalog、version evolution を定義する
8. implementation と独立 validator を実装する
9. positive/negative/property/fault test を実装する
10. public API と production implementation を実装する
11. provider → claim → store → query → analysis/recipe の vertical path を接続する
12. typed/dynamic、memory/SQLite、in-process/process parity を検証する
13. actionable unknown と completion plan を検証する
14. example、negative example、install consumer、Doxygen を追加する
15. support matrix と qualification evidence を更新する
16. learning checkpoint と unresolved blocker の不在を確認する
17. experimental period と独立 consumer evidence を経て stable admission を判断する

schema-first の順序を崩しません。

```text
use case / capability gap
→ semantics / invariants
→ identity / value types
→ constructibility
→ schema / registry
→ validator
→ tests
→ service / runtime / public API
→ consumer integration / qualification
```

## Implementation rules

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
- `unknown` は不足 reason と completion plan を保持する。
- coverage、closure、guarantee、condition、provenance を後段で失わない。
- compile command、variant、provider、version の silent fallback / first-wins を禁止する。
- conflict、stale digest、reparse failure、unknown value を無視しない。
- diagnostic prose substring を制御に使わない。
- shell command を文字列連結して実行しない。
- mutation/generation は plan、独立 validator、dry-run、transaction の順を守る。
- macro expansion range を直接 edit しない。
- 公開 API、relation、provider を変更したら catalog/registry、Doxygen、acceptance test、設計 traceability を更新する。
- 旧124 API catalog に新しい surface を追加しない。
- ユーザーの既存変更と無関係な差分を上書きしない。

## Verification

各 issue について targeted test を先に実行し、完了前に次を実行します。

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang --output-on-failure
cmake --build --preset dev-clang --target cxxlens-quality
```

対象に応じて次を追加検証します。

- supply/demand closure と orphan detection
- actionable unknown/completion-plan negatives
- constructibility field-availability/crash/resource vectors
- static/dynamic descriptor・IR parity
- memory/SQLite semantic parity
- in-process/process provider surface parity
- root relocation、jobs 1/2/8、insertion/task order、seeded shuffle
- cold/warm/no-cache、static/shared install consumer
- GCC/Clang public-header compatibility
- malformed、truncated、oversized、checksum failure
- stale、unknown、conflict、corruption
- timeout、cancel、budget exhaustion、crash
- prior snapshot survival
- ASan/UBSan、TSan、clang-tidy、clean no-cache stress
- real-project qualification、scale/performance manifest
- provider exact binary/support tuple qualification

test 失敗時は原因を特定し、contract を弱めずに修正します。flaky、環境差、既知警告として根拠なく無視しません。
固定 fixture と contract checker だけで production qualification を宣言せず、runtime test、negative test、independent consumer、
real-project evidence を組み合わせます。

## Commit, push, issue closure

1つの GitHub issue を1つの active write unit とし、issue、contract ID、repository-relative write path を宣言します。
同時 active unit は最大四つまで許可しますが、異なる unit の contract ID は disjoint、write path は同一・祖先・子孫関係を含めて
非重複でなければなりません。shared authority/contract、依存実装、同じ path prefix を所有する作業は直列化します。

対象差分だけを commit して branch へ push し、PR を作成します。各 PR の exact-head required checks、未解決 review の解消、
branch protection、bounded conflict-scoped active-unit invariant を確認した後に merge します。protected `main` は PR workflow だけで
更新します。

production scope に tracked gap がある intermediate unit の merge 後は、exact merged-main SHA の required checks、Foundation、
Wave 0、G5、`release-evaluation`、normal production-scope report を確認します。`release-evaluation: not-qualified` は評価器の
fail-closed success だけを意味し、`gate.release`、GR、production support を満たしません。その unit が所有する surface の
classification/evidence、completion evidence、learning checkpoint が揃った後に issue を閉じます。

全 tracked gap の解消後は `release-evaluation: qualified`、strict GR report、final-mode production-scope report を同じ exact
merged-main SHA で確認します。複数 issue の無関係な変更を1 commit にまとめません。

issue には完了前に次の evidence をコメントします。

- commit SHA と変更した authority/contract/catalog/use-case ID
- production implementation または documentation-only boundary
- positive/negative/real-project tests
- result states、error/unresolved/coverage/guarantee/completion plan
- constructibility witness（該当する場合）
- 実行した build/test/quality command と CI run URL
- deferred 項目と follow-up issue
- learning checkpoint と `issue-ready` 結果
- 完了判定の根拠

DoD を満たした場合だけ `completed` として閉じます。未実装、未検証、未認定の作業を `not planned` で隠しません。

## CI monitoring and progress

各 unit の merge 後と全対象 issue の完了後に、exact merged-main SHA の required CI を監視します。失敗した場合は job、step、
log、artifact を調査し、根本原因を修正します。新しい SHA の全 required CI が緑になるまで継続します。過去 SHA の成功を最終
SHA の evidence として流用しません。

作業中は日本語で簡潔に、現在の use case/capability/issue、完了事項、根拠、検証、残作業、blocker、design feedback を報告します。
生の長大な log ではなく、結論と証拠を要約します。authority から決定できない重大な public semantics だけをユーザーへ確認し、
それ以外は安全で reversible な判断を行います。

## Final completion criteria

次をすべて満たすまで goal を完了しません。

- supply inventory と demand inventory が exactly once に分類されている
- orphan public surface と orphan admitted use case がない
- admitted use case が executable capability path または明示 tracked gap を持つ
- required capability/API が implementation と public surface を持つ
- semantics、identity、partiality、versioning、result states が明示されている
- `unknown` が不足 reason と dependency-ordered completion plan を持つ
- high-risk contract が constructibility witness と independent review を持つ
- schema、validator、positive/negative/real-project test が存在する
- typed/dynamic、backend、provider surface の必要 parity が成立する
- example、install consumer、Doxygen、catalog、traceability が揃う
- G5 と release qualification を含む mandatory gate が evidence 付きで完了する
- final production-scope report が `qualified` である
- production-supported provider tuple が exact digest と qualification を持つ
- deferred/planned 作業が追跡 issue なしで残っていない
- 全 implementation issue が learning checkpoint を持ち、未解決 blocking design feedback を残していない
- stable capability/API が stable admission 条件を満たす
- final `main` SHA の required CI がすべて成功する
- `HEAD` と `origin/main` が一致し、worktree が clean である
- unsupported/future scope が明示され、実装済みと誤認されない
- product-value metrics が report される:
  - time to first trustworthy result
  - actionable unknown ratio
  - agent autonomous completion rate
  - real-project qualified use-case count
  - independent-consumer capability reuse
  - contract constructibility escape rate

完了時には、use-case/capability roadmap、実装・stability・qualification、issue/commit、test/CI evidence、
support matrix、残存非スコープをまとめた最終レポートを提出します。
