# ADR 0095: distribution 1.0 scope を typed graph と二段階 qualification で閉じる

- Status: Accepted
- Date: 2026-07-19
- Issue: #179
- Design feedback: DF-0180 / #180
- Amends: ADR 0091, ADR 0094
- Depends on: ADR 0088, ADR 0089, ADR 0090, ADR 0091, ADR 0092, ADR 0093

## Context

distribution 1.0 の既存 checker は Public API Catalog、callable inventory、Relation Registry、provider protocol、
Logical Query、security、Wave 0、G5、GR を各 domain 内で検証する。しかし release root から全 surface を列挙し、
各 item を一度だけ production-required、evidence、明示的 non-1.0 のいずれかへ分類する契約がない。

#173 の read-only audit は catalog 14 entry、public callable 536 件、relation 18 descriptor という局所 census が正しい一方、
release migration/gate、installed package matrix、provider NG1、query/closure、security/compatibility、Nightly と NG2/NG3 の
明示的除外がその census から欠落することを確認した。また Relation Registry の全 18 descriptor は generated tag を持つが、
Public API Catalog が static header を admit するのは 11 件だけであり、残る 7 件は DF-0174 が authority 未確定として block している。

同じ audit は既存 GR が actual-source installed Clang worker から snapshot までの evidence を通らず production tuple を生成できること、
NG1 provider hardening、production incremental materialization、latest Nightly に tracked gap があることも確認した。scope contract を
即座に strict GR input にすると #179 自身の main qualification が赤くなり、gap を許したまま従来 GR を生成すると false production
claim を温存する。inventory と qualification の dependency graph を循環させず、intermediate main と final release の意味を分離する
必要がある。

## Decision

### Release-rooted typed graph

`cxxlens.ng-production-scope-closure.v1` を distribution 1.0 scope の machine-readable closure contract とする。
Release Bundle は release disposition の root のまま維持し、各 source contract は item ID と semantics を所有する。scope contract が
所有するのは typed reference、release disposition、cross-link、remediation/evidence mapping だけであり、source payload を複製して
第二の semantic authority にしない。

node identity は `(domain, id)` とする。異なる domain の同じ文字列は別 node である。relation descriptor、provider relation requirement、
generated header、catalog entry、callable、production tuple template は typed edge で結び、string alias として collapse しない。
exact-once は assignable leaf node の typed namespace 内で適用する。aggregate node と evidence census は leaf から導出し、別 owner を
付けて二重計上しない。

aggregate derivation は次の accepted rule に固定する。aggregate row は `assignment`、`owner_issue`、leaf の `scope` / `qualification`
field を持たず、leaf qualification totals と closure count に加算しない。代わりに canonical `derived_from` と
`derived_scope` / `derived_qualification` を出力する。

- required NG profile は provider feature/message の typed profile cross-link から導出し、Release Bundle が 1.0 非 blocker とした
  NG2/NG3 は `excluded/not-applicable` と導出する。
- acceptance gate は manifest の `depends_on` と profile edge を基礎とする。`gate.quality-evidence` は全 `quality.check` leaf、
  `gate.g5` は全 `g5.closure-kind`、`public.incremental`、R4、`gate.release` は全 included/unresolved assignable leaf を追加 member とする。
- member に `tracked-gap` または `blocked` があれば aggregate は `included/tracked-gap`、qualified member のみなら
  `included/qualified`、明示非 1.0 member のみなら `excluded/not-applicable` と導出する。aggregate の derived state は leaf の
  authority blocker を消さず、独立 qualification を付与しない。

v1 の closed enumerator set は accepted Release Bundle とこの ADR が固定し、manifest が source list を自由に減らせないよう
Release Bundle、schema、checker の同一 const set で bind する。exact namespace ID set は次の 30 件である。

