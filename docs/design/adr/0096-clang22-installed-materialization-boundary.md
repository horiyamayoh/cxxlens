# ADR 0096: Clang 22 installed materialization を provider-owned JSON tool に閉じる

- Status: Accepted
- Date: 2026-07-19
- Issue: #182
- Design feedback: DF-0182 / #182; DF-0187 / #187
- Amends: ADR 0015, ADR 0038, ADR 0064, ADR 0091, ADR 0095
- Depends on: ADR 0010, ADR 0043, ADR 0044, ADR 0083, ADR 0084

## Context

`cxxlens-clang-worker-22` は actual source を parse し、provider-owned observation と canonical
`cc.entity`、`cc.call_site`、`cc.call_direct_target` を columnar provider protocol で出力できる。一方、installed host が worker の
private task codec と descriptor construction を正当に再現する machine contract はなく、process runtime の public result は検証済み
transcript の raw frame を保持するだけで、Store が採用できる immutable sealed dependency group を返さない。

この空白を test-only decoder や private header include で埋めると、wire validation を二重実装し、raw frame を semantic authority と誤認し、
実際の worker から memory/SQLite snapshot まで到達しないまま ADR 0091 の production tuple を生成できる。反対に Clang 固有の public C++
host bridge や一般化された public adoption API を追加すると、現在証明されている installed Clang 22 vertical だけのために stable SDK surface を
広げる。

追加監査では source base claim にも hidden assumption が見つかった。native normalizer は primary range の source snapshot、file、half-open
begin/end、semantic role、read-only、span ID と ordered origin chain を一度は持つが、既存 private observation v1 row は span ID と origin chain
だけを残す。`source_span_identity` は一方向 digest なので、host は span ID から begin/end、role、read-only を復元できず、worker 実行前の
base ingestion だけでも `source.span` を作れない。従って既存 observation v1 をそのまま Store adoption authority に昇格できない。

## Decision

### Installed machine surface

Clang 22 の installed end-to-end materialization authority を provider-owned executable
`cxxlens-clang22-materialize` に置く。tool は strict versioned JSON の
`cxxlens.clang22-materialization-request.v1` を受け、
`cxxlens.clang22-materialization-report.v1` を返す。両者と tool semantics の machine authority は
`cxxlens.clang22-materialization-contract.v1` とする。
transport は stdin 上の一つの JSON request と stdout 上の一つの JSON report に固定し、stderr は diagnostic-only とする。shell command
construction、stdout の複数 report、stderr prose による outcome 判定を禁止する。

v1 request/report の version は exact `1.0.0` とし、missing/unknown field、unknown version、identity/digest mismatch を effect 前または
publication 前に拒否する。raw byte は BOM のない exact UTF-8 として一つの top-level JSON object だけを decode し、任意 depth の duplicate
member、invalid UTF-8、BOM、non-finite number、object 以外の top-level value、二つ目または trailing JSON value を拒否する。authority YAML
loader や permissive/last-key-wins decoder を request/report transport に流用しない。v1 に implicit migration、adjacent version fallback、
unknown-field preservation、caller-selected default を設けない。request は exact project catalog census、expected parent snapshot、
condition/interpretation、worker/provider/toolchain/environment identity、source/input/invocation digest と budget/security binding を失わず、tool が全
compile-unit task を bottom-up に導出する。
caller が task ID、descriptor digest、dependency group、publication result を差し替えることはできない。

この tool は installed process/machine surface であり、新しい public C++ Clang host bridge ではない。既存の
`process_execution_report`、`provider_session`、native SDK に Clang 22 task authoring、sealed result、Store publication method を追加しない。
一般化された public provider adoption API もこの decision では認可しない。

portable task、provider protocol、provider runtime はこの specialization より上流の generic dependency のまま維持する。各 generic contract から
本 contract への参照は discovery 用の `reverse-specialization-index` であり、`dependency: false` とする。従って明示 dependency DAG の向きは
materialization contract から generic contracts への一方向であり、generic validator や public runtime が Clang 22 specialization に依存することを
意味しない。

