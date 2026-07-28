# ADR 0013: NG SQLite store は publication journal と canonical payload の hybrid とする

- Status: Accepted
- Date: 2026-07-16
- Decision owner: store-kernel
- Decision issue: #68
- Tracking issue: #56
- Current-layout amendment: ADR 0097 / #200
- Accepted same-process SHM authority amendment: ADR 0097 / #205 / DF-0205
- Exact accepted same-process proposal: `6cb705c256c9576f74b50a2dca8fc4e8f72d06bb`
- Same-process independent review: <https://github.com/horiyamayoh/cxxlens/issues/205#issuecomment-5095883584>
  (`P0=0 / P1=0 / P2=0`)
- Accepted writer native-attachment amendment: ADR 0097 / #206 / DF-0206
- Exact accepted writer attachment proposal: `bf30978eb34d5f94bbadfd675c8ce2b50fb2f899`
- Writer attachment independent review: <https://github.com/horiyamayoh/cxxlens/issues/206#issuecomment-5097950062>
  (`P0=0 / P1=0 / P2=0`)

## Context

ADR 0009 は semantic snapshot identity、publication series、transaction、reader pin を backend 非依存で確定したが、
SQLite の物理 schema は未決定だった。relation claim、condition、closure、unresolved、exact series selector を
一つの semantic contract として保存できる専用 format が必要だった。

## Decision

NG SQLite format は `cxxlens.sqlite-semantic-store.v2` とし、次の hybrid を採用する。Issue #69 で physical
minor を 2.1.0 へ進め、payload v2 に query annotation projection を追加した。Issue #73 で minor 2.2.0 /
payload v3 とし、query row/report が exact producer ID と semantic contract を保持できるようにした。
Issue #78 / ADR 0021 で minor 2.3.0 / payload v4 に exact partition identity binding と validated closure
certificate subject を追加し、v1〜v3 の ID-only closure は closed-world proof に使用しない。
Issue #90 / ADR 0033 で minor 2.4.0 / payload v5 に完全な partition claim envelope を追加し、open/compaction 時に
manifest と query-visible row/annotation/coverage projection を bottom-up で再構成する。
Issue #132 で physical minor を 2.5.0 とし、connection/process 間 publication CAS のための durable series headを追加する。

- `cxxlens_ng_metadata` は physical format version を保持する。
- `cxxlens_ng_publication` は publication ID、exact series ID、semantic snapshot ID、monotonic sequence、physical
  generation、parent、state、checksum、versioned canonical payload を保持する。
- `cxxlens_ng_series_head` は series ID ごとの current publication と sequence を保持する。publish は
  `BEGIN IMMEDIATE` 後にこの head と expected parent/sequence を再照合し、publication の immutable `INSERT` と head updateを
  同一 transaction で commit する。不一致は `store.publication-conflict` で rollbackし、publish path は `INSERT OR REPLACE`を使わない。
- series/sequence index は exact head lookup のためだけに使い、scan/page order を semantic order にしない。
- payload は pointer-free detached rows、manifest projection、query 用 claim annotation を length-prefixed binary
  で格納し、open 時に full SHA-256 checksum と semantic snapshot digest を再計算する。payload v1 は row read の
  ために読めるが、condition/interpretation/provenance/guarantee を推測せず query execution を拒否する。payload
  v2 は query annotation を読めるが producer field を持たないため、明示的な legacy-unknown producer として扱う。
- publication は WAL、`synchronous=FULL`、DB head CAS により複数 connection/process 間でも atomic に書く。memory head は
  database commit 成功後だけ更新する。
- compaction は payload を新 physical generation へ copy-on-write し、既存 handle が pin する generation は
  shared token の最終解放まで保持する。

