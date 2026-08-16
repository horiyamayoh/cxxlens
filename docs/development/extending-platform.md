# Extending the semantic relation platform

NG foundation 完了後の通常開発は kernel redesign ではなく、versioned contract と conformance evidence の追加です。
recipe 利用者、typed/dynamic query 作者、portable provider 作者、native provider 作者は同じ relation descriptor、
Logical Query IR、coverage、unresolved、guarantee、provenance を共有します。

## Constructibility before acceptance

Issue #276 の既存 authority は、`schemas/cxxlens_ng_api_development_readiness.yaml` とその versioned schema にある
`development.constructibility-gate.v1` の witness inventory です。現在の bounded checker
`tools/quality/check_quality_ownership.py constructibility` は、この既存 schema を検証し、#261 の agent context が同じ gate に
bound された `blocked` disposition のままであることと、manifest/schema の SHA-256 provenance を確認します。

これは witness inventory の disposition projection に限った preflight です。phase graph、phase ごとの field availability、
phase-authentic outcome union、resource/crash/recovery witness の実装や、high-risk contract の acceptance は追加しません。
`blocked` の DF を `constructible` または `not-applicable` に昇格せず、#191/#194/#195/#198/#199/#200/#201/#202/#205/#261 の
authority・implementation disposition も変更しません。そうした実装には、該当 contract、独立 review、learning checkpoint
を先に受理する必要があります。

## Start from a use case

拡張の開始点は API や relation の案ではなく、独立 consumer が答えたい質問です。

1. consumer と use case を一意な ID で宣言する。
2. 現在の capability graph で、input capture、relation、provider、analysis、model、query、recipe、evidence までを逆引きする。
3. 足りない capability を `missing`、authority 未解決を `blocked`、将来追加を `tracked-gap` として明示する。
4. 新 surface が既存 use case に使われるか、独立 consumer の vertical slice で不可避かを確認する。
5. use case から executable result までの需要側 closure と、public surface から consumer/evidence までの供給側 closure を両方検査する。

orphan public surface と orphan admitted use case は、どちらも completion を満たしません。API 数、relation 数、checker 数を
増やすこと自体を成果にしません。重複 API や根拠のない convenience surface は追加せず、既存 capability の合成で足りる場合は
新規 API を作りません。

最初の real-project substrate は source closure / capture / replay です。その後は semantic graph、
CFG/control exit、use-def/value flow、alias/effect/invalidation、interprocedural summary/model packs、
proof-carrying rewrite/artifacts、cross-provider semantic consensus の順で共通再利用価値を優先します。
この順序は個別 issue の accepted authority を先取りせず、各能力は versioned capability として導入します。

## Result and actionable unknown

recipe/analysis の product-level result は次を区別します。

```text
proved
disproved
unknown
partial
conflicting
```

結果は row だけでなく、coverage、closure、unresolved、conflict、differential disagreement、producer contract、
guarantee、provenance、logical/physical explain を保持します。`unknown` は generic failure ではなく、少なくとも次を返します。

```text
missing inputs
missing capabilities
missing models or assumptions
missing qualification evidence
dependency-ordered completion plan
```

completion plan は「何を追加・有効化・再取得・再実行すれば、どの結果軸が強化されるか」を機械可読にします。
不足を隠して empty success、safe、no finding、unsupported omission へ畳みません。

## Capability packs and maturity

core は identity、claim、snapshot、query、provider、partiality の安定核に保ち、領域機能は relation/provider/analysis/model/recipe の
versioned capability pack として追加します。例:

```text
cpp.graph
cpp.call
analysis.flow
analysis.effect
analysis.safety
change
rewrite
abi-ir-binary
models.*
```

capability は原則として `prototype → experimental → versioned → stable → production-qualified` を通ります。
stable admission は二つ以上の独立 consumer、または不可避な foundational invariant と十分な experimental evidence を要求します。
production support は exact support tuple、real-project、negative/fault、relocation、resource evidence に限定し、
implemented や schema-admitted だけから推測しません。

## Constructibility before acceptance

public semantics、identity、protocol、persistence、不可逆 effect、resource bound を変える high-risk contract は、実装開始前に次を揃えます。

- executable state machine
- field availability by phase
- phase-authentic success/failure outcome union
- minimal witness implementation
- finite resource witness
- crash/effect matrix
- independent counterexample review