installed specialization は request の ordered `(condition universe ID, condition ID)` を
`("cxxlens.clang22.condition-ref.v1", condition universe ID, condition ID)` として
`cxxlens-canonical-tuple-v1` で encode し、同名 semantic-digest-v2 domain から導出した
`condition-ref:<digest>` を generic portable task の condition に渡す。universe と condition の両方が task ID に bind
されるまで task を開かず、caller-supplied ref、universe の省略、pair の sort/deduplicate を拒否する。この private
mapping は generic `condition_ref` の public C++ API、ABI、factory semantics を変更しない。

### Exact task and descriptor family

各 compile unit の task は次の canonical order の exact six descriptor だけを要求する。

```text
cc.call_direct_target.v1
cc.call_site.v1
cc.entity.v1
frontend.clang22.call_observation.v2
frontend.clang22.entity_observation.v2
frontend.clang22.type_observation.v2
```

observation v2 は accepted Relation Registry の exact descriptor/domain identity を使用する。private observation v1 は installed Store adoption
authority ではなく、診断または既存 conformance evidence に限定する。v1 row を v2 として reinterpret、field synthesis、digest alias してはならない。
worker input codec は exact `cxxlens.clang22.task.v3` とし、request/catalog/source/build binding から tool が full global catalog entry projection、
selected catalog entry ID、final relation compile-unit ID、bytes と digest を生成する。worker は shared catalog factory で global catalog と generic task
ID を再構成し、selected entry mapping を検証してから final relation ID を observation/canonical row に写す。旧 codec、caller-generated payload、
同一 task/input/execution tuple 下の payload mutation を受理しない。
v3 payload は shared `cxxlens-canonical-tuple-v1` の signed `int64` integer domain を使うため、request budget の各正整数は
`1..INT64_MAX` に限定する。public `execution_budget` の `uint64` surface を狭めるのではなく、この installed JSON v1 から shared codec へ
lossless に写像できない値を worker launch 前に `materialization.request-invalid` で拒否する。

observation v2 row は別の exact native codec `cxxlens.clang22.observation-native.v2` で構築する。入力は kind、final relation
compile-unit ID、native semantic key、payload map、optional full primary-span bundle、ordered origin chain、exact-equivalence、optional limitation の
exact record とし、validated native detached value 以外から再構成しない。kind は `entity` / `type` / `call` から対応する Registry v2 descriptor へ
exact に写像する。semantic key column は nonempty strict UTF-8 native string の byte sequence をそのまま保持し、normalization、再hash、v1 alias を
行わない。
`exact_equivalence` は native boolean とし、true と limitation absent、false と nonempty strict UTF-8 limitation を双方向に
couple する。report leaf の `limitation_digest` は exact なら `null`、non-exact なら limitation の exact UTF-8 bytes の
content digest とし、primary-span bundle の有無とは独立に検証する。

payload は strict UTF-8 の nonempty key と strict UTF-8 value の unique map とし、key の UTF-8 byte order で sort した entry tuple を用いる。

```text
payload_projection = tuple(
  "cxxlens.clang22.observation-payload.v2",
  exact relation descriptor ID,
  tuple(tuple(key_utf8_string, value_utf8_string)...)
)
payload_digest = semantic_digest_v2(
  "cxxlens.clang22.observation-payload.v2",
  canonical_binary(payload_projection)
)
```

origin chain は native extractor の immediate expansion から outermost への順序を変えず、duplicate も保持する。各 entry は
`(kind UTF-8 string, logical path UTF-8 string, begin signed-int64, end signed-int64, read-only bool)`、全体は
`("cxxlens.clang22.source-origin-chain.v2", ordered-entry-tuples)` の `cxxlens-canonical-tuple-v1` bytes とする。kind/path は nonempty、
`0 <= begin <= end <= INT64_MAX`、read-only は true を要求する。chain が空なら optional column は absent、nonempty なら exact encoded bytes を格納し、
path normalization や set sort を行わない。type observation は primary span と origin の双方を持たない。

