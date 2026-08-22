# Extending the semantic relation platform

NG foundation 完了後の通常開発は kernel redesign ではなく、versioned contract と conformance evidence の追加です。
recipe 利用者、typed/dynamic query 作者、portable provider 作者、native provider 作者は同じ relation descriptor、
Logical Query IR、coverage、unresolved、guarantee、provenance を共有します。

## Constructibility before acceptance

Issue #276 の既存 authority は、`schemas/cxxlens_ng_api_development_readiness.yaml` とその versioned schema にある
`development.constructibility-gate.v1` の witness inventory です。現在の bounded checker
`tools/quality/check_quality_ownership.py constructibility` は、この既存 schema を検証し、#261 の agent context が同じ gate に
bound された `blocked` disposition のままであること、v1 の contract/applicability/witness/acceptance projection が pin と一致すること、
および manifest/schema の SHA-256 provenance を確認します。ファイル digest は provenance であり、v1 projection の trust anchor ではありません。

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

## High-risk contract witness requirements

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

### Bounded #277 context slice

Issue #277 の最初の disjoint slice は、既存の #261 `first_packet` template を入力として、#275 の
`cxxlens.ng-use-case-capability-catalog.v1` declaration projection、#276 の constructibility projection、DF-0261 の
blocked record を同じ exact revision/tree に bind する、schema-first の context/completion-plan projection です。generator
と schema は `tools/quality/check_ng_agent_context.py` と `schemas/cxxlens_ng_agent_context.schema.yaml` にあります。
ただし、この #277 出力は machine-readable な開発者向け projection であり、明示的に
`authority: non-authoritative-projection`、`release_authority: none` です。

```sh
REV=$(git rev-parse HEAD)
TREE=$(git rev-parse HEAD^{tree})
python tools/quality/check_ng_agent_context.py plan \
  --use-case repository-semantic-query.explain-translation-unit.v1 \
  --issue 261 --expected-revision "$REV" --expected-tree "$TREE" \
  --output-json /tmp/cxxlens-ng-agent-context-issue-277.json \
  --output-markdown /tmp/cxxlens-ng-agent-context-issue-277.md
python tools/quality/check_ng_agent_context.py check \
  --use-case repository-semantic-query.explain-translation-unit.v1 \
  --issue 261 --expected-revision "$REV" --expected-tree "$TREE" \
  --input-json /tmp/cxxlens-ng-agent-context-issue-277.json
```

出力は dependency-ordered capability path、preserved semantics、tracked gap の reason/owner/reevaluation
trigger、completion plan、許可された write path、required evidence、known feedback、実在する authority reading file の
digest、DF-0261 の path/digest/status/disposition/review/resolution refs、#275/#276 の状態、authority digest、canonical
digest を保持します。未知の use case、未登録または forward dependency、path overlap、dirty worktree、stale digest、
constructibility の promotion、machine authority と異なる contract ID は fail closed です。現在、完全な template が
authority にあるのは #261 だけであり、他の admitted family を推測して packet 化しません。

CI の `quality` workflow は二つの明示的な artifact lane を生成します。`agent-context` job が生成する
`cxxlens-ng-agent-context-261-${revision}`（`cxxlens-ng-agent-context-issue-261.json/.md`）は Wave 0 readiness の
authoritative artifact で、required #261/full gate の入力です。一方、独立した
`agent-context-projection` job が `check_ng_agent_context.py` で生成する
`cxxlens-ng-agent-context-277-${revision}`（`cxxlens-ng-agent-context-issue-277.json/.md`）は #277 の
non-authoritative projection です。projection job は authority に `workflow_job: agent-context-projection`、
`non_gating: true`、`failure_policy: continue-on-error` として記録され、workflow の `continue-on-error: true`、`needs` なし、
他 job からの依存なしを checker が検証します。したがって projection の失敗は advisory evidence として残りますが、required
`#261` lane、readiness report、release qualification、issue closure を失敗させません。artifact 名、packet schema、generator、
consumer を分けることで、二つの generator が同じ authority を曖昧に競合しないようにしています。