```text
release.profile
release.gate
release.migration
distribution.target
distribution.native-package
distribution.installed-tool
distribution.consumer-configuration
distribution.source-surface
public.package
public.author-path
public.catalog-entry
public.header
public.callable
relation.descriptor
relation.static-admission
provider.profile-feature
provider.message
provider.execution-surface
provider.support-tuple
provider.production-tuple-template
query.operator
query.result-status
query.backend
g5.closure-kind
security.profile
security.namespace
security.sandbox-profile
security.sandbox-policy
compatibility.axis
quality.check
```

whole namespace の追加・削除・rename は manifest assignment ではなく accepted authority とこの const binding の versioned update を要求する。

qualification level、reason code、conformance vector、G5 exact-input axis、artifact list は source checker が semantics を所有する evidence
census とする。report は count/digest を保持するが、各 enum/vector を独立 remediation surface に昇格させない。public callable は 536 行を
manifest に複製せず、各 row の exact catalog owner から assignment を継承し、expanded report で全 callable node を出力する。

### Scope disposition と qualification state の分離

source から導く release disposition と evidence state は次の exhaustive disjoint partition とする。

```text
canonical source set
  = production-required
  ⊎ qualification-evidence
  ⊎ explicit-non-1.0
  ⊎ unresolved-authority

production-required ⊎ qualification-evidence
  = evidence-qualified ⊎ tracked-gap

explicit-non-1.0 = not-applicable
unresolved-authority = blocked
```

manifest assignment は `scope: included | excluded | unresolved` と
`qualification: qualified | tracked-gap | blocked | not-applicable` を別 field として持つ。

- production-required / qualification-evidence node は `included` であり、independent evidence mapping がある場合だけ `qualified` にできる。
- release authority が NG2/NG3、R5-R7、C++ module 等を 1.0 非 blocker と明示した node だけを `excluded/not-applicable` にできる。
- authority が public admission を決めていない node は `unresolved/blocked` とし、active blocking DF を参照する。
- `status: implemented`、source checker green、manifest の自己申告だけでは evidence-qualified にならない。

valid pair は `(included, qualified)`、`(included, tracked-gap)`、`(excluded, not-applicable)`、
`(unresolved, blocked)` の四つだけである。schema と checker はその他の組合せを拒否する。final mode は explicit non-1.0 の
`excluded/not-applicable` を保持できるが、`tracked-gap` と `unresolved/blocked` を一件も許さない。

全 leaf は一つの assignment にだけ属する。unknown、missing、duplicate、whole-namespace omission、unsupported exclusion、source drift は
fail closed とする。checker は assignment より先に repository の全 design-feedback record を census し、`observed`、`investigating`、
`proposed` かつ `implementation_disposition: blocked` の record を applicable blocking set とする。この default set から除外できるのは、
record の affected node が release authority 由来の `explicit-non-1.0` node だけであることを独立に証明する typed feedback exclusion がある
場合だけである。assignment は applicability を決めない。

一つの applicable DF が複数 node を block することは許すが、全 applicable DF は少なくとも一つの `included/tracked-gap` または
`unresolved/blocked` assignment から参照されなければならない。manifest が relevant DF を黙って scope 外にすると failure になる。
DF issue close や record 自体は resolution authority にならない。

### Acyclic evidence and truthful rollout

qualification DAG を次で固定する。

```text
accepted authorities
  -> static scope inventory/classification
  -> Foundation and Wave 0 inventory binding
  -> G5 exact-SHA evidence
  -> release evaluation
  -> terminal production-scope report
```

Foundation は ADR 0088 の G0-G4/R0-R3 boundary を維持し、G5/GR を逆参照しない。Wave 0 は manifest、source census、classification digest を
exact revision/tree に bind するだけで production qualification を付与しない。GR は terminal scope report を consume せず、Wave 0 binding と
G5/install/security/documentation evidence から release evaluation を作る。terminal scope report は release evaluation と、qualified 時だけ
存在する GR report を consume する。この向き以外の digest edge を禁止する。

既存 `cxxlens.ng-release-qualification-report.v1` の意味は変更しない。これは全 prerequisite と全 required scope が qualified の場合だけ
`release-qualification` job が生成し、exact production tuple だけを持つ。intermediate main では別名の
`release-evaluation` job が新しい `cxxlens.ng-release-qualification-evaluation-report.v1` artifact を常に生成する。