entity/call の primary bundle は七 field を Registry column へ一対一に写し、type では absent とする。payload digest と origin bytes を構築後、
`observation` ID は Registry に宣言された ordered domain projectionを `sdk::derive_domain_identity` と同じ codec で再計算し、
`sdk::validate_domain_identity` 相当の exact comparison を batch seal 前に行う。private v1 の newline/decimal-length `canonical_form()`、
`clang22.observation-payload.v1` digest、origin framing は v2 の bytes または digest として再利用・reinterpret しない。
各 observation と同じ worker task から生成する canonical `cc.*` row の compile-unit column は current task の final relation compile-unit ID と
exact に一致させる。別 task の有効な base row が同じ transaction に存在しても cross-task attribution を許可せず、shared validator は row decode 後、
hard-reference lookup より前に current task binding を検証する。

task の dependency group は canonical order の exact `[canonical, observation]`、各 group の atomic output group は exact
`clang22-atomic` とする。両 group は
全 task で mandatory であり、missing、duplicate、extra、unsealed、順序または descriptor binding の不一致は task 全体を failure にする。
partial group adoption、compile unit 単位の partial publication、成功 task だけの first-wins は禁止する。

request の project catalog entry ID は project より上流の catalog-local input identity とし、final
`build.compile_unit.v1.compile_unit` relation ID の equality alias にしない。tool は全 entry の ID、effective invocation、source、environment digest
から shared `project_catalog::make` と同じ codec で global catalog digest/ID を再計算し、そこから project、variant、source、final compile-unit relation
ID を acyclic に導出する。各 task は selected catalog entry ID と final relation ID の両方を保持し、entry の invocation/source/environment digest と
relation payload を exact に cross-bind する。missing、duplicate、extra、digest drift、または暗黙 ID alias は
`materialization.catalog-census-mismatch` / `materialization.identity-mismatch` で拒否する。

effective invocation の installed exact codec は `cxxlens.clang22.effective-invocation.v1` とする。projection は ordered
`["cxxlens.clang22.effective-invocation.v1", working_directory, effective_argv]` だけとし、`effective_argv` は argv0 を含む
全 element の順序と duplicate をそのまま保持する。shell parsing、quoting の再解釈、sort、deduplicate、argv0 の除外を
行わない。tool はこの projection の `cxxlens-canonical-tuple-v1` bytes から同名 domain の semantic-digest-v2 を
再計算し、request の `normalized_invocation_digest`、selected catalog entry の effective invocation digest、および
`build.compile_unit.v1` identity projection の `effective_invocation_digest` とすべて exact に一致することを task 実行前に検証する。
argv または working directory を変えて旧 digest/catalog/final compile-unit ID を使う request は fail closed に拒否する。

全 TU の portable task は同じ validated global catalog/session/output/condition/interpretation/group projection を使うため、同じ generic
`provider_task_id` を共有し得る。これは execution の alias ではない。exact `task_input_digest` は full global catalog projection、selected catalog
entry、final relation ID と TU input を bind し、`provider_execution_id` は accepted `core.provider_execution.v1` projection から導出する。request/report
の task/result 対応は `(provider_task_id, task_input_digest, provider_execution_id)` の exact tuple で行い、task ID だけの map、first-wins、同一 tuple
duplicate を禁止する。`provider_execution_id` は installed binary occurrence を bind する physical report identity なので base claim の semantic
producer/provenance/set digest には投影しない。base semantic mapping は task ID、input digest、selected catalog entry ID、final relation ID を保持し、
static/shared の physical execution evidence は report 側で別に bind する。

### Full source-span bundle and base ownership

entity/call observation v2 は primary span ID だけでなく、その ID を再計算して `source.span` claim を構築するための coherent full bundle を保持する。