`check_ng_agent_context.py` が #261 向け v1 non-authoritative projection の producer です。v1 packet は読み取り互換として維持します。
全 open issue の実行選択には work-unit authority と v2 producer を使います。

```sh
python3 tools/quality/check_ng_work_units.py check --root .
python3 tools/quality/check_ng_agent_context_v2.py packet --root . \
  --issue 200 --unit wu-200-candidate-adoption-report
python3 tools/quality/check_ng_agent_context_v2.py corpus --root .
```

v2 packet は issue/unit の exact match、authority digest、current revision/tree、最小 reading set、write scope、
blocker、dependency-ordered completion plan、evidence command、残余 qualification を結合します。corpus の bounded
completion 選択は、登録済み issue/unit identity、dependency reference、packet の manifest/authority digest、actionable
field の非空性を packet と manifest の独立 projection で再照合し、unknown work unit を受理しません。dirty worktree、
unknown/foreign unit、authority drift は実行 packet を生成しません。共有 checksum/ledger は packet の write scope
から除外され、integration owner が生成・commitします。固定 corpus は安全停止率100%と bounded packet 完備率80%以上を要求し、
v1 の #261 exact packet `plan`/`check` を一時 clean-HEAD clone と一時出力ディレクトリで実行した measurable evidence を
`v1_issue_261_compatibility_evidence` に保持します。v1 producer/schema は変更しません。

### Canonical capability resolution and SDK doctor

`cxxlens.agent-capability-resolution.v1` is the canonical result contract consumed by v2 packets
and by SDK-doctor evaluation. It is produced by
`tools/quality/check_ng_agent_capability_resolution.py` from the exact source-bound corpus in
`schemas/cxxlens_ng_agent_capability_resolution.yaml`; relation presence alone is never used to
infer a use-case capability. Every result preserves the closed state set
`proved`/`disproved`/`unknown`/`partial`/`conflicting`, a typed missing reason, and a
dependency-ordered completion plan. Unknown results are safe stops, not empty success.

When checking a saved resolution with `check --input-json`, the checker binds the document to the
current revision/tree and every catalog source digest. A self-consistent packet from an older tree
is rejected as stale; `--synthetic` is reserved for the corpus's zero-authority fixtures and is not
execution evidence. The installed SDK doctor follows the same boundary: an input without an exact
authority object is reported as `unbound`, while a bound input must match the compiled authority.

The installed `cxxlens-sdk-doctor` exposes the same consumer-facing boundary:

```sh
cxxlens-sdk-doctor capability <use-case-id> [--format json|markdown]
cxxlens-sdk-doctor explain <resolution.json> [--format json|markdown]
cxxlens-sdk-doctor missing --project <project.json> [--format json|markdown]
```

JSON is the default output. Markdown is a deterministic projection of the same canonical
resolution and may not introduce or remove a result, reason, evidence, or completion step. The
doctor also emits an `authority` binding on capability, explain, and project-missing outputs.
When an input carries an authority object, the installed executable requires the exact
`revision`, `tree`, catalog `source`/`source_digest`, and aggregate `authority_digest` compiled
from the shipped readiness/catalog bytes; a mismatch returns the stable
`sdk.capability-authority-stale` failure. Legacy project inputs without that object are reported
as `status: unbound` and are never promoted to canonical capability evidence. This binding is
content/revision integrity only: it does not authenticate an agent invocation, evaluate semantic
execution, or qualify a release.
nine-path corpus covers relation, recipe, analysis, model, portable provider, native provider,
query operator, support tuple, and actionable unknown consumers. The corpus requires a 100%
safe-stop rate and exercises every result state; it is readiness evidence and does not by itself
promote a provider or platform tuple to production qualification. The legacy v1 producer remains
unchanged and v2 packets retain the exact v1 compatibility evidence.

