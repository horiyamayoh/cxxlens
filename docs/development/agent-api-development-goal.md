# Agent-driven public API development goal

この文書は、Codex の `/goal` と複数のコーディングエージェントを使って、cxxlens を継続開発するための実行契約です。
次の短い goal からこの文書を参照します。

```text
/goal docs/development/agent-api-development-goal.md を実行契約として CXXLENS_AGENT_AUTHORIZATION_V1 を適用し、cxxlens を需要側・供給側の双方で閉じた証拠付き意味知識基盤へ育て、issue 単位の小さな commit を default branch に反映し、最終 SHA の CI evidence まで継続してください。
```

## 2026-08-16 delivery amendment

通常の repository delivery は **direct-to-main** とする。feature branch、pull request、merge queue は routine implementation の
既定条件ではない。この節は delivery workflow に限って、ADR 0094 と過去の goal/issue prose にある branch/PR 必須の順序を置換する。
製品 semantics、security boundary、bounded implementation completion、exact-SHA integration/release qualification は弱めない。

- current `main` SHA と active unit の contract/path conflict を書き込み直前に確認する。
- issue scope 内の変更を小さく可逆な atomic commit にする。
- 変更に対応する focused build/test/checker を事前に実行する。
- canonical repository は fast-forward だけで更新し、history rewrite を行わない。
- 更新後の exact `main` SHA に対する CI を integration evidence とする。
- 変更起因の CI failure は follow-up commit で修正し、公開済み commit を改変しない。
- issue close evidence は commit SHA、bounded acceptance、残余 gap、Learning checkpoint を含む。
- PR は external contribution、high-risk independent review、または安全な隔離が必要な例外に限る任意経路とする。

- `delivery-route: main-atomic-commit-post-update-ci`
- `pull-request: optional-risk-or-external-contribution`
- `history-policy: fast-forward-no-rewrite`

## Autonomous execution and approval boundary

上の短い `/goal` のように、この文書と policy ID を明示参照した goal の実行中だけ standing authorization を有効にします。
単なる質問、診断、read-only review、またはこの実行契約を参照しない依頼から暗黙に有効化しません。ユーザーは実行中でも
authorization をいつでも revoke または narrow でき、その後の操作は狭められた範囲に従います。

| 区分 | 実行境界 |
| --- | --- |
| Standing authorization | read-only audit、active unit 内の編集・生成・test/build、同一 issue の CI 根本修正、atomic commit、canonical repository の fast-forward update、active issue の更新、必要時の任意 PR 作成・review 対応・merge、bounded implementation evidence と learning checkpoint 後の active issue close は再承認不要。明示的な integration/readiness/qualification issue は自身の qualification evidence まで満たす |
| Notify and continue | 当初想定外の supporting test/file が必要でも、同一 contract・同一 issue 内で可逆なら、原因、追加 scope、検証方法を commentary で通知して継続します。これは approval gate ではありません |
| Fresh user approval | destructive operation/history rewrite、branch protection/ruleset 変更、secret/permission 追加、課金、外部 production deploy、active workflow 外の顧客・第三者への連絡、ユーザー変更との解消不能な競合、authority で決められない重大な public semantics は停止します。対象、effect、不可逆性または rollback を開示し、exact target/effect に限定した承認を得ます |
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
- `fresh-approval-reuse: forbidden`
- `revocation: user-anytime`
- `completion-class: bounded-implementation`
- `production-qualification: not-claimed-by-default`
- `issue-close-owner: bounded-issue-or-explicit-qualification-gate`
- `aggregate-qualification-owner: exact-merged-main-integration-readiness-release`
- `reopen-condition: bounded-acceptance-or-scope-regression-only`

次の二つは frozen Wave 0 baseline の移行互換性だけのために残す **non-normative legacy checker token** です。
delivery の意味を持たず、agent は token から branch/PR 必須運用を復活させてはなりません。

- `protected-main: unit-branch-pr-exact-head-review-merge-exact-merged-main`
- `direct-main: prohibited`

skill が一般的な explicit approval を要求しても、操作が active policy の standing-authorization 範囲に明示されていれば、
goal 開始時の承認で満たされたものとします。skill の診断、focused plan、結果報告は実行しますが、その approval のためだけに
会話を停止しません。skill がより具体的な安全条件を持つ場合、または操作が列挙範囲外なら、その条件または fresh-approval gate を
維持します。

standing authorization は repository 内の active unit を越える mutable authority を与えません。fresh approval は開示した
exact target/effect にだけ有効で、別 target、別 effect、後続操作へ categorical に流用しません。platform approval も別の
capability gate であり、この contract は迂回しません。

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

製品完成は両側の orphan がゼロであることを要求します。API 数、relation 数、green check 数、open issue 数だけを
完成の根拠にしません。

## Product result contract