複数 connection/process の CAS は、一つの connection または Store implementation への process-local 集約で代替しない。同じ
process の二つの live Store が同じ series head を競合更新した場合も、先に commit した一件だけが head を更新し、後続 writer は
transaction を rollback して `store.publication-conflict` を返す。SQLite Unix VFS が同一 process・同一 runtime 内で既存 writer の
SHM mapping を再利用し、後続 readonly probe の native `xShmMap(extend=0)` に `SQLITE_OK` と non-null pointer を返す場合の
narrow admission authority は、ADR 0097 の DF-0205 accepted authority amendment が定義する
`cxxlens.sqlite.same-process-writer-shm-mapping-lease.v1` にだけ基づく。exact proposal
`6cb705c256c9576f74b50a2dca8fc4e8f72d06bb` は Issue #205 の独立 review
<https://github.com/horiyamayoh/cxxlens/issues/205#issuecomment-5095883584> で
`accepted-authority-implementation-pending` (`P0=0 / P1=0 / P2=0`) として accept された。
schema の `same_process_writer_mapping_lease_proposal` key は reviewed exact artifact/history として保持し、
acceptance pending を意味しない。この acceptance は internal implementation を認可するが、それ自体では
exception を activate しない。distinct exact implementation commit と全 counterexample matrix の独立 review が
完了するまで、current source は従来どおり全 native `SQLITE_OK` を fail closed とし、production は block する。
connection sharing、PID-only token、pointer equality、VFS name、path spelling、post-hoc endpoint equality を
CAS または SHM nonmutation authority にしない。
accepted authority はwriter `xShmMap`のcaller/delegated extend pair、writer cohort in-flight、stat-only namespace
epoch、current-v3 Store gate、shared runtime/VFS cohortとaliasごとのdistinct lifetime pinをreceiptにする。
pre-existing SHMは`{0,0}`のexact size不変と、authenticated `{1,1}`のpreallocated-range
zero-size-effectまたはexact monotonic extensionを分け、absent SHMのexact createはauthenticated
`{1,1}`だけに許す。extend pair/effect receiptはmapping
generation keyではなく各attempt/resulting holder固有とし、valid `{1,1}`と`{0,0}`のcross-holder
joinはpair equalityなしで許す。prior mapping generationのreader handoffが一pageでも残る間はexact
file family全体で全pageのsuccessor generationを排他する。
SQLite lockがliveになり得る間にmain/WAL/SHM targetのduplicate FDをopen/closeせず、retained parent
directory FD、既存main/WAL native-file-node/`xOpen` receipt、SHM native attachment receiptだけを使う。
native close、same-thread reentrant retirement、unknown callback outcomeはleaseをfail closedにretire/
quarantineし、memory pin、final size、caller intentだけでauthorityを復元しない。

Issue #206 / DF-0206 は、一つのwriter native attachmentに複数page map holderが属し得る一方、
native `xShmUnmap` がattachment全体を一 callbackで解放するcardinalityをDF-0205 authorityが定義して
いないことを記録した。ADR 0097 と四つのSQLite/Snapshot contract/schema mirrorに置く
`cxxlens.sqlite.writer-shm-native-attachment.v1` は exact proposal
`bf30978eb34d5f94bbadfd675c8ce2b50fb2f899` を Issue #206 の独立 semantic/structural review
<https://github.com/horiyamayoh/cxxlens/issues/206#issuecomment-5097950062> が
`accepted-authority-implementation-pending` (`P0=0 / P1=0 / P2=0`) として accept した
writer-only amendment である。proposalはmap receiptをcallbackごとに保持しつつ、
checked attachment identityごとのcomplete pending/live holder setを一回のunmap outcomeへbindする。
cross-attachment grouping、partial group、duplicate unmap、second-page validation failure後のfirst-page
継続、closeのdouble cleanupを禁止する。この acceptance は internal writer attachment-group state
machine と focused tests の実装だけを認可する。reader grouping は Issue #207 / DF-0207、writer VFS
production binding は distinct exact implementation/matrix review まで blockし、current blanket native
`SQLITE_OK` rejectionを維持する。