Each golden path also carries an explicit demand edge to the admitted `agent-guided-extension`
family and its declared #277 capabilities. The checker reads that family from the #275 readiness
projection and rejects an unknown family or capability edge, so the evaluation corpus cannot
become a parallel handwritten demand authority.

The separate `agent-autonomous-completion-rate` metric is receipt-bound. Running
`tools/quality/check_ng_agent_autonomous_completion.py report` without an execution-evidence
file deliberately returns the exact nine-scenario population as `not-evaluated` with a null
rate. A numeric rate is emitted only when every scenario has an exact current revision/tree and
catalog digest, a bounded-completion witness, and typed receipt/context/command digests. Failed
and safe-stop outcomes remain in the denominator. Evidence v2 additionally requires each evaluated
outcome to carry the current canonical capability-resolution context, an argv/environment command
receipt, a closed process terminal/stdout/stderr receipt, and a typed result receipt. The checker
recomputes the context, command, completion-plan, and whole-witness digests and applies the same
cross-binding when checking a saved report; digest-shaped placeholders alone are rejected. These
fields prove internal content binding, not that the observer is authenticated or that the reported
execution is semantically correct. This metric reports evaluation state only;
`qualification: not-qualification-evidence` is fixed by schema and it cannot promote a provider,
consumer, support tuple, constructibility gate, or release.
An input file containing only `not-evaluated` rows remains `not-evaluated` and is recorded with
`evidence_source: none`; no digest-shaped or synthetic row can promote the metric to `evaluated`.

The bounded command runner is `tools/quality/run_ng_agent_autonomous_completion.py`. It accepts the
versioned `cxxlens.agent-autonomous-completion-runner-input.v1` manifest, requires the exact current
revision/tree/catalog digest and the canonical nine-scenario order, then executes each declared argv
without a shell in one temporary clean clone reset to the exact revision before every scenario. The
runner uses a source-bound 900-second timeout, records byte-exact stdout/stderr and terminal status,
and accepts `completed` only when the command emits an exact
`cxxlens.agent-autonomous-completion-result.v1` JSON receipt for that scenario. Invalid output,
non-zero exit, launch failure, signal, or timeout remains a typed `failed`/`safe-stop` outcome with
the real process receipt and completion plan. The temporary clone is discarded; no command can
modify the authority checkout. For example:

```sh
python3 tools/quality/run_ng_agent_autonomous_completion.py run \
  --root . \
  --input-json runner-input.json \
  --output-evidence agent-completion-evidence.json \
  --output-report agent-completion-report.json
```

This runner supplies executable process and content-binding evidence, but does not authenticate the
invoked agent or prove semantic correctness of a claimed bounded change. The resulting metric remains
evaluation-only and cannot satisfy constructibility, review, provider, platform, or release
qualification. A caller without a real command/result receipt must leave the metric `not-evaluated`.

The `quality` workflow publishes the current metric state as
`cxxlens-ng-agent-autonomous-completion-${revision}` after rechecking the report against the
checkout's exact revision and tree. This artifact improves provenance for the #277 readiness
audit; it is still evaluation-only, and a report without real execution receipts remains
`not-evaluated` with a null rate.

静的 relation inventory を残す場合の契約名は `relation-presence` です。これは canonical capability resolution の
use-case/capability graph、support tuple、input/model/evidence gap と dependency-ordered plan を読む `missing --project`
とは別機能であり、相互代用しません。

legacy v1 projection の producer は引き続き `check_ng_agent_context.py` だけです。`check_ng_api_development_readiness.py`
は #261 readiness artifact の authority/generator であり、readiness document と workflow を検証します。#277 projection の出力は
Issue #261 の source-closure implementation や #276 の constructibility acceptance を進めるものではなく、
source-closure/VFS、provider qualification、real-project evidence、aggregate qualification、`agent-autonomous-completion-rate`
の測定、Nightly の release qualification は別の accepted slice が必要です。この generator の出力だけで constructible、
qualified、production-ready を主張してはいけません。

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