```text
source snapshot ID
file ID
half-open begin/end byte offsets
semantic range role
read-only state
source span ID
```

bundle は entity/call の双方で optional all-or-none とする。存在する場合は tool が identity、range、snapshot/file binding を再検証する。
bundle がない observation も捨てず、typed unresolved と non-exact guarantee で会計し、対応する source-dependent canonical `cc.*` row を生成しない。
特に complete bundle がない `cc.call_site` は transaction を reject する。tool は request/catalog の完全な authoritative payload から、次の exact
topological order の base claims を bottom-up に再計算し、canonical deduplicate して同じ unpublished transaction へ stage する。

```text
build.project.v1
build.toolchain_context.v1
build.variant.v1
source.file.v1
build.compile_unit.v1
source.span.v1
```

request は `build.project` の catalog/root/environment、`build.toolchain_context` の family/exact-version/target/builtin/sysroot/ABI/plugin、
`build.variant` の language/standard/target/macro/include/semantic-flags、`source.file` の project/path/content/size/encoding/line-index/read-only、
`build.compile_unit` の project/main-source/variant/toolchain/effective-invocation/language/working-directory を claim key とともに保持する。
`normalized_invocation_digest` は上記 installed exact codec から再計算し、`build.compile_unit.effective_invocation_digest` へ exact に写像する。caller-supplied ID や opaque digest だけを payload
の代用にせず、各 relation の domain identity と hard reference を Registry の canonical tuple から再計算する。base claim の
condition/interpretation/provenance/evidence/guarantee envelope も request から exact に構築し、暗黙 default や first-wins を使わない。semantic
producer identity は executable/interface/distribution/source revision/tree だけへ投影し、installed binary、configuration、prefix digest は含めない。
後者の physical producer occurrence は report の installation/provider/authority evidence が別に bind する。これにより static/shared の semantic
base set と snapshot identity は同一のまま、実際に実行した bytes は失わない。

base claim の set digest は row set と task-context set を独立に hash してはならない。各 canonical row identity と exact row digest を、
その row を生んだ semantic task context の canonical sorted list に直接 pair する。context は provider task ID、task-input digest、selected
catalog-local compile-unit ID、final relation compile-unit ID、condition universe/condition、interpretation domain の exact tuple とし、physical
provider-execution ID は含めない。condition、interpretation、provenance、evidence、envelope の各 digest はこの per-row binding list を共通入力に
再計算するため、二つの row 間で condition または provenance の task edge を入れ替えると必ず変化する。

`source.span` では report に exact seven-field validated bundle、bundle digest、そこから構築した source-span row digest、originating semantic task
context の binding を保持する。tool/release validator は bundle の range/source binding と span ID、row digest、canonical binding set digest を再計算し、
同じ binding list を base `source.span` row-envelope/provenance/evidence projectionにも用いる。bundle set と row set の独立 hash だけで両者の edge を
代用しない。

検証済み bundle から `source.span` を materialize し、canonical row の hard reference 検証より先に stage する。standard `source.span.origin` は
独立した authoritative standard-origin payload がない限り absent とし、provider origin-chain bytes から合成しない。report は上記 six descriptor
ごとの canonical claim count/set digest と全 base-claim set digest を保持し、release matrix は memory/SQLite/static/shared の全 run で同一 semantic
base set を要求する。ordered lossless origin-chain は bundle の有無から独立した provider evidence として別 field に保持し、
primary span の欠落を埋める authority にはしない。`source.span` の claim owner は source ingestion/tool sideであり、worker の exact six output に
第七の standard relation を追加しない。

macro origin は provider-owned ordered evidence のまま losslessly 保持する。authoritative snapshot/file binding がない header spelling range から
standard `source.origin` を捏造しない。primary span と origin の read-only policy、macro expansion range への直接 edit 禁止は維持する。

### Observation equivalence census, in-report digest DAG, and external release digest