exact proposal `3c52b7e01a4d2a4e382940017d1dfb8f07f1be54` の独立 review は
`P0=0 / P1=2 / P2=1` でrejectした。non-last attachmentが唯一supportしたpageをcleanup後も
fresh readerへ見せ得る点と、map-before-gate group snapshotが同じattachmentのlater mapと
total-orderされずpartial promotionを作り得る点がP1である。revised proposalはlive attachment
groupごとのpage supportを保持し、non-last cleanup後にfresh-reader-admissible page setをatomicに
再計算する。support zeroのpageはlive attachmentが再map/resealするまでfresh admissionへ戻さず、
そのpageへhide前にadmit済みのreader predelegationはno-map resolution、confirmed reader cleanup、
またはexact handoff promotionまでcleanup blockerとする。same-threadは待たずunmapせずquarantine、
other-threadはmutex外のbounded ordered wait後にexact recheckし、timeout/unknown/driftはretryしない。
別groupのexact supportが残るpageと確立済みhandoffはblockerではなく、handoffをfresh supportにしない。
sealed SHM sizeはpage authorityと分離したmonotonic physical observationとしてだけ保持する。
またgate boundaryとlater-map admissionを一つのregistry state boundaryでtotal-orderし、map-winningは
bounded blocker、gate-winningはcallback return前promotion routeとする。timeout、unknown、open-epoch
driftはcomplete groupをhide/quarantineし、successful gate後にpre-boundary pendingを残さない。
`9011b22` review時のenclosing lease digestは
`sha256:612d450d22b676e4144b76f61cab60cade3ae860f3457b7ec168a9bd00cd9550` である。
reader-predelegation orderingを加えたcurrent proposal digestは
`sha256:05624ed7e918d43705a4dd6b37884c43c4e98e7a2b89427bbe64367e0655a15f`
である。この exact revision は commit `bf30978eb34d5f94bbadfd675c8ce2b50fb2f899` として
<https://github.com/horiyamayoh/cxxlens/issues/206#issuecomment-5097950062> の独立 semantic/structural
reviewに合格した。review receiptとaccepted statusを加えたDF-0206 accepted checkpointの
enclosing lease digestは
`sha256:e522cbbe3c6bb9bf2ed645816941f1921f5884eacc36360e8dc546b779bded29` である。
この acceptance は writer-only であり、DF-0207 の reader attachment authority や production activation
を推移的に認可しない。

Issue #208 / DF-0208 の
`cxxlens.sqlite.writer-gate-outcome-evidence.v1` は、accepted DF-0206 のgate failure transitionに
exact typed negative evidenceとsole cleanup authorityを与えるreview-pending writer-only proposalである。
四つのSQLite/Snapshot contract/schema mirrorで `proposed-unqualified-non-authorizing` とし、
acceptance review receiptを追加しない。fresh independent exact-commit review完了前は、gate outcome
mutation、native cleanup、production/VFS binding、public API、native OK projectionを認可しない。
accepted DF-0206 fieldsとreader-predelegation ordering fenceは維持し、DF-0207 reader groupingへ推移
認可しない。

proposalはgate outcome evidenceとregistry cut executionを二軸に分ける。issuerはprocess/runtime/VFS/
file-family/alias/connection/open epoch、one-shot expected attachment epoch reservation、closed profile、
control epoch、attempt tokenへbindしたmove-only ownerにimmutable outcome kindをregistry transfer前に
exactly once sealする。caller lifecycleは
`open -> issuer_sealed_kind -> transferred_to_registry`であり、registry continuationのtimeout、drift、
abandonment、internal failureはkindを変えず `cut_execution_indeterminate` にする。preacceptance owner
dropはcut/effectを作らず、既存DF-0206 member owner/cleanup obligationをcancel、merge、dischargeしない。