recipe、analysis、inspection、transformation preflight は、少なくとも次を区別します。

```text
proved
disproved
unknown
partial
conflicting
```

結果は coverage、closure、unresolved、conflict、differential disagreement、producer semantic contract、
guarantee and assumptions、provenance and evidence、logical and physical explanation を損失なく保持します。

`unknown` は generic failure または terminal prose ではありません。why unknown、missing input、missing capability、
missing model or assumption、missing qualification evidence、dependency-ordered completion plan を machine-readable に返します。
不足を empty success、safe、no finding、unsupported omission へ畳みません。

## Consumer and use-case authority

consumer は人間の人数ではなく、公開 contract を利用する独立コードベースです。独立 consumer として数えられるものは、
API 実装とは別に変更・リリースされる application、provider、recipe、analysis module、model pack、package です。
同一 repository 内で実装変更に追従する unit test、fixture、example は qualification evidence にはなりますが、
独立 consumer には数えません。

新しい capability/API を開始する前に、use-case ID、consumer、question、expected states、input/capture requirements、
required capability path、coverage/closure/partiality、satisfied/missing/blocked disposition、exact authority/contract/write scope、
acceptance evidence、support/stability tier、actionable unknown と completion plan を roadmap または issue に記録します。

stable API は、二つ以上の独立 consumer、または不可避な foundational invariant、実装、acceptance fixture、
error/partial semantics、lifetime/threading/order、versioning、performance characteristics、lower-level escape path、
experimental period が揃うまで stable と宣言しません。

## Capability roadmap

開発は kernel surface の増加ではなく、再利用可能な versioned capability pack を垂直に完成させます。

1. source closure / capture / replay による real-project substrate
2. declaration/reference/include/macro/inheritance/override/template を含む semantic graph
3. CFG と control exit
4. use-def と value flow
5. alias、read/write/escape、lifetime/invalidation effect
6. interprocedural summary と versioned model/assumption packs
7. proof obligation、immutable plan、overlay verification、journaled transaction を持つ transformation/artifact
8. Clang/GCC/LLVM IR/object/binary 間の cross-provider semantic consensus

各能力は relation/provider/analysis/model/recipe の versioned contract として authority-first に導入し、
core enum/switch や opaque payload へ用途固有意味を押し込みません。

## Authority

作業開始時に repository root の `AGENTS.md` を読み、常に次の authority 順序を守ります。

1. `docs/design/cxxlens_next_generation_integrated_design_ja.md` の原則と invariant
2. Relation Registry、Provider Protocol、Public C++ API Catalog、Acceptance Manifest、Security Profile、release bundle
3. accepted ADR と担当 GitHub issue の exact contract
4. acceptance fixture と実装
5. `docs/archive/` の履歴資料

delivery workflow だけは本書の 2026-08-16 amendment と root contract が、過去の branch/PR 必須 prose を置換します。
archive、旧124 API catalog、旧 freeze、過去の実装都合を新規 API の authority にしません。core abstraction、identity、
condition、truth、closure、protocol major、snapshot format、native lifetime、sandbox、mutation、determinism を変更する場合は、
実装前に ADR を作成します。

## Implementation Learning and Design Feedback

normative document は明示的に置換されるまでは現在の正しい契約です。実装事実と contract の矛盾、hidden assumption、
再利用可能な mental model、public contract の有力な改善案を発見した場合は
`docs/development/implementation-learning/README.md` に従います。

- 専用 GitHub issue と non-normative design feedback record に observation、working model、evidence、alternatives を保存する。
- correctness、security、invariant、compatibility、不可逆な変更は解決まで対象実装を block する。
- local/reversible change は self review、high-risk normative change は著者と異なる reviewer の反証 review を要求する。
- accepted record 自体を authority にせず、ADR/contract/catalog/test/traceability を先に更新する。
- issue 完了時に `Learning checkpoint: none` または関連 DF ID を evidence に残す。

raw work log を全 issue に義務化しません。material な知見だけを record にし、reusable な accepted insight は
curated mental model へ反映します。未解決 blocking record を持つ implementation issue は閉じません。

## Constructibility gate

public semantics、identity、protocol、persistence、不可逆 effect、resource bound を変更する high-risk contract は、
acceptance 前に executable state machine、field availability by phase、phase-authentic outcome union、minimal witness、
finite retained-memory/I/O/open-file/time resource witness、crash/effect/recovery matrix、independent counterexample review を要求します。

report field はその phase で観測可能な値だけを要求します。fixture sentinel、fabricated identity、prose parsing、
silent fallback で構成不能性を隠しません。

## Execution loop