- evaluation checker が source/evidence を正しく評価できた場合、`release-evaluation` job は green になり得る。
- tracked gap または blocking authority が一件でもあれば evaluation は `not-qualified`、production tuple は空、既存 GR v1 は生成しない。
- gap がゼロで exact evidence が揃う場合だけ evaluation は `qualified` となり、後続 `release-qualification` job が既存 GR v1 を生成する。

`release-evaluation: green/not-qualified` は `gate.release`、GR、production support、final completion のいずれも満たさない。
intermediate unit の merged-main closure は required checks、Foundation、Wave 0、G5、`release-evaluation`、normal terminal scope report の
fail-closed success を要求し、その unit が所有する surface を manifest で更新した後に限る。この rule は ADR 0094 と
`docs/development/agent-api-development-goal.md` の per-unit post-merge rule を明示的に amendment する。

従って per-unit merged-main workflow は評価器の fail-closed success を確認できるが、green evaluation を production release と読み替えない。
distribution 1.0 の final completion は terminal scope report の `closure_status: qualified`、evaluation `qualified`、既存 GR v1 の存在と exact
tuple をすべて要求する。gap がある期間の Release Bundle state は `qualification-in-progress`、`production_supported: false` とし、final unit
だけが scope/evidence と同時に `qualified` / `true` へ遷移できる。

### Reports and modes

scope checker は次を提供する。

- `check`: 任意 branch で schema、closed source set、census digest、assignment partition、cross-link、blocking DF accounting を検証する。
- `report --mode normal`: clean exact `main` の upstream evidence と `release-evaluation` artifact を bind し、明示 gap を含む完全な report を生成する。
- `report --mode final`: normal の全条件に加え、required/evidence node が全件 qualified、unresolved authority と blocking DF がゼロ、release
  evaluation と既存 GR v1 が qualified/exact であることを要求する。

report は out-of-tree artifact とし、revision、tree、clean main、mode、manifest/schema/checker/source digest、canonical node/edge digest、
classification digest、upstream evidence digest、stable finding ID を保持する。tracked file に自身の revision/tree/hash を埋め込まない。

## Consequences

- 既存 domain checker を一つの巨大 checker へ再実装せず、release scope の omission と二重 ownership を検出できる。
- callable 536 件や semantic payload の手書き複製を避けつつ、report では leaf ごとの owner/state と aggregate の derived state を監査できる。
- DF-0174 の 7 static-admission node だけを block し、18 relation descriptor の dynamic/system semantics を gap と誤分類しない。
- known U1-U4/Nightly gap は別 implementation unit のまま残り、#179 がその production implementation を取り込まない。
- intermediate main は正直な `not-qualified` evaluation を green job として保存できるが、production tuple や GR v1 を生成しない。
- final release は manifest の自己申告ではなく exact evidence と terminal report によってのみ成立する。

## Verification

positive test は全 typed source node の一意 partition、callable inheritance、relation/static-admission separation、normal-mode gap report、全 qualified
final report を検証する。negative test は whole namespace omission、source item addition、cross-domain name collision、duplicate ownership、
unsupported exclusion、evidence-free qualification、unmapped blocking DF、mixed revision/tree、stale digest、final mode の gap、GR tuple/template の
missing/duplicate、不正な conformance-only support promotion を個別に拒否する。
さらに unrelated CTest による自己 qualification、exact JUnit test omission、count/ID を維持した reason/vector/artifact semantic drift、aggregate の
assignment/owner 再導入、domain/source/evidence census key duplicate、GR と evaluation artifact digest の不一致を拒否する。

quality contract は static check を一度だけ所有する。Wave 0 は inventory binding、release evaluation は qualified/not-qualified 判定、terminal
job は expanded scope report を一度だけ生成する。PR exact-head と merged-main evaluation、独立 design/implementation review を完了する。