six-stage profileはdomain `cxxlens.sqlite.current-v3-writer-gate.v1` と
`cxxlens-canonical-tuple-v1-length-framed` encodingの下で、authority path、expected/observed canonical
value bytes、typed receipt、allowed/observed effect bytesをclosed orderで保持する。各stage resultは
`passed` / `typed_determinate_failure` / `terminal_indeterminate` のclosed enumであり、ordered stageと
六つのfailure enumをexact bijectionにする。positiveはsix passed、failureはpassed strict prefix+
immediate stage-specific failure、terminalはclosed locus/phaseと一致するimmediate terminalまたは
six passed後のterminalだけである。unknown result、wrong-stage failure enum、prefix/locus/overall outcome
mismatchを拒否する。passed/failureはobserved value/receipt/effectのfull bundleを持つ。at-stage terminalは
value `not_observed|exact_present`、receipt `not_issued|exact_present`、effect
`not_executed|started_outcome_unresolved|exact_present`をphase-tagする。closed phaseは
`before-value-read | after-value-before-effect | after-effect-start-before-effect-result |
after-effect-before-stage-result`である。`observation_commit`はvalue/receiptを同時にpublishし、
`effect_start_commit`はdelegation直前にstart permitをconsumeし、`effect_result_commit`はterminal
effect transcriptをpublishする。value present/receipt not-issuedはunreachableである。
expected policy/allowed effectは常に必須、negative tagはpayloadなし、present/started tagは各exact payload
必須で、tag/payloadをcanonical evidenceへbindする。Store factory success control edgeと `post_format_prewrite/no-candidate-yet`を含む
zero publication-effect proofを要求し、SHA-256はacceleration keyだけとする。

attachment reservationは
`reserved -> claimed_inflight -> consumed_to_present | revoked | quarantined` とreservedからのrevokeを
含むclosed graphである。map-before-gateのpresentは既にconsume済みreceiptを持ち再consumeしない。
gate-before-mapのfirst callbackはregistry mutex下の`claim_and_form_dual`でreservation claim、shared
owner/control epoch、member sequence、native/member prestart projections、ordinary routes、一unused
delegation permitを一つの`atomically_dual_bound` formationとして同時publishする。claim-onlyまたはpartial
peer formationはreachableな通常状態ではなく、zero-effect invalid-partial sinkへfail closedする。
`start_existing_dual`だけが既存complete formationを一native callへ進め、second claim/native delegationを
拒否する。exact matchはconsume、known no-mapはrevoke、mapped mismatchは
revoke+accepted DF-0206 cleanup、throw/unknown/abandonmentはquarantine+eligibility revoke+sole unfired
late-resolution lineageとする。claim中のsame-thread/reentrant contenderはwaitせずrejectし、other-threadは
一回bounded wait/recheckし、timeoutはwaiting attemptだけをrejectしてoriginal claimを変えない。

member admissionとcutは一つのchecked non-reusable sequence domainを共有する。exhaustionはcut前に
registry admissionをquarantineし既存ownerを保つ。cut universe全blockerをterminal classifyした後だけ
complete final groupをmutex下で再導出し、one-to-one coverageを証明する。member terminal stateはnative
stateだけとし、attachment全体でnormalized `all_no_map` / `mapped` / `unresolved` census、直交する
`not_applicable_no_mapping` / exact live attachment owner / absent / ambiguous、およびexact live
connection-open-epoch close owner / absent / ambiguousを一回導出する。member receiptやequalityからownerを
mint/split/replicateしない。

coverage accountingとeffect readinessは別である。`effect_ready` はexact coverage、unresolved zero、
選択dispatch cellに必要なvalid reservation/attachment owner/close ownerの論理積だけで成立する。
unresolvedが一件でもあればsealed completionを禁止して `cut_execution_indeterminate` とし、whole cutを
hide/quarantineしてone unfired precut late-resolution lineageを設置する。missing/extra/duplicate/
cross-bound/inconsistent coverage/censusは21 rowsの外でwhole scopeをzero-call quarantineし、一つの
invalid-coverage tombstoneをconsumeしてfallthroughしない。このlineageは全bound callbackがexact
terminal/cancelledになるまでpermitを発火しない。lineageはexact expected reservationとtagged
`absent | present_exact | unresolved`へbindし、unknown identityからpresent/ownerを推測しない。`absent`は
expected reservation/callback cohortへbindしたexact terminal no-map receiptまたはvalid pre-start
cancellation receiptを必須とする。同じcallback
owner/control epochがnative開始前にfuture native effect impossibleを証明したexact cancellationだけをno-map
へnormalizeし、postcut claimならregistry mutex下で`claimed_inflight -> revoked`をatomicにconsumeする。
callback-started/unknown、caller cancellation、timeout、abandonmentはunresolvedのままとする。