report は observation v2 の三 descriptor ごとに `observation_equivalence_census` を持つ。observation batch は各 adopted row の
`{observation_row_digest, final_relation_compile_unit_id, originating_task, exact_equivalence, limitation, limitation_digest}` を row digest と
semantic task context で canonical sort し、
`cxxlens.clang22-observation-equivalence-set.v1` domain の `row_equivalence_set_digest` を再計算する。
`exact_equivalence == true` では limitation は absent、false では nonempty strict UTF-8 とする。各 batch の
`exact_equivalence_count + non_exact_equivalence_count` は exact row count に一致しなければならない。

observation claim stage の census は対応する全 observation batch の raw census row を semantic origin 付きで結合し、同じ
`cxxlens.clang22-observation-equivalence-set.v1` domain と canonical ordering で再計算する。physical execution key や batch digest を stage
census の入力にせず、stage census は raw-row union と exact に一致する。
guarantee の `observation_descriptor_censuses` は call/entity/type の exact descriptor order でこの三 stage census に一致する。
valid な non-exact report は limitation を保持できるが、production `approximation: exact` は三 descriptor のそれぞれで
`non_exact_equivalence_count == 0`、blocking unresolved zero、および独立な full-span precondition が成立する場合だけ生成できる。

report 内の digest は opaque caller input ではなく、machine contract が宣言する domain-separated canonical projection の非巡回 DAG とする。

```text
sealed typed leaves
  -> descriptor batch
  -> dependency group
  -> task side channels / task result
  -> task-result set / global typed summaries
  -> guarantee and claim stages
  -> global provenance + base-guarantee cross-binding
```

sealed batch は `ordered_chunk_set_digest`、`batch_digest`、`claim_content_set_digest`、`provenance_edge_set_digest`、および
observation descriptor だけ optional census を再計算可能な leaf として持つ。`batch_digest` は physical composite execution key に加え、
各 row binding の semantic origin、final relation compile-unit ID、`primary_span_bundle_digest`、`exact_equivalence`、`limitation_digest` を
bind する。canonical descriptor の後三 field はすべて `null`、observation descriptor の exact-equivalence は boolean とし、
exact なら limitation digest は `null`、non-exact なら non-null とする。type observation の span-bundle digest は `null`、entity/call は
bundle がある場合だけその exact digest を持つ。group の `batch_set_digest` は
`cxxlens.clang22-group-batch-set.v1` domain で physical task execution key
`[provider_task_id, task_input_digest, provider_execution_id]`、group metadata、descriptor-contract order の exact batch summary を bind する。
descriptor の row が 0 件でも batch 自体は mandatory とし、Provider Protocol の empty-batch 正規形に合わせて row count、ordered chunk、
row binding、provenance edge をすべて 0/empty にする。zero-row に nonempty leaf を残すこと、および positive row count で ordered chunk、
row binding、provenance edge のいずれかを空にすることを拒否する。
task は coverage/unresolved/evidence/guarantee fragment の component digest を持ち、`side_channel_digest` を
`cxxlens.clang22-task-side-channels.v1`、`task_result_digest` を `cxxlens.clang22-task-result.v1` で導出する。後者は
execution/selected catalog/final relation ID、terminal、transcript digest tuple、canonical/observation group digest、side-channel digest を bind する。

adoption の `task_result_set_digest` は `cxxlens.clang22-task-result-set.v1` domain で task execution key 順の task result を、raw-frame
set は `cxxlens.clang22-raw-frame-set.v1` domain で同じ physical task 順の frame count/digest を bind する。global coverage、unresolved、evidence はそれぞれ
`cxxlens.clang22-global-coverage.v1`、`cxxlens.clang22-global-unresolved.v1`、`cxxlens.clang22-global-evidence.v1` domain で、
自身の digest を除く typed summary fields と semantic-task-key-sorted component leaves から再計算する。semantic task key は exact
`[provider_task_id, task_input_digest, selected_catalog_compile_unit_id, compile_unit_id]` とし、`provider_execution_id` を除外する。
evidence kinds は exact sorted key set、
kind counts の和は total count とする。guarantee digest は `cxxlens.clang22-materialization-guarantee.v1` domain で typed fields、三 global
side-channel digest、同じ semantic-task-key-sorted guarantee fragments、exact order の observation descriptor censuses を bind する。このため
static/shared の physical execution ID が異なっても global side-channel/guarantee と semantic base set は一致できる。

