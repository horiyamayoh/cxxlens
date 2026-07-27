# ADR 0013: NG SQLite store は publication journal と canonical payload の hybrid とする

- Status: Accepted
- Date: 2026-07-16
- Decision owner: store-kernel
- Decision issue: #68
- Tracking issue: #56
- Current-layout amendment: ADR 0097 / #200
- Accepted same-process SHM authority amendment: ADR 0097 / #205 / DF-0205
- Exact accepted proposal: `6cb705c256c9576f74b50a2dca8fc4e8f72d06bb`
- Independent review: <https://github.com/horiyamayoh/cxxlens/issues/205#issuecomment-5095883584>
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
いないことを記録する。ADR 0097 と四つのSQLite/Snapshot contract/schema mirrorに置く
`cxxlens.sqlite.writer-shm-native-attachment.v1` は現時点では
`proposed-unqualified-non-authorizing` である。proposalはmap receiptをcallbackごとに保持しつつ、
checked attachment identityごとのcomplete pending/live holder setを一回のunmap outcomeへbindする。
cross-attachment grouping、partial group、duplicate unmap、second-page validation failure後のfirst-page
継続、closeのdouble cleanupを禁止する。fresh independent authority reviewまでattachment group実装と
writer VFS production bindingをblockし、current blanket native `SQLITE_OK` rejectionを維持する。

exact proposal `3c52b7e01a4d2a4e382940017d1dfb8f07f1be54` の独立 review は
`P0=0 / P1=2 / P2=1` でrejectした。non-last attachmentが唯一supportしたpageをcleanup後も
fresh readerへ見せ得る点と、map-before-gate group snapshotが同じattachmentのlater mapと
total-orderされずpartial promotionを作り得る点がP1である。revised proposalはlive attachment
groupごとのpage supportを保持し、non-last cleanup後にfresh-reader-admissible page setをatomicに
再計算する。support zeroのpageはlive attachmentが再map/resealするまでfresh admissionへ戻さず、
sealed SHM sizeはpage authorityと分離したmonotonic physical observationとしてだけ保持する。
またgate boundaryとlater-map admissionを一つのregistry state boundaryでtotal-orderし、map-winningは
bounded blocker、gate-winningはcallback return前promotion routeとする。timeout、unknown、open-epoch
driftはcomplete groupをhide/quarantineし、successful gate後にpre-boundary pendingを残さない。
revised enclosing lease digestは
`sha256:612d450d22b676e4144b76f61cab60cade3ae860f3457b7ec168a9bd00cd9550`
であり、fresh independent reviewまでは引き続きnon-authorizingである。

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