precut kindはcomplete cut/final group/censuses/shared dispatchへbindし、exact coverage+zero unresolved+
closed cell後だけoperational rowへsubmitする。postcut kindはcompleted-positive receipt、exact claim/control
epoch、DF0205 pending-install/post-map-seal receipts、complete DF0206 member censusへ別にbindし、original cut
census/close/dispatchをauthorityにしない。claim ownerをdistinct decision lineageへ変換し、no-map/cancelは
zero effectでconsumeする。exact mappedはrequired receiptsとzero-live-memberを証明するcomplete censusが
揃い、cleanup instanceの全lifecycle/tombstone authoritative countがzeroの
場合だけ同じownerをnew sealed DF0206 `post_native_failure_without_live_member` instanceへtransformする。
既存instanceがどのstateにも一つでもある、またはauthority欠落/曖昧ならjoinせずzero-call quarantineする。first exact
postcut receiptはoriginal completed terminal後も有効で、own tombstone後のduplicate/cross-boundだけを拒否する。
DF0208 row authorityとpostcut claim-derived DF0206 transferはdisjointで、postcut close/new owner/cut再利用は
禁止する。

native effectはpermanent-unresolved fence、live-positive scope rewrite、mapping-identity-integrity
fence、coverage-integrity fence、unresolved fence、cut-indeterminate operational matrix、sealed outcome
matrixの順である。domainは
`waiting_no_effect | decision_unfired | terminal_or_consumed_reentry` のclosed partitionとする。cut-open、
coverage未完resolving、unresolved cut-indeterminateはzero row/zero permit。complete final rederivation+
exact coverage+zero unresolved+closed consistent cellを持つresolvingはzero effect/consumeでeffect-readyへ
入り、effect-ready+unfiredは15 sealed rowの一つ、同条件のcut-indeterminateは6 operational rowの一つを
選ぶ。valid sealedはeffect-ready→completed、
invalid 7 sealedは同じtokenでcut-indeterminate+named actionを完了しoperational rowを再選択しない。
zero-callを含む全rowが`unfired -> consumed_tombstone`をatomic consumeし、reentryはzero row/replay rejectで
ある。positive completedはvalid reservation+exact closeのempty
eligibility、またはconsumed reservation+exact attachment/close ownerのwhole-group promotionだけで、
cleanup zeroかつownerを保持する。determinate no-mapはexact close-only、determinate mappedは両exact owner
がある時だけunmap→closeとし、closeだけ欠ける場合はcut-indeterminateのone drain/no close、
attachment owner欠落/曖昧はzero callとする。terminal/cut-indeterminateはno-map+exact closeならone close、
mapped+exact attachment ownerならone drain/no close、owner absentはzero、ambiguousは規定cellで
cut-indeterminateにする。failure/terminal/cut-indeterminate/lateの全no-map rowはstill-reserved expected
attachmentをatomic revokeする。後続callbackはsealed gate kind/cut terminalを変えない。live-positive
contradictionは既存accepted DF-0206 live group/lifetimes/attachment/close/cleanup ownerをtargetとcensus
から除外する。frozen cut universe/tokenは変更せず、全tokenを既存live groupまたはresidualへexact total
disjoint projectionし、両側をone-to-one accountしてresidualを再導出する。residual unresolvedはwait、
invalid coverageはzero-effect tombstone、
complete residualは一つのoperational rowをconsumeし、既存live ownerへfallthroughしない。

normal admissionはdurable prior-cut state、active slot phase、work kindに加え、reservation-bearing workでは
`claim_and_form_dual | start_existing_dual`のexactly-one substepを選ぶ。cutでfreeze済みのtokenはimmutable
originに従い`fresh_member_start | fresh_terminal_resolution | dual_shared_start |
dual_terminal_resolution`のexactly-one stepだけを選び、cross-origin step、再claim、late formationを禁止する。