claim content stage set は `cxxlens.clang22-claim-stage-content-set.v1` domain で descriptor と semantic-task-key-sorted
`{semantic_task_key, row_count, row_bindings}` を、provenance stage set は `cxxlens.clang22-claim-stage-provenance-set.v1` domain で
`{semantic_task_key, row_count, provenance_edge_digests}` を bind する。どちらも physical `provider_execution_id`、`batch_digest`、
`group.batch_set_digest` を投影しない。`claim_stage_digest` は `cxxlens.clang22-claim-stage.v1` domain で descriptor、stage、
count、content、provenance、guarantee、optional census を bind する。global provenance の `edge_set_digest` は
`cxxlens.clang22-global-provenance.v1` domain で summary counts と canonical-output stage summary を bind するため、claim stage とともに
static/shared で identical である。guarantee から下流の
claim-stage/base digest への edge は作らず、DAG の循環を禁止する。in-report DAG は global provenance と独立な
base-claim/guarantee cross-binding で終了し、report schema は top-level digest または self-hashing root を持たない。validator は leaf から
これらの terminal cross-check まで bottom-up に再計算し、stale、差し替え、欠落、count drift を publication/release
adoption 前に拒否する。semantic base digest から除外する physical execution identity も task/execution evidence では bind する。

release evidence layer は internal DAG を検証した後、strict canonical serialization に従う complete report artifact bytes/value 全体を
report 外で `report_digest` に hash する。configuration ごとの `report_set_digest` は configuration と exact backend order
`[memory, sqlite]` の `{backend, report_digest}` から canonical に導出する。どちらの digest も materialization report 自身の field ではない。

### Internal sealed adoption boundary

shared typed transcript validator は accepted batch の decoded rows、descriptor/task/group binding、ordered batch digest、coverage、unresolved、
evidence、guarantee、provenance、terminal を tool-private の immutable `sealed-materialization-result` に value-own する。この value は全 batch と
両 mandatory group が seal され、span/claim/hard-reference/coverage validation が完了した後だけ生成できる。

raw frame、stdout bytes、diagnostic JSON、成功 terminal 文字列は diagnostic/evidence であり、row reconstruction、claim construction、Store adoption
の authority ではない。downstream code が frame を再 decode して sealed result を作り直すこと、private codec を複製すること、diagnostic prose
substring で control flow を決めることを禁止する。sealed value は tool 内部だけで消費し、public C++ type として export しない。

### One transactional publication

tool は request の全 compile unit、両 dependency group、base/span/canonical claim、coverage/unresolved/evidence/guarantee/provenance を一つの
snapshot draft に stage する。一 request の target backend は memory または SQLite の exactly one とする。全 hard reference と independent
validator が通った後だけ、expected-parent CAS を伴う一回の snapshot transaction で publication する。stale parent、worker failure、task/group/
span/claim/coverage/report/store failure は draft 全体を破棄し、prior published head を変更しない。

同じ semantic request を各 backend に与えた matched run は memory と SQLite で同じ snapshot/partition/query digest を生成しなければならない。
SQLite evidence は close/reopen 後の再検証を含む。一方の backend、static/shared configuration、compile unit、mandatory group だけが成功した
report を production evidence として採用しない。

### Typed report, errors, and release binding

