# Extending the semantic relation platform

NG foundation 完了後の通常開発は kernel redesign ではなく、versioned contract と conformance evidence の追加です。
recipe 利用者、typed/dynamic query 作者、portable provider 作者、native provider 作者は同じ relation descriptor、
Logical Query IR、coverage、unresolved、guarantee、provenance を共有します。

## New relation or public semantic API

1. semantics、identity、condition/interpretation、coverage/closure、evolution rule を先に定義する。
2. relation registry と schema を更新し、validator の positive/negative vector を追加する。
3. IDL compiler から typed header を生成し、dynamic lookup と同じ descriptor/column ID を使う。
4. public API catalog、Doxygen、traceability、example、install consumer を更新する。
5. memory/SQLite、typed/dynamic IR、挿入順/root/jobs の parity を確認する。

central enum/switch、pretty type string identity、opaque custom payload、unordered iteration 由来の ID は追加しません。
standard relation と custom relation は同じ registry/claim/query path を通します。

absence、difference、recursive relation を追加する場合は positive operator と同じ扱いにしません。absence は対象 subtree の
coverage と closure kind/domain を明記し、前提不足を structured unresolved として返します。incremental service は source だけで
なく provider semantics、descriptor、condition universe、model/assumption、precision を含む全 invalidation input を列挙します。
recursion は iteration/edge budget、truncated positive の保持、closure 非認定を negative test で固定します。

## New recipe or analysis module

recipe は public query/C++ semantic contract だけを利用し、kernel-private surface や native compiler object に依存しません。
result は row だけでなく empty/unknown/ambiguous、coverage、closure、unresolved、conflict、differential disagreement、
producer contract、guarantee、provenance、logical/physical explain を保持します。

新規 recipe には exact relation requirements、deterministic Logical Query IR、budget/cancellation、partial result policy、
positive/negative fixture、installed example、catalog の recipe semantics version を揃えます。

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
malformed/oversized output、sandbox report、prior snapshot survival の evidence が必要です。

## Completion checklist

- schema/invariant、identity、validator、tests、service の順で実装した
- public header/signature/ownership と error/unresolved/coverage を定義した
- ID/order/version compatibility と unknown handling を固定した
- positive/negative/conformance/install example を追加した
- API catalog、relation registry、Doxygen、設計 traceability を更新した
- `ctest` と `cxxlens-quality` が green である
- support matrix は conformance と production support を混同していない

foundation 自体の完了根拠は
`schemas/cxxlens_ng_foundation_completion_manifest.yaml` と、同一 commit/tree の CI が生成する
`cxxlens-ng-foundation-completion-report.json` です。