mapped terminalはtagとmapping lifetime pinを同じterminal commitでpublishする。fresh unsafe tokenは
`live_origin_tombstone | retired_identity_tombstone`だけを持ち、dual nonpromotionは一つのcomposite pin
custody cellを使う。fresh retirementはregistry mutex下のsole transitionでstate/pins/retirement receiptだけを
変え、fence-firstでは既設typed reason/member tag/quarantine reference/terminal-route bindingをbyte-identicalに
保持し、retirement-firstでは後続fenceがそれらをexactly once bindしてpinを再構成しない。dual cellは
`live_owned | integrity_quarantined_live | cleanup_inflight | cleanup_unknown_quarantined |
retired_tombstone`のclosed graph、ownerless/independent-DF0206/retired custody partition、およびexact
retirement terminal proofを持つ。

各cut terminalはchecked nonreusable generation付きdurable latest recordをslot release/wakeup前にinstallまたは
replaceする。後続dispatch consumptionは同じgenerationだけをupdateし、fresh/dual pin custodyとpostcut
continuationのcomplete transfer後にだけslot releaseとwaiter wakeupをatomicにpublishする。stale generation、
incomplete custody、terminal record未installではreleaseしない。

このreview-pending siblingを含むcurrent enclosing lease canonical digestは
`sha256:79f31929806955fceeb373739b5f67b8395525bb77d57f8886f6f0c559bcd89f` であり、
proposal acceptanceを意味しない。

ADR 0097 はこの hybrid と logical payload policy を維持しつつ、current physical layout を
`cxxlens.sqlite-semantic-store.v3` / `3.0.0` の bounded chunk table に置き換える。本 ADR の v2.6.0 schema は
read-only direct-open predecessor と registered migration source としてのみ authority を保つ。新規 DB と write は
v3 を使用し、v2 open は DDL/metadata/PRAGMA write を行わない。v2→v3 は既存 `snapshot_store::compact()` の
single-transaction COW migration だけを許し、open-time migration と新 public migration surface を禁止する。
v3 のlocator/VFS observation、closed format classifier、fresh file+parent durability、rollback/COMMIT terminal recovery、
same-main descendant判定はADR 0097と`cxxlens.sqlite-authority-state.v1`、
`cxxlens.sqlite-authorized-descendant.v1`、`cxxlens.sqlite-terminal-reclassifier.v1`が所有する。本ADRの旧open/compaction記述を
これらのfail-closed境界より優先したり、generic VFS、implicit recovery、diagnostic row rewriteを認可する根拠にしない。

## Consequences

- memory と SQLite は同じ canonical identity implementation を使い、backend/path/page order は snapshot ID に
  入らない。
- physical schema は public API に露出せず、format migration は semantic digest が一致した場合だけ成功する。
- corrupted current head は prior head へ fallback せず、明示的な intact prior publication は読める。
- claim payload の query index は semantic kernel 完成後に additive physical index として追加できる。

## Verification

`tests/unit/sdk/store_test.cpp` が memory/SQLite parity、reverse arrival、reopen、staged invisibility、単一 instance と
複数 SQLite instance の CAS、conflict rollback/retry、prior open、
partial closure rejection、cursor lifetime、pinned compaction を検証する。installed consumer は static/shared の両方で
memory と SQLite factory を link/open する。contract conformance は root relocation、forward/reverse/seeded shuffle、
jobs 1/2/8 の 36 通りを比較する。same-process CAS qualification は二つの独立 live Store/connection と materialization
winner/loser raceを実行し、cross-process CAS と同じ durable head verdictを要求する。accepted DF-0205 authority に基づく
internal lease implementation は、fork/PID reuse、writer unmap/last release、reader lifetime handoff、mapping pointer ABA、
runtime/VFS unregister/unload、main/WAL/SHM object/entry/mount/namespace replacement、page/size/pointer drift、native callback
outcome unknown、extend pair全分類、simultaneous first writers、writer in-flight対last-holder retire、
new-page/size receipt、duplicate-target-FD lock loss、same-thread reentrant retirementとbounded wait timeoutを
fail-closed matrixに含め、controlled VFSでprior-generation handoffと異なるpageへのsuccessor mapも
file-family-wideに拒否する。この exact implementation commit と matrix の独立 review が完了するまでは
current source の blanket native `SQLITE_OK` rejection を維持し、production activation を block する。物理契約は
`schemas/cxxlens_ng_sqlite_store_contract.yaml` と schema に固定する。