report は exact request/tool/worker/provider/task/catalog/descriptor/group/input/toolchain/environment/expected-parent identity、各 task/group verdict、
publication identity、memory と reopened SQLite の parity、coverage、unresolved、evidence、guarantee、provenance、structured diagnostics と
structured diagnostics と全 retained child digest を保持する。observation equivalence census とその production-exact verdict、および
leaf から global provenance/base-guarantee cross-binding まで再計算可能な in-report DAG を必須とするが、top-level
semantic/report digest は保持しない。schema-valid な任意 digest を semantic evidence として受理しない。少なくとも次の
stable error family を区別する。

```text
materialization.request-invalid
materialization.version-unsupported
materialization.identity-mismatch
materialization.catalog-census-mismatch
materialization.task-binding-mismatch
materialization.descriptor-binding-mismatch
materialization.transcript-invalid
materialization.group-incomplete
materialization.span-invalid
materialization.claim-invalid
materialization.coverage-incomplete
materialization.stale-parent
materialization.store-failure
materialization.worker-failure
materialization.report-invalid
```

distribution 1.0 qualification は同一 exact revision/tree に対する relocated static/shared package と memory/SQLite backend の直積について、
materialization report を各 combination exactly one、合計四件要求する。各 configuration の production tuple evidence digest は、その
configuration の memory/SQLite report 二件の external canonical report-set digest と installed worker/tool bytes、request artifact/
external report artifact/contract digest を bind する。
report がない、重複する、configuration/backend/revision/worker/tool/task/store digest が一致しない、memory と close/reopen SQLite の semantic
snapshot/canonical-export/query digest が一致しない、三 descriptor census のいずれかで
`non_exact_equivalence_count != 0`、blocking unresolved、または
full-span precondition failure がある、digest DAG を bottom-up に
再計算できない、または transaction が publication まで完了していない場合は `not-qualified` とし、GR production tuple を生成しない。

authority-only rollout 中は、production-scope の materializer assignment 自身が exact
`scope.clang22-installed-adoption-gap` / `#181` / `[DF-0182, DF-0187]` の `tracked-gap` である場合に限り、normal release evaluation が request/report/set の
exact zero state を typed `tracked-gap-empty` として受理する。この状態では materializer executable 一件だけを installed-file census から除外し、
schema、worker、他の required file は除外しない。zero state に一件でも request/report が混じる、または exact four-pair evidence が先行する場合も
assignment と evidence が一致しないため拒否する。

#181 が同 assignment を `included` / `qualified` として完了させた後は、他の assignment に tracked gap が残り全体の scope closure が
`classified-with-gaps` であっても、この例外を解除する。materializer executable と exact four co-located request/report pairs、two report-set
digests を必須とし、それらがない evaluation は `not-qualified` とする。owner、feedback、scope、qualification がこの二状態のどちらにも exact に
一致しない中間状態は fail closed に拒否する。

本 ADR、統合設計/Public API Catalog、machine schema、Relation Registry v2、provider/release/acceptance contract と fail-closed checker の変更は
Issue #182 の authority-only unit である。独立反証 review がこれらの exact binding を受理した時点で DF-0182 を accepted / may-proceed とし、
Issue #181 を unblocked にする。runtime/tool implementation と四件の installed report evidence は Issue #181 が完成させる。それまでは production
scope の tracked gap と Clang 22 production-support prohibition を維持する。

## Rejected alternatives

- **Clang-22-specific public C++ host bridge**: provider 固有 task と publication を stable SDK に露出し、option 2 の installed machine boundary を崩す。
- **general public task-builder/adoption API**: 再利用可能だが、証明済みの一つの installed vertical より広い public semantics を先に固定する。
- **raw frame/test-only reconstruction**: shared validator と registry/store adoption を迂回し、diagnostic bytes を authority にする。
- **worker が `source.span` を第七 output として送る**: source ingestion ownership と exact six descriptor contract を崩し、base/canonical responsibility を混在させる。
- **task/group/backend ごとの partial publish**: project catalog census、hard reference closure、memory/SQLite parity、one-snapshot evidence を失う。