1. current authority、active issues、CI、tracked gaps、open design feedback を audit する。
2. 需要側 impact と dependency order から、競合しない最小 active unit を選ぶ。
3. issue に exact contract ID、allowed write paths、acceptance evidence、completion commands を固定する。
4. scope 内で implementation、tests、docs、validator を完成させる。
5. focused checks を実行し、self review または必要な independent review を行う。
6. current `main` SHA を再確認し、競合がなければ atomic fast-forward commit を反映する。
7. updated exact SHA の CI を確認し、変更起因 failure は follow-up commit で修正する。
8. bounded completion evidence と learning checkpoint を issue に残し、残余 gap を別 owner へ route する。
9. integration/readiness/release owner は exact `main` SHA の aggregate evidence を一度だけ所有する。

複数 active unit は contract ID と repository-relative write path が競合しない場合だけ並行できます。
同一・祖先・子孫 path、同じ contract、共有 authority の変更は直列化します。

## Issue completion and qualification boundary

通常の implementation issue の既定完了クラスは **bounded implementation completion** とします。
issue を閉じるために distribution 全体の production qualification を再実行・再証明してはなりません。

bounded implementation completion は次を要求します。

- exact contract と宣言 scope の実装が完成し、placeholder、silent fallback、既知 blocker がない
- 変更した振る舞いと直接 dependency closure の positive/negative test が成功する
- 必要な determinism/resource/error evidence が成功する
- 直接影響する contract、schema、catalog、Doxygen、example、documentation、generated inventory が整合する
- scope 外 work に owner、依存順、完了条件がある
- issue evidence に `production qualification: not claimed` または限定 qualification claim を記録する
- Learning checkpoint を `none` または関連 DF ID として記録する

通常の issue に全 static/shared matrix、全 installed consumer、native/platform matrix、`full`/`stress`、Nightly、
release evaluation、terminal production-scope closure、無関係な gate/issue の完了を要求しません。

既存 checker と evidence schema の qualification vocabulary として、
Foundation、Wave 0、G5、`release-evaluation`、normal/final、および
exact merged-main SHA の required checks と fail-closed evidence という表現を保持します。
ここで `merged-main` は PR merge 限定ではなく、更新後の `main` 上で評価対象となる exact SHA を意味します。
全 tracked gap の解消後は `release-evaluation: qualified` を要求し、
final-mode production-scope report を同じ exact SHA に束縛します。
過去 SHA の成功を最終 SHA の evidence として流用しません。

後続 integration failure はまず gate owner に記録します。閉じた implementation issue を reopen するのは、
その failure が当該 issue の bounded acceptance を誤りと証明した場合、または当該 scope の regression を示した場合だけです。
製品全体が未認定であること自体は reopen 理由にしません。

## Required implementation rules

- C++23 を使用し、公開 namespace/type/function は設計書の lower snake case に従う。
- 通常の public header に `clang::*`、`llvm::*` または LLVM/Clang header を露出しない。
- schema-first の順序は semantics/invariants、identity、value types、schema、validator、tests、service とする。
- filesystem、process、time、hash は port 越しに扱う。
- AST pointer を保存、所有、別スレッドへ移送しない。raw owning pointer を導入しない。
- unordered container の iteration order を serialization や ID に使用しない。
- read result は empty と unresolved を区別し、evidence/coverage/guarantee を落とさない。
- `unknown` は不足 input/capability/model/evidence と completion plan を失わない。
- mutation/generation は plan、独立 validator、dry-run、transaction の順を崩さない。
- public API/relation/provider を変更したら次世代 catalog/registry、Doxygen、acceptance test、設計 traceability を更新する。
- implementation issue の完了時に learning checkpoint を行う。

## Forbidden shortcuts

- name や pretty type string だけによる semantic identity
- compile command や variant の silent fallback/first-wins
- macro expansion range への直接 edit
- conflict、stale digest、variant、reparse failure の無視
- unsupported surface、consumer gap、unresolved capability の omission
- API 数や relation 数だけによる completion claim
- actionable な不足理由を持たない generic `unknown`
- diagnostic prose substring による制御
- shell command の文字列連結
- test に合わせた上位 contract の縮小
- design feedback を記録しない silent contract deviation
- routine implementation へ branch/PR を機械的に要求すること
- CI failure を隠すための history rewrite

## Commands and reporting

通常の issue は、issue に固定した affected target/test/checker を実行します。

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang --target <affected-targets>
ctest --preset dev-clang -R '<affected-tests>' --output-on-failure
python3 tools/quality/run_gate.py fast --preset dev-clang \
  --report build/dev-clang/fast-report.json
```

public contract/schema/documentation を変更した場合は、その変更に直接対応する validator/checker を追加します。
全体 `cxxlens-quality`、`check|full|stress`、install/native matrix、Nightly/release command は、
その qualification surface を issue が明示的に所有する場合だけ issue 完了条件に含めます。

各 completed unit の報告には、issue、contract ID、changed paths、commit SHA、focused validation、
post-update CI status、残余 gap、production qualification claim、Learning checkpoint を含めます。