report field は、その phase で観測可能な値だけを要求します。decode 前の失敗に decoded identity、publish 前の report に
post-commit receipt のような値を要求しません。構成不能な contract を test fixture の sentinel や fabricated identity で通しません。

## New relation or public semantic API

1. consumer/use case、capability gap、需要側 closure を先に定義する。
2. semantics、identity、condition/interpretation、coverage/closure、evolution rule を定義する。
3. relation registry と schema を更新し、validator の positive/negative vector を追加する。
4. IDL compiler から typed header を生成し、dynamic lookup と同じ descriptor/column ID を使う。
5. public API catalog、Doxygen、traceability、example、install consumer を更新する。
6. memory/SQLite、typed/dynamic IR、挿入順/root/jobs の parity を確認する。
7. consumer vertical slice と support/stability disposition を記録する。

central enum/switch、pretty type string identity、opaque custom payload、unordered iteration 由来の ID は追加しません。
standard relation と custom relation は同じ registry/claim/query path を通します。

absence、difference、recursive relation を追加する場合は positive operator と同じ扱いにしません。absence は対象 subtree の
coverage と closure kind/domain を明記し、前提不足を structured unresolved として返します。incremental service は source だけで
なく provider semantics、descriptor、condition universe、model/assumption、precision を含む全 invalidation input を列挙します。
recursion は iteration/edge budget、truncated positive の保持、closure 非認定を negative test で固定します。

## New recipe or analysis module

recipe は public query/C++ semantic contract だけを利用し、kernel-private surface や native compiler object に依存しません。
新規 recipe/analysis には exact relation/capability requirements、deterministic plan/Logical Query IR、budget/cancellation、
result-state policy、actionable unknown、positive/negative fixture、installed example、semantics version を揃えます。

analysis framework は solver 固有の状態を kernel enum に埋め込まず、domain schema、bottom/join/order/widen、transfer provider、
precision、convergence/budget、assumptions、derived relation output を versioned contract にします。model pack は実装コードと独立して
versioning、trust、provenance、conflict、unknown external effect を持ちます。

## New portable provider

`cxxlens::provider_sdk` の manifest、typed detached value、relation sink、coverage/evidence builder、production codec と同じ
test harness を使います。filesystem、process、time、hash は port 越しに扱い、provider selection、binary digest、
toolchain、variant、sandbox outcome を明示します。新 relation/provider の追加で core source list や enum/switch を
変更してはいけません。

## New native provider

native provider は compiler major ごとの package/process に閉じ込めます。AST/TU pointer は callback-scoped borrowed
object であり、保存、所有、別 thread への移送を禁止します。source range と semantic observation は worker 内で
detached value に正規化し、portable protocol boundary を越える前に validation します。

native package には exact toolchain configure failure、lifetime compile-negative test、crash/timeout/cancel、
malformed/oversized output、sandbox report、prior snapshot survival の evidence が必要です。real-project input は
ambient host filesystem の偶然の成功へ依存せず、accepted capture/replay policy に従います。

## Agent context

coding agent の task packet/context card は、少なくとも次を持ちます。

```text
goal and admitted use case
capability gap and dependency path
exact contract IDs
minimum authority reading set
allowed write paths
required positive/negative/real-project evidence
known design feedback and disposition
forbidden shortcuts
completion commands
```

手書き要約を新しい shadow authority にせず、catalog/registry/readiness/issue から生成します。不足 field を agent に推測させず、
blocked reason として返します。

## Completion checklist

- consumer/use case と capability dependency path を固定した
- 供給側 closure と需要側 closure を確認した
- relevant mental model と unresolved design feedback を開始前に確認した
- high-risk contract の constructibility witness を acceptance 前に確認した
- schema/invariant、identity、validator、tests、service の順で実装した
- public header/signature/ownership と error/unresolved/coverage を定義した
- ID/order/version compatibility と unknown handling を固定した
- actionable unknown と dependency-ordered completion plan を追加した
- positive/negative/conformance/real-project/install example を追加した
- API catalog、relation registry、Doxygen、設計 traceability を更新した
- agent context が goal、authority、write scope、evidence を推測させない
- learning checkpoint に `none` または関連 DF ID を記録し、未解決 blocker を残していない
- `ctest` と `cxxlens-quality` が green である
- support matrix は conformance と production support を混同していない

foundation 自体の完了根拠は
`schemas/cxxlens_ng_foundation_completion_manifest.yaml` と、同一 commit/tree の CI が生成する
`cxxlens-ng-foundation-completion-report.json` です。