## Compatibility and rollback

- public C++ signature、symbol、ABI、callable inventory は変更しない。catalog-local identity の Doxygen/contract clarification だけを行い、既存
  portable/provider-runtime/native SDK は引き続き protocol/conformance surface とする。
- observation v2 は v1 と semantic-compatible と仮定しない。v1 transcript/report は v2 adoption input にならず、silent upgrade path を持たない。
- JSON contract v1 の意味変更は patch で行わない。required field、identity、group、publication、error semantics の変更は明示的な新 contract/version とする。
- implementation rollout 前は tool/report を production authority として advertise しない。rollout 後に障害があれば new materialization を停止し、unpublished
  draft/report を破棄して prior snapshot head と既存 public SDK を維持する。published history の rewrite や v1 report の reinterpret は rollback に使わない。

## Consequences

- Clang 固有の installed task authority と publication path を一つの versioned machine surface に限定できる。
- shared transcript validation の結果を losslessly retain しながら、raw frame と public API の authority inflation を防げる。
- catalog input identity と final relation identity を acyclic に分離し、暗号学的 fixed point や opaque placeholder を要求せず base claim を構築できる。
- source span の復元不能な hidden assumption を observation v2 full bundle と host-owned materialization で解消する。
- whole-project one-transaction と memory/reopened-SQLite parity が static/shared release evidenceへ直接結び付く。
- Issue #181 は accepted machine schemas/registry を変更せず、tool、runtime/store integration と四件の release evidence を同一 contract に実装する必要がある。

## Verification

contract/checker は exact JSON version、catalog census、six descriptor/group binding、full span bundle、raw-frame non-authority、sealed result completeness、
one expected-parent transaction、memory/reopened-SQLite parity、typed report/errors、static/shared matrix を positive/negative fixture で検証する。
negative fixture は missing/extra task/group/descriptor、v1 observation、partial span bundle、source bundle のない canonical row、span identity
mismatch、unsealed batch、dropped
coverage/unresolved/evidence/guarantee/provenance、raw-frame reconstruction、stale parent、partial publication、backend/configuration/
external-report-artifact digest mismatch を
個別に拒否する。さらに catalog-local/final relation ID の implicit alias、entry digest mapping drift、catalog entry/task census drift、同じ generic task ID を
共有する二つの valid execution の誤 deduplicate、同一 task/input/execution tuple duplicate をそれぞれ反証する。
condition universe だけを変えて旧 condition ref/task ID を使う case、effective argv の argv0、順序、duplicate または
working directory を変えて旧 invocation/catalog/final compile-unit ID を使う case も task execution 前に拒否する。
observation の exact flag/limitation を変えて row-binding/batch/stage/guarantee census を残す case、または batch、group、task
component/result、task-result/global typed summary、guarantee、claim stage、global provenance、base-guarantee cross-binding のいずれかだけを
差し替える case を各 in-report DAG layer の negative fixture で拒否する。external release fixture は complete report artifact の
byte/value mutation による report digest drift と、backend/report entry の swap、duplicate、missing による configuration report-set digest drift を拒否する。
static/shared の matched fixture は physical `provider_execution_id` の差を batch/group/task-result に保持しつつ、semantic task key が
同じ global coverage/unresolved/evidence/guarantee、claim-stage/global-provenance digest と base semantics を生成することを検査する。
反対に selected catalog または final
compile-unit ID だけを変えて global digest を維持する fixture は拒否する。

authority-only PR は独立反証 review 中だけ design feedback を `proposed/blocked` に保ち、machine/relation/release/acceptance authority の整合が
受理された後に `accepted/may-proceed` と exact authority/resolution/review refs を記録する。Issue #181 の implementation PR は installed
static/shared tool を actual source で実行し、memory と close/reopen SQLite の snapshot/query parity、
prior-head preservation、external exact report artifact/report-set digest、GR `not-qualified`/`qualified` transition を同一 exact-head SHA で検証する。
