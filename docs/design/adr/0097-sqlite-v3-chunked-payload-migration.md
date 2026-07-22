# ADR 0097: SQLite v3 は bounded chunk payload と明示 COW migration を使う

- Status: Accepted
- Date: 2026-07-21
- Decision owner: store-kernel
- Decision issue: #200
- Implementation issue: #181
- Amends: ADR 0013, ADR 0096 D6

## Context

accepted Clang 22 materialization profile は、同一 request を memory / reopened SQLite、static /
shared の四象限で比較し、limit-adjacent の成功入力でも同じ semantic snapshot、publication、
canonical export、query projection を得ることを要求する。SQLite physical v2 は一 publication の
canonical payload 全体を一つの BLOB row に置くため、qualified runtime の
`SQLITE_LIMIT_LENGTH = 1,000,000,000` を越える有効な canonical-v5 payload を保存できない。
この物理上限を public request cap として転用したり parity を弱めたりすることは、accepted
request/report 2.1 と DF-0200 の retained-memory guarantee を変更してしまう。

2026-07-21 に Issue #200 で Option A が fresh decision として選択された。本 ADR はその決定を
physical Store contract に昇格し、logical payload、semantic identity、public callable を変えずに
SQLite の単一 BLOB ceiling を除く。

## Decision

### Format and compatibility

current physical format は `cxxlens.sqlite-semantic-store.v3` / `3.0.0` とし、新規または空の
database は v3 を作る。semantic payload は引き続き
`cxxlens.ng-snapshot-payload.v5` の byte-exact canonical stream であり、v1〜v5 の既存 readable
payload policy、snapshot/publication/claim identity、canonical export、request/report 2.1、public
C++ signature、result/cursor lifetime、accepted request set は変えない。physical format と chunk
layout は semantic snapshot ID から除外する。

direct-open matrix は次の exact state とする。

| Database | `compatibility()` | Mutation |
|---|---|---|
| v3.0.0 | `{sqlite, 3.0.0, true, false}` | publish と v3 compaction を許可 |
| v2 marker + exact v2.6-compatible layout/authority | `{sqlite, 2.6.0, true, true}` | read API だけを許可 |
| v2 marker + v2.6 profile 不一致 | `store.format-incompatible / sqlite-physical-format / v2-profile-mismatch` | 禁止 |
| exact v2.6 profile + corrupt committed row | direct open 後の既存 typed corrupt result | `compact-validation-failed` |
| exact current-v3 marker/profile metadata + schema/row/chunk damage | `store.corrupt` | 禁止 |
| v2/v3 signal 混在 | `store.corrupt / sqlite-format-classification / mixed-v2-v3` | 禁止 |
| missing/unknown nonempty marker | `store.format-incompatible / sqlite-physical-format / unknown-format-or-layout` | 禁止 |

legacy DB は `physical_format=cxxlens.sqlite-semantic-store.v2` だけを永続化し minor marker を持たない。
したがって 2.6.0 は persisted minor の推測ではなく、exact current v2 schema/codec profile と typed diagnostic
classification を満たすときに報告する readable capability version である。historical v2 layout を table shape や
first-wins で補完せず、一項でも不足する marker-v2 DB は mutation せず format-incompatible とする。一方、shape-valid
DB の corrupt committed publication は factory reopen を失敗させず process state に retained/marked-corrupt として保持し、
`current()` / `open(snapshot)` / `open_publication()` の既存 typed corrupt result と intact prior explicit open を維持する。
全 publication/head/payload の clean authority census は `begin()` / `compact()` の mutation gate 内でだけ必須とする。
base-symbol discriminator は exact empty、mixed、declared v2、declared current-v3、unknown nonempty の順で一度だけ分類する。
table shape だけから format を推測せず、mixed/unknown は v3 symbol/version gateへ進めない。declared current-v3 だけが v3 gateを
通り、その後の current-v3 profile damageを corruption とする。

empty public input は runtime/VFS/DB を開かず既存 exact tuple
`store.sqlite-path-empty / database_path / empty-detail` を返す。public input が exact `:memory:` の場合だけ filesystem
canonicalization より先に ephemeral locator として分類し、
fresh v3 を `READWRITE|CREATE|PRIVATECACHE|FULLMUTEX`（URI なし、loader-origin default を underlying とする explicit owned
forwarding alias）で作る。この branch は
filesystem database/sidecar、v2 predecessor/migration、close 後の persistence を持たず、既存 example/install consumer の
成功 semantics を維持する。それ以外の nonempty input は user URI ではなく filesystem path である。ただし embedded NUL
または platform path として lossless に表現不能な input は runtime/filesystem access 前に、zero-effect の exact tuple
`store.sqlite-failure / sqlite-locator / invalid-filesystem-path` で拒否する。valid raw path は runtime と source VFS を
bind した後、その VFS の `xFullPathname` だけで absolute locator にし、`mxPathname`、終端、失敗を検査する。host
`std::filesystem::canonical` / `realpath` や user の `file:` URI/query は authority にしない。symlink の扱いは bound
VFS の open policy と file identity が決め、internal URI percent encoding は source-private copy にだけ使用する。

filesystem locator は最初の DB/sidecar access 前に VFS capability を一度だけ bind する。caller-supplied rooted VFS は
registered name、implementation identity、owned backend lifetime token を bind し、通常 SDK path は loaded SQLite library の
default built-in VFS を一度 resolve し、original VFS pointer と non-null `pAppData` を保持したまま original pointer で
method を呼ぶ owned non-default forwarding alias を登録する。executable method と VFS/`pAppData` storage は pinned runtime
image の loader-origin proofを持ち、supplied VFS は explicit backend lifetime token を持つ。identity は alias/name/method
だけでなく original VFS pointer と `pAppData` も bind する。ただし標準 `sqlite3_vfs` ABIだけでは stable object identity、
parent entry、sidecar census、held read/hash、exclusive create、file/parent syncを観測できない。したがって default built-in VFSは
同じ opened objectへbindした owned platform observation companion、supplied VFSは source-private typed `rooted-vfs-v1` または
独立 qualified exact equivalentを必須とする。generic registered name/method/lifetime tokenだけの supplied VFSはDB access前に
`store.backend-unavailable / sqlite / vfs-observation`で拒否する。この capability identityもreceiptにbindする。以後の
probe、RW reopen、recovery、migration、compaction、sidecar operation は同じ alias/token を使い、NULL default の再解決や
name-only binding を禁止する。保持不能は zero-effect の
`store.backend-unavailable / sqlite / vfs-lifetime` とする。source-private copy decoder は source locator と別の owned
private-copy VFS capability を使う。`:memory:` も loader-origin/pinned default を一度 resolveして owned forwarding aliasを
登録・保持し、sole connection の explicit fourth-argument VFS とする。NULL-default raceやfilesystem main/sidecarは許さない。

main、WAL、SHM、journal は内容 read/copy/hash または SQLite open 前に、default filesystemではheld nonblocking regular file、
supplied backendではfinite seekable byte object、stable identity、非 directory/FIFO/socket/device、durable content/parent syncを
備えた exact equivalentであることを検査する。不一致はzero-effectの
`store.sqlite-failure / sqlite-object-kind / not-regular-or-equivalent`、namespace census自体のI/O failureは
`store.sqlite-failure / sqlite-sidecar-state / observation-io-failure`であり、concurrent driftと推測しない。

runtime preflight は locator validation 後に一つの library handle と exact v2-base symbols を pin する。existing filesystem は
VFS/observation capability bind、canonicalization、namespace/object-kind/sidecar classifier、base format discriminatorの順で
read-only classify する。exact v2 と unknown/mixed terminalは v3 symbol/version/limit floorを観測せず、exact v2だけが eager
diagnostic stateを返す。declared current-v3だけが v3-only symbols、SQLite 3.37.0、read-only full validation、actual
RW/no-create connection、16 MiB limitの順で進む。
fresh filesystem は VFS bind と nonmutating census の後、v3 symbol/version と separate scratch `:memory:` limit gate を通してから
raw bootstrap create/file+parent syncとtarget RW/no-create openを行う。exact `:memory:` は v3 symbol/version、owned alias、target
sole RW/CREATE connection、same-connection limitの順で進み、同 connection の limit を
DDL/metadata/marker より先に読むため、別 scratch/default-VFS resolution は行わない。この pre-gate connection construction は
authority effect ではない。WAL-only private recoveryにも同じclosed discriminatorを適用し、exact v2はそのまま complete decode、
declared current-v3だけが private connectionでv3 gate後にfull v3 decodeする。unknownはformat-incompatible、mixedはcorruptであり、
old runtimeを理由に別classへ推測しない。
source recovery を含む実 writer connection は forwarding VFS の write/delete/truncate、sidecar CREATE、SHM extension、
effectful file-control を initially deny する。owned wrapper は local `pOutFlags` を exact integer zero で初期化してから
その nonnull address を underlying `xOpen` に渡し、input の
`SQLITE_OPEN_MAIN_DB|SQLITE_OPEN_READWRITE` request と、成功時だけ returned flags を記録する。成功後に returned flags が
`SQLITE_OPEN_READONLY` を含まないことを、limit、synchronous、arming、lock、journal/recovery/store effect より先に証明する。
output が input flags をechoすることは要求しない。fallback/missing proof は
`store.sqlite-failure / open / read-write-required` とし、unarmed connection を close してretry/fallbackしない。
その後だけsame connection のsymbol/version/limit、locator、identity gateを通してarmする。`:memory:` はfilesystem mainの
`xOpen` proofを持たないsole RW/CREATE branchとして明示的に除外する。
全 `sqlite3_open_v2` profile は non-OK return と raw handle を同時に受け得る。null handle はcloseせず、nonnull handleは
statementを作らずexactly one `sqlite3_close_v2`を試す。close OKなら選択済みopen errorを返し、close non-OK/unknownなら
handle と runtime/VFS pinをquarantineして unregister/reopen/retryしない。fresh nonexistentで既にdurableなraw zero-byte mainだけは
残り得るがformat/Store authorityにはしない。

existing filesystem database は最初に no-create の read-only probe で format、schema、head/counter、payload authority を
完全検証する。quiescent private `immutable=1` connection の `PRAGMA journal_mode` は authority にせず、held main header の
offset 18/19 read/write version bytes が双方 2 であることを persisted WAL oracle とする。v3 と確認した場合だけ同じ
canonical path/VFS/file identity を read-write/no-create で再 open する。
existing v3 open は persistent PRAGMA を実行せず、held main header の read/write version bytes と original RW connection の
`PRAGMA journal_mode=wal` の双方で WAL を確認し、connection-local
`synchronous=FULL` を設定する。最初の `BEGIN IMMEDIATE` と各 write transaction 内で eager all-row/head anchor の exact
または valid descendant、WAL/header、format/schema/head/counter epoch を最初の Store write より前に再検査する。journal drift は
`store.sqlite-failure / sqlite-journal-mode / drift-before-write` とし、fully valid non-descendant replacement は
`store.sqlite-failure / sqlite-open-epoch / concurrent-authority-change` で mutation 前に拒否する。filesystem の nonexistent
actual limit/identity/synchronous/epoch gate failure はphaseごとにcleanupする。filesystemでoperation/recovery receiptをsealする前は
statementを残さずexactly one closeを試し、close OKなら選択済みpre-effect error、non-OKならconnection/runtime/VFSをquarantineする。
receipt seal後またはcoordination/journal effect後はfinalize、必要なら一回rollback、一回closeの後、close OKでだけ該当operationの
precommit classifierへ委譲する。`:memory:` はreceipt/reclassifierを使わず一回closeし、OKならsole databaseを破棄、non-OKならquarantineする。
`BEGIN IMMEDIATE`後のdriftもschema/data/process-stateを変更せず、publish、compaction、migration、fresh initializationの各precommit規則へ
委譲し、cleanup不確実性やclose non-OKを単なる元errorへ縮退させない。
database は raw bound-VFS exclusive zero-byte bootstrap 後、existing exact-empty database と同じ SQLite read-write/no-create
handoffで v3 を作る。SQLite `CREATE` を使えるのは exact `:memory:` branchだけである。read-write open のread-only
fallbackは上記actual `pOutFlags` gateでfail closedにする。existing v2 を open するときは、format を
識別する前に `CREATE`、`ALTER`、journal mode の変更、metadata write、migration を行わない。source DB は明示
`compact()` 以外で read-write open しない。sidecar は SQLite open 前に total classification する。`-journal` は他の
sidecar と併存しても `store.sqlite-failure / sqlite-sidecar-state / journal-present`、SHM-only と unreadable pair は
それぞれ `incomplete-wal-shm-pair` / `unreadable-wal-shm-pair` で拒否する。readable WAL+SHM でも held main header
offsets 18/19 が双方 2 でなければ zero-effect の `sqlite-journal-mode / expected-wal` とする。active-WAL source は
bound nonmutating census が readable WAL+SHM を識別した後、最初の underlying SQLite source `xOpen`、SHM map、authority read
より前に `sqlite-source-shm-readonly-unix-uri-v1` capability を確立できる場合だけ開く。この capability は loader-origin を
証明した SQLite Unix default VFS、または source SHM に同じ nonmutating contract を提供する typed exact equivalent に限定する。actual
runtime、VFS、filesystem profile の target source に触れない scratch WAL/SHM behavioral qualification を先に行い、最初の SHM map から
initialize、truncate、extend、create、delete、resize がないこと、CANTINIT heap-index と READONLY mapped-index の双方を検査する。
この branch だけは `sqlite3_sourceid`、`sqlite3_uri_parameter`、`sqlite3_uri_key` を v2 base symbols と同じ pinned runtime
handle から解決する。bound census が active WAL を選択する前の exact-v2 quiescent diagnostic readには、この追加 symbol、version、
behavioral qualification gateを課さない。
qualification unavailable/failure は underlying source `xOpen`、SHM map、authority read 前に fail closed とし、name-only Unix 推測や undocumented parameter の存在だけを
capability authority にしない。

qualification scratch の producer/cold/active locator は retained directory descriptor から `/proc/self/fd` 経由で解決し、専用の
same-thread exact `xFullPathname` arm 以外へ一般化しない。held target main/WAL/SHM は各 object の typed filesystem profile が
retained parent と scratch profile に exact 一致しなければならず、parent filesystem だけから target profile を推測しない。
unavailable/failure は内部 stage を公開せず exact
`store.backend-unavailable / sqlite / source-shm-readonly-qualification` とする。

application-generated URI は exact
`file:<uppercase-percent-encoded-canonical-absolute-path>?mode=ro&cache=private&readonly_shm=1` とする。canonical absolute path の
URI delimiter/non-unreserved byte は uppercase hex で percent-encodeし、`READONLY|URI|PRIVATECACHE|FULLMUTEX`（CREATEなし）と explicit
owned VFS alias で開く。`vfs`、`immutable`、user URI/query、unknown parameter を禁止する。owned forwarding VFS の main `xOpen`
callback receipt は canonical path と `mode=ro`、`cache=private`、`readonly_shm=1` の exact set、および prohibited parameter absence を
underlying source `xOpen` の delegation、authority read、SHM map 前に確認する。

logical canonical URI と別に、qualified target native main/WAL/SHM resolution は receipt に bind した一つの retained parent-fd
locator だけへ内部投影する。target `xFullPathname` 前から eager decode transaction 終了前まで parent namespace の
create/delete/move/self/watch-loss/queue-overflow を監視し、外部 writer の content modify/attrib は監視対象から除外する。native map
前後に fd-relative exact census と watch を検査し、event、loss、identity drift は native mapping を non-removing unmapして fail closed
とする。target main/WAL/SHM は retained parent directory の direct regular entry に限定し、symlink その他の間接参照は native
callback 前に拒否する。この direct-entry proof は bound source census に seal し、main header oracle その他の target source read より前に
検証する。leaf/ancestor の A-to-B-to-A は endpoint identity が復元しても受理しない。
epoch 開始後は logical host path を census や identity receipt のためにも再解決せず、retained parent-fd と held object receipt だけを使う。

forwarding `xShmMap` は native VFS の最初と後続すべての `extend=0` call を delegateする。caller の `extend=1` も最初と後続の
どちらでも native VFS へ `extend=0` として delegateし、extension request 自体は渡さない。
`SQLITE_READONLY_CANTINIT`+null と exact `SQLITE_READONLY`+non-null mapping を保持し、`SQLITE_READONLY`+null は
CANTINITへ正規化する。一度 READONLY-family を返した後も後続 map を抑止する permanent latch は禁止する。qualified readonly-SHM
profile で native `SQLITE_OK` が返ることは null/non-nullを問わず backend protocol violationとしてfail closedにし、READONLYへ
変換しない。writer attachによる正当なtransitionはCANTINIT+nullからexact READONLY+non-nullであり、generic non-profile callerの
`extend=0`+OK semanticsは別に保持する。この per-file READONLY-family state は successful delegated `xShmUnmap` でだけ
resetする。CANTINIT route は connection を閉じず、同じ connection と
`WAL_READ_LOCK(0)` のまま SQLite の heap WAL-index を使い、authority read 前から complete eager decode まで一つの explicit read
transaction を維持する。actual VFS-open main/WAL/SHM identity、directory binding、header WAL oracle/salt、VFS が観測した SHM
read-lock slot、complete decoded logical projection を receipt にする。post-close endpoint/digest-only private copy、別 connection、
arbitrary SQLite errorからのfallbackは禁止する。
SQLite API が公開しない `mxFrame` や post-hoc whole-file digest を推測しない。read-lock 0 は main snapshot route として
SQLite snapshot が保たれる限り WAL reset/rewrite/append と SHM coordination の変化を許すが、decode 前後の exact main
size/SHA-256 は一致しなければならない。read-lock N は main checkpoint と post-snapshot appendを許す一方、held lockがWAL
reset/prefix reuseを防ぐことを要求する。lock 0 の main size/SHA-256 drift、open-handle/directory identity replacement、または
snapshot lock lossを `sqlite-open-snapshot / concurrent-source-change` とする。一方、main/WAL/SHM identity、held main hash、
already-open WAL header/salt、SHM lockの観測I/O failureは
`store.sqlite-failure / sqlite-open-snapshot / active-wal-observation-io-failure`であり、driftと推測しない。decode後はprobeを閉じ eager stateだけを保持する。
この active read が exact logical empty/no-format を示す init-crash state は receipt/identity を exclusive に再検査し、
actual-connection gate 後に SQLite recovery/checkpoint で normalized empty を証明して fresh initialization へ進める。

main+WAL/no-SHM/no-journal は main-only marker の有無を問わず分類するが、WALがunreadableならcopy/openせず
`store.sqlite-failure / sqlite-initialization-sidecar / unreadable-wal-only`を返す。readable WALだけがpreauthority crash
candidateである。held main+WAL だけを
before/after identity/size/digest receipt 付きで private directoryへ copyし、SHM は copyせず private RW/no-create recovery
に作らせる。nonzero WAL は header/page-size/salt/frame/checksum と last valid commit prefix の内部妥当性を要求するが、
SQLite WAL format には main DB の durable UUID/salt がないため historical main-binding を主張しない。valid frame があって
commit frame がない場合は authoritative prefix を empty とし、private recovery が stable main-alone の exact empty/v2/v3 と
同一の場合だけ許す。cold factory で valid committed prefix から fully validated exact v2/current-v3 が復元された場合は、
それを current filesystem state authority として受理する。main-alone が partial checkpoint でもよいが provenance 証明とは
呼ばない。live operation receipt がある recovery は expected projection または authorized descendant を要求する。
zero-byte WAL は authority を持たず stable exact main だけを使う。valid headerでcommit prefixがない場合は、zero frame、
uncommitted frames、first invalid/truncated remainderを全てnonauthoritativeとしてmain-only equalityを要求する。valid commit後も
first invalid/truncated suffixでscanを止め、以後をresumeしない。invalid/torn WAL header、recovery disagreement、fully validated
result のない state だけを拒否する。exact v2 は eager read-only state、current-v3 は eager state と first-mutation recovery gate、empty は
init gateへ route する。

WAL/SHM/JOURNAL が全て absent の quiescent state は main file と parent directory entry identity を held read-only descriptorで
固定し、bounded copy/digest の source-private snapshotだけを strict internal URI `mode=ro/cache=private/immutable=1` で開く。
complete decode 前後の source digest/directory identity/sidecar absence drift は
`store.sqlite-failure / sqlite-open-snapshot / concurrent-source-change` とする。held main header の offsets 18/19 が双方 2
でない exact-v2 candidate は zero-mutation の `store.sqlite-failure / sqlite-journal-mode / expected-wal` で拒否する。
held main/parent観測、bounded copy、size/hash、before/after sidecar censusのI/O failureは
`store.sqlite-failure / sqlite-open-snapshot / quiescent-observation-io-failure`であり、driftとは区別する。
private snapshot も一つの explicit read transaction で完全 decode し、connection を閉じ eager process stateだけを保持する。
current、open、open_publication、canonical_export、query はその immutable process state と既存 pin lifetime を使う。
`snapshot_store::begin()` は既存 draft validation を zero-effect で先に行い、invalid draft には既存 public error を返す。
valid draft だけを、draft を作らず DB と process state に作用する前に exact tuple
`store.migration-required / sqlite-physical-format /
cxxlens.sqlite-semantic-store.v2-to-v3` を返す。`snapshot_store::compact()` だけを既存 public API 上の
明示 migration trigger とし、自動 migration、新 public migration callable、CLI を追加しない。
materializer は migration を代行せず、v2 Store の read-only open と head observation の後、最初の
`begin()` を authenticated operation `writer_begin` として一度だけ試みる。この Store error は compact phase
`store-stage` の `materialization.store-failure`、logical draft discarded、publication not attempted、zero commit
として保持する。

v2 の `open`、上記 read、`begin()` の zero-mutation oracle は、operation 前後で main database、`-wal`、
`-journal` の presence/size/bytes、および同じ directory の entry set が exact に一致し、`-shm` は
presence/size が一致して create/delete/resize されないこととする。WAL read lock/read-mark に伴う既存 `-shm` 内の
coordination bytes は durable Store authority ではなく byte-exact 比較から除外する。read-only SQLite が missing
sidecar の作成を必要とする状態は作成前に fail し、`immutable=1` で concurrent
change detection を無効化して成功扱いしない。この byte-equality oracle は no-external-writer の exclusive fixture
qualification に限定し、active-WAL runtime で別 writer の正当な変更を reader 自身の mutation と誤判定しない。runtime
read は write-denying flags/VFS と SQLite snapshot isolation を authority とする。この oracle から除外されるのは明示
`compact()` だけである。

### v3 physical layout

fresh/nonexistent または exact empty database の v3 initialization は、zero `application_id`、zero user object/metadata/
semantic-or-diagnostic authority を一つの exact-empty predicate とする。nonexistent path だけは canonical parent entry を予約・
再検査し、bound VFS でzero-byte regular mainをexclusive createしregular/equivalentとidentityを検証した後、held fileをfull
syncし、authenticated parent namespaceをsyncし、entry identity/zero size/empty digestを再検査する。このcreateと二つのsyncだけを
唯一のpre-lease persistent effectとする。fileまたはparent sync/recheck failureは
`store.sqlite-failure / sqlite-initialization-bootstrap / durability-opaque`とし、delete/retry/SQLite openを行わない。empty mainが
残ってもsuccess/format authorityではなく、次回はcomplete censusから再開する。
その後 SQLite exclusive lock と same identity を取得・再検査し、sidecar/header/journal/schema/metadata/authority effect は
lease 前に許さない。新規 empty main は failure 後に残ってよい。journal effectをarmする直前に locator/VFS、actual target-main
identity/directory bindingに加えて、bootstrap前から保持したpreinit absent/exact-empty anchor、exact-empty projection、raw main
size/digest、main/WAL/SHM/journal census、initialization ID、expected
empty-v3 projectionをreceiptへsealする。arm後のjournal transition failureはmain WAL headerやWAL/SHM/journal residueを残し得るため
zero-persistent-effectとしない。ただしschema/metadata/marker/semantic/diagnostic authority writeはまだ行わない。
locked exact-empty state で transaction 外から WAL をset/verifyし、続く一つの `BEGIN IMMEDIATE` 内で WAL/header、identity、
empty authority を再検査してから exact six DDL、
three non-marker metadata rows、physical-format marker last、exact DDL/four-row/empty-census revalidation、commit の順に行う。
arm後のjournal failureはfinalize、必要なら一回のrollback、一回の`sqlite3_close_v2`を行い、close OK後だけreceiptからtotal
sidecar classifierを実行する。same-identity exact emptyは`sqlite-initialization-recovery / opaque`でStoreを返さず、expected
empty-v3またはそこからcompressed algebraで証明したcurrent-v3 descendantはrecovered success、replacement/non-descendant/
invalid/mixedはphase固有のtyped failureとする。close non-OKはconnection/runtime/VFSをquarantineしreopen/unregisterしない。
別 initializer の same-identity exact/valid-descendant current v3 は write せず共通 handoffへ進め、replacement/non-descendant は
`store.sqlite-failure / sqlite-initialization / concurrent-authority-change` とする。

init-crash sidecar は前節の total classifierを使う。WAL-only は main+WALだけを stable receipt付きで copyし private SHM を
生成し、exact empty、v2、current-v3を別々に routeする。WAL+SHM active snapshot が exact emptyなら、main/WAL receipt と
SHM coordination identityを再検査する。empty の original recoveryは actual connection gateをwrite-denying状態で通した後だけ
SQLite recovery/checkpointをarmし、normalized exact-emptyを再証明する。v2はsourceを変更せずread-only stateを返し、
明示`compact()`だけが同じ receipt/recovery gateを使う。current-v3もreadはeager stateから返し、最初のmutationだけが exact
receipt recovery、exclusive close/release、normal RW/no-create reopen、`BEGIN IMMEDIATE`内の exact/valid-descendant WAL/limit
recheckを通る。receipt driftはno-write concurrent-source-change、recovery後handoff failureはStoreをpoisonし、result operationは
`backend-unavailable / sqlite-connection / reopen-required`、`compatibility()`とpin countはlast validated stateを返す。

process state は successful commit と cold validation 後だけ作る。DDL/metadataのprecommit failureでrollback/finalize/connectionが
健全でも一回closeを試み、close OKでだけ元のtrigger errorとno Storeを返す。close non-OKはinit-recovery opaqueとquarantineに
する。不確実ならfilesystemは同じfresh receiptでfinalize/一回rollback/一回closeを行い、
close OK後だけtotal reclassifyする。same-identity emptyは元のprecommit error、expected/authorized current-v3はidempotent success、
unsafe stateはtyped init-recovery resultとする。`:memory:` の不確実性はfinalize/一回closeを試み、close OKならsole DBを破棄、
non-OKならconnection/runtime/VFSをquarantineし、receipt/reopen/reclassifierなしでinit-recovery opaqueとno Storeに閉じる。
COMMIT outcome-unknown は同じcleanup/close gate後だけreceipt-aware total classifierを使う。journal arming前からseal済みのreceipt は
locator/VFS、preinit absent/exact-empty anchor、actual target main open-object identityとdirectory-entry binding、pre-arm raw
size/digest/sidecar census、expected empty-v3 projection、initialization IDをbindする。exact preinit emptyは
`store.sqlite-failure / database / opaque`、same identityのexact expectedまたはcompressed normal formで証明したv3 descendantは
recovered success、valid non-descendantは`sqlite-initialization-recovery / concurrent-authority-change`、partial/mixedはcorruptとする。
known successもそのalready-sealed receiptを使い、exactly one closeがOKだった後だけnormal RW/no-createでsame identity exact/authorized descendantを
full validateしてStoreを返す。close non-OKはinit-recovery opaque、quarantine、no Storeでありlock releaseを推測しない。
`:memory:` はsole MEMORY-journal connectionの一つのtransactionを保持し、COMMIT ambiguityでは同じone-close gateでStoreを返さず
`store.sqlite-failure / database / opaque`とする。

### Authority-state reachability and terminal recovery

rollback、COMMIT、close の不確実性は共通の
`cxxlens.sqlite-authority-state.v1`、`cxxlens.sqlite-authorized-descendant.v1`、
`cxxlens.sqlite-terminal-reclassifier.v1` に bind する。authority-state は SQLite storage class と exact value bytes、format/DDL/
metadata、publication row、owned chunk、head、fully validated committed generation maximumをlength-framed canonical bytesにする。
state equalityはbyte countとbyte-for-byte比較であり、SHA-256は探索の高速化だけに使いcollision時のauthorityにしない。state
maximumはcommitted rowがzeroのときcanonical tagged `none`、nonzeroのときtagged `some(exact u64 max)`とする。`none`をu128 zeroへ
写すのはallocation/reachability式の中だけであり、canonical state equalityでは`some(0)`と同一視しない。row付き`none`または
rowなし`some`はinvalid censusである。
0/1/2/4/5 のdiagnostic rowは、v2では publication ID、series ID、snapshot ID、sequence、generation、nullable parent、state、
stored checksum、raw BLOB、v3ではこれらにpayload byte/chunk countと全7-field chunk rowを加え、validation verdict vectorまで
byte/value exactに保持する。diagnostic rowの追加、削除、rewriteはauthorized descendantではない。

descendant は exact stateから五つのoperator、legacy-v2 publish、nonempty whole-v2 compaction、registered v2→v3 migration、
v3 publish、nonempty whole-v3 compactionだけのreflexive-transitive closureである。ただし判定は各edgeを世代差回replayせず、
source/targetから導出するcanonical run-length compressed normal formを使う。legacy v2 operatorは
already-running predecessor processのraceを認識するだけでcurrent binaryがv2 writeを発行するauthorityではない。各 publishは
one valid committed rowとhead CAS、各compactionはprior fully validated committed maximum+1..+countのwhole-set replacementであり、
diagnostic projectionを変えない。migration/compactionのcommitted payloadはdeclared v1〜v5 parserでembedded generationの
exact schema-specific 8-byte big-endian fieldを一つだけ特定し、old field valueとrow generation、prefix/suffix equality、target
decode/re-encodeを検査して置換する。schema versionとそれ以外のbytesはexactで、full/chunk checksumだけを結果bytesから導出する。
proofはsource committed count `S`、target count `R`、追加publish count `P=R-S`から、same-formatのno-reset/final compact `k`、
v2→v3のmigration-last `m`/final-v3-compact `(m,k)`だけをrow-count順に列挙する。tagged maximumを式内だけzeroへfoldした
`C=G_target-G_source-P-m`を一つのchecked residualとし、generation距離をv2/v3へ列挙分割しない。migration-lastは
`[max(1,S),m]`で`C`全体を解き、`m=0`ならzero residualだけを許す。final compactはdesignated `k`を一回引いてから
`[max(1,S),k]`でpre-final residualを一度だけ解く。v2→v3ではpopulation `<m`をv2、`>m`をv3、`=m`をcanonical v2へ
割り当てる。両formatのlegal intervalは`m`で重なり、同populationのmigration/compact resetはstable rankとmaximum offsetの下で
可換なので、このboundary ruleはreachable setを縮めない。この可換性は各committed rowのformat parse、logical projection、
generation rewrite、最終v3 physical projectionをbyte-exactに一度検証する。
population interval `[a,b]` のpositive residual `D` は、`q=ceil(D/b)`が`floor(D/a)`以下のときだけreachableである。
`d=D div q`、`r=D mod q`から`(d,q-r)`と`(d+1,r)`のzeroでないrunだけを作り、final edgeを別tagにするため高々三runである。
residualに同じpopulation `k`のrunがあってもdesignated final edgeとはcoalesceしない。
各candidateはfirst `k` target rowのcontiguous reset range/stable sequence-prior chronology rank/source ID包含と、残りのstrict-generation
publish suffixをbyte-exactに検査する。targetは少なくとも一つのcandidateが通れば受理し、報告用には
`(migration population, last-reset kind, last-reset population, run vector)`のlexicographically firstだけを選ぶ。
legacy-v2/v3 compact edge bitはcanonical runだけから決めず、各formatのlegal population `p`を一つ強制して`D-p`を同じclosed-formで
解けるかをORする。境界`m`は両formatのedge queryへ含め、全accepted candidateを集約する。
final reset前のcross-series orderはtargetのcontiguous replacement rank、reset後またはno-resetのpublish orderはstrict target generationから復元し、series topology/source generationと照合する。
whole resetはsequenceをprimary、unique prior generationをsecondaryにするstable sortなので、同一sequence内のpublish chronologyを
保持する。各nonzero population runは最初のrank transformだけを実行し、残る`count-1`回はorderがfixed pointでpublishが
interleaveしないことを検査してmaximum offsetだけを算術加算する。
candidate `(m,k)`とforced `p`の列挙を含むworkは高々`(committed-row-count+1)^3 + canonical-byte-count`、追加storageはrow countと
既存eager stateにlinearとする。

post-format operationのprewrite receiptは canonical locator/VFSに加え、`BEGIN IMMEDIATE`中のactual writer main handleのfile-instance
identity、directory-entry binding、prestate authority-state canonical bytesとdigest、operation kind/phaseをbindするが、candidate完成前は
candidateを持たない。candidate IDとexact projectionが完成した後だけcandidate receiptへ不可変に拡張し、COMMIT outcome-unknownは
complete candidate receiptを要求する。fresh-initは別profileとしてpreinit absent/exact-empty anchor、journal arming前のactual
target-main identity/directory-entry binding/raw size/digest/sidecar census、expected empty-v3をbindし、post-format authority-stateを
偽装しない。close後のpath replacementはbyte/semantic
exactでもconcurrent-authority-changeであり、descendantではない。cleanupはstatement finalize、必要なら一回のrollback、一回の
`sqlite3_close_v2`の順とし、rollback/finalize resultをdurable outcome authorityにしない。closeが`SQLITE_OK`でない、または
確認不能ならconnectionとruntime/VFS pinをquarantineし、reopen/unregisterせずStore result operationをreopen-requiredへpoisonする。

confirmed close後だけ、main absent/unreadable/replaced、namespace observation I/O/nonregular or non-equivalent object、unstable
namespace census、journal、stable presentだがunreadableな WAL/required SHM、readable WAL+SHM、readable WAL-only、SHM-only、no-sidecarの順で一度分類する。
namespace/object-kind failureはreclassifier unavailable、journal/unreadable sidecar/SHM-onlyは選択済みoperation phaseのopaqueに閉じる。
WAL+SHMは一つのactive read transactionとheld lock、WAL-onlyはheld main+WAL private
recovery、no-sidecarはheld main quiescent private copyを使う。journal、SHM-only、unreadable/drifting censusはopaque observation
failureであり、source recovery/cleanupや二回目snapshotを行わない。initializationだけはsame-identity exact-logical-emptyを
classification-onlyで受理するがStore stateにはinstallせずphase固有のno-Store resultを返す。publish/compactionはcurrent-v3、
migrationはv2/current-v3だけをadmitし、independently mutation-grade validated exact/authorized descendantだけをinstallする。
terminal classそのものはpublic resultではなく、exact operation/phaseを先に選び、そのphaseの
non-descendant、invalid、mixed、opaque規則を適用する。unsafe stateはinstallせずpoisonへ閉じる。

result precedenceはphase-specificである。publishはCOMMIT-attempted outcome-unknownならauthorized stateをinstallできる場合も、
main replacement、valid non-descendant、invalid/mixed stateまたはreclassification failureでpoisonする場合も、candidateの有無に
かかわらず`store.sqlite-failure / database / opaque`と`publication_outcome_unknown`を返す。precommit failureはCOMMIT未実行
なので、confirmed close後にcomplete concurrent candidateを含むauthorized stateをinstallできた場合だけ元のtrigger errorを返し、
それ以外は`store.sqlite-failure / database / opaque`を返してpoisonする。
candidate classifierはattempted physical projectionのexact presence、同じpublication ID/immutable logical projectionだがconcurrent
publishまたはlater compactionでgeneration/chunksが異なるauthorized presence、candidate identity absenceを別々にadmitする。
同じIDでimmutable logical projectionが違う、またはowned chunksがpartialな状態はinvalidであり、generation exactだけをpresence
条件にしてvalid descendantを取りこぼさない。exact `:memory:` のprecommit/COMMIT uncertaintyはfilesystem receipt/reopen/
reclassifierを使わず、finalize後に一回closeを試み、OKならsole DBを破棄、non-OKならconnection/runtime/VFSをquarantineする。
`compact()`はidempotent postconditionを使い、migration edgeまたはnonempty whole-v3-compaction edgeをreachability proofが含む場合は
success、edgeがなければ元のprecommit errorまたはCOMMIT-unknown opaqueを返す。zero committed authorityのv3 compactionは
`BEGIN IMMEDIATE`内でfull census後にno write/no COMMITとする。filesystemはrollback/finalize/confirmed close/reclassify後の
authorized stateだけをsuccessとする。exact `:memory:` はhealthyなrollback/finalize後にsole connectionをcloseせず保持して
successとし、terminal reclassifierを使わない。memoryのrollback/finalize/health uncertaintyはfinalize/一回closeを試み、
OKならsole DBを破棄、non-OKならquarantineしてphase固有opaque resultとpoisonへ閉じる。

`cxxlens_ng_publication` は immutable semantic publication metadata と current physical locator として
publication/series/snapshot ID、sequence、generation、parent、state、full payload SHA-256、logical
payload byte count、chunk count を持つ。payload bytes は `cxxlens_ng_payload_chunk` に分離する。chunk
row の primary key は `(publication_id, generation, chunk_ordinal)` であり、各 row は canonical
byte offset、exact byte count、SHA-256、BLOB を持つ。full/chunk checksum の TEXT spelling はいずれも既存
`content_digest` と同じ exact `sha256:<64 lowercase hex>` とし、raw 32 bytes、bare hex、semantic-v2 prefix は拒否する。
publication ID/generation の対応は独立 validator
が検査し、table rename を不透明にする implicit foreign-key rewrite を migration authority にしない。

v3 metadata は exact four rows
`physical_format=cxxlens.sqlite-semantic-store.v3`、`physical_format_version=3.0.0`、
`payload_chunk_profile=cxxlens.sqlite-payload-chunks.v1`、`payload_chunk_maximum_bytes=8388608`
を持つ。missing、duplicate、unknown additional format/profile row、value drift は mutation 前に reject する。

schema shape は table/column 名だけでは判定しない。Store contract の `schema_profiles` が v3/v2 ごとに
exact user table/index/view/trigger census、STRICT/WITHOUT ROWID、declared type、nullability、primary-key ordinal、
closed CHECK set、metadata row cardinality/key/value を定義する。v3 publication state は exact enum `0..5` を保持し、
semantic head/generation authority と reader visibility に寄与するのは committed state `3` だけとする。
chunk count/ordinal/byte count/BLOB length は closed CHECK と独立 validator の両方で拘束する。legacy v2 の rowid
`TEXT PRIMARY KEY` nullability quirk は schema capability として認識するが、row authority は全 primary key の
non-null/unique、state enum 0..5 の typed diagnostic classification、checked-u64、payload/semantic validation を要求する。
default/hidden column、BINARY collation、`index_list` / `index_xinfo` の unique/origin/partial/order/key/auxiliary
projection も closed profile に含める。extra user object、affinity drift、
partial/extra metadata は exact v2.6-compatible capability ではない。

v3 STRICT table を使用するため、fresh/current v3 と v2 `compact()` は mutation 前に
`sqlite3_libversion_number() >= 3,037,000` を要求し、未満は
`store.backend-unavailable / sqlite-runtime-version / 3037000` とする。v2 diagnostic read 自体には新しい version/16 MiB
floor を課さず、既存 runtime/symbol capability と実 payload の read result を保つ。v2 diagnostic open と migration は state enum
0..5 の全 persisted row を v3 に exact preserve する。non-committed row は head/generation allocation/counter authority
から除外し、drop、reclassify、semantic authority 扱いをしない。
dynamic library 不在、required symbol 不在、unsupported platform は既存 public tuple
`{store.backend-unavailable, sqlite, library|symbols|platform}` をそのまま使う。v3 が追加で必要とする symbol の不足も
`sqlite / symbols` であり、Option A はこの既存診断を collapse・rename しない。

- canonical chunk maximum は `8,388,608` bytes (8 MiB)。
- runtime の `SQLITE_LIMIT_LENGTH` は少なくとも `16,777,216` bytes (16 MiB) でなければならず、
  未満は `store.backend-unavailable / sqlite-limit-length / 16777216` とする。この floor は physical
  implementation condition であり public request/payload cap ではない。
- non-final chunk は exact 8 MiB、final chunk は 1..8 MiB とし、zero-length chunk を禁止する。committed
  publication は 1..2,199,023,255,552 chunks を要求する。v2 の corrupt noncommitted empty BLOB を exact preserve
  できるよう、noncommitted row だけは `payload_byte_count=0`、`payload_chunk_count=0`、chunk row なしの diagnostic
  representation を許す。これを committed row や nonempty payload に使えば publication-local corruption である。
- ordinals と byte offsets は canonical unsigned 64-bit で、それぞれ 0..chunk-count-1 と
  `chunk_ordinal * 8,388,608` に exact 一致し、gap、duplicate、extra を許さない。u64 payload length から
  chunk count は最大 2,199,023,255,552、ordinal は最大 2,199,023,255,551 と導出されるため、ordinal は
  SQLite nonnegative signed INTEGER domain 内に収まる。row/page/insertion order は authority ではなく、read は
  numeric `ORDER BY chunk_ordinal ASC` を必須とする。負値を ordinal として decode しない。
- open/replay は各 chunk checksum、ordinal/count、row length、aggregate logical length、full payload
  checksum、既存 canonical decode/re-encode、bottom-up semantic graph/manifest/publication identity の順に
 検証する。missing、orphan、foreign-generation、reorder、truncation、相関 checksum 改変を含む不一致は
 既存 corruption result で fail closed とし prior publication へ fallback しない。

validation order は exact schema/user-object census、structural publication/chunk owner-orphan census、publication-local
chunk/full-checksum/semantic verdict、fully validated committed authority だけの global generation uniqueness/retired census
とする。current generation に閉じた missing/extra/duplicate/order/offset/length/checksum/semantic defect は該当 publicationを
retained/marked-corrupt とし、`current` / `open(snapshot)` / `open_publication` はそれぞれ既存の corrupt result を返す。
state=3 でも local corruption のある row は generation authority から除外するため、valid row と generation が衝突しても
publication-local corrupt のままである。noncommitted diagnostic row の generation collision は publication-local chunk bindingが
exactなら許す。unknown publication orphan、fully validated committed noncurrent/retired generation、fully validated committed
authority 間の duplicate generationだけを exact
`store.corrupt / sqlite-chunk-authority / global-orphan-retired-or-duplicate-committed-generation` とする。v2 migration と v3 ordinary
compaction のいずれも、corrupt committed publication を検出した場合は `(prior sequence, prior generation, publication ID)`
順の最初の offender に対する既存 `store.compact-validation-failed / offending-publication-id / empty-detail` を返し、writeを
開始しない。

v3 production path は prepared statement と bound/column BLOB API を使う。SQL hex literal、
`hex(payload)`、`group_concat`、全 payload または全 chunk の一括 resident vector を禁止する。
canonical-v5 encoder/decoder、incremental SHA-256、chunk writer/reader は bounded/replayable とし、checked
u64/u128 accounting を使用する。memory backend の final immutable payload 一 owner は許されるが、
prepublication occurrence graph や複数の full payload copy を許可しない。

### Explicit v2 to v3 migration

v2.6.0 の `compact()` は、長寿命の read-only/query-only reader を mutation connection に昇格しない。exact same
canonical path/VFS を read-write/no-create で開いた distinct compact connection による一つの `BEGIN IMMEDIATE`
copy-on-write transaction で database 全体を移行する。read-write open直後かつlimit/lock/effect前にowned VFSのinput role/requestと
returned `pOutFlags`で`SQLITE_OPEN_READONLY`がclearであることを証明できなければ
`store.sqlite-failure / open / read-write-required`でrejectする。

1. open 時に eager decode した v2 の locator/VFS、全 persisted semantic/diagnostic row、head を established anchor とする。
   write lock 取得後かつ最初の schema/data write 前に format/schema と全 persisted row、および committed
   publication/head authority を再読し、established anchor の exact または recovery と同じ valid-descendant であること、
   v2 payload bytes、full
   checksum、accepted payload v1〜v5 の decode policy、semantic graph、snapshot/publication identity、parent
   topology、series head、counter authority を検証する。fully valid でも descendant でない file replacement/authority は
   no-write の `store.sqlite-failure / migration-source / concurrent-authority-change` とする。正当な concurrent publish による
   descendant はその latest locked census を migration source として取り込む。
2. main schema に final 名とは異なる shadow v3 metadata/publication/chunk/head tables を作る。committed row だけを既存 compaction と同じ
   `(prior sequence, prior generation, publication ID)` order で、prior fully validated committed maximum+1から
   maximum+committed-row-countまでのchecked distinct contiguous generation rangeを割り当てる。committed rowがなければrangeを
   割り当てない。accepted payload schemaとcanonical field encodingを維持し、declared schema parserが一つのembedded
   `physical_generation` fieldのexact 8-byte big-endian rangeを特定し、old valueがsource row generationと一致すること、prefix/
   suffix equalityとtarget decode/re-encodeを証明して新しいauthorized generationへ置換する。それ以外のpayload
   bytes、semantic/publication/snapshot ID、series/parent/sequence/head、export と query semantics は
   byte/semantic exact に維持する。noncommitted row はpublication ID、series ID、snapshot ID、sequence、generation、nullable
   parent、state、stored checksum、raw payloadの全source columnとtyped verdictをexactに維持したまま
   chunk 化し、新しい per-chunk checksum だけを付与する。corrupt noncommitted row も source と同じ typed diagnostic
   classification で保持し、head/counter/generation/semantic authority に昇格しない。v1〜v4 を v5 に変換せず、不足
   field を推測しない。
3. bounded reader から shadow chunk rows を書く。committed row は chunk/full checksum、canonical payload、semantic
   graph/identity、authority census/head を独立 replay する。noncommitted row は raw length、chunk 再結合、保存した
   full-checksum bytes、source typed diagnostic verdict の一致を検証する。source verdict が corrupt のとき、その checksum
   や decode の成功を要求して修復・昇格してはならない。
4. shadow 全体の検証後、同じ transaction 内で legacy objects を drop し、Store contract の exact six canonical DDL
   statements をそのまま実行して final objects を新規作成する。shadow object の rename は `sqlite_schema.sql` を書き換え、
   index 名も canonical にできないため禁止する。shadow から final publication/chunk/head と marker 以外の three metadata
   rowsを bounded copy し、final schema/data/row-class projection を再検証して全 shadow object を drop する。
   `physical_format=cxxlens.sqlite-semantic-store.v3` を最後に insert し、exact six-statement DDL digest、four metadata rows、
   full final census を再検証して commit する。cold reopen でも同じ DDL digest を要求する。process-local committed census と
   generation pin は DB commit 成功後だけ入れ替える。

validation、allocation、DDL、chunk write、swapのprecommit failureではCOMMITを呼ばず、rollback/finalize/confirmed close後に
共通terminal reclassifierを使う。authorized proofがv2 stateのまま、またはmigration edgeを含まなければ元のtrigger errorを返す。
exactly one v2→v3 edgeを含むsame-main valid descendantなら、別migratorによるidempotent `compact()` postconditionが満たされた
ためindependently validated v3をinstallしてsuccessとする。lock取得後かつfirst write前に別migratorのreachable v3を観測した
場合も、writeせずrollback/finalize/confirmed close/reclassifyし、same identityとexactly one migration edgeを証明してsuccessにする。
fully valid non-descendant は `migration-source / concurrent-authority-change` とする。

COMMIT の返り値からdurable outcomeを判定できない場合、receiptはcanonical locator/VFS、actual writer main file instance/
directory entry、pre-v2 full authority-state canonical bytesとdigest、expected-v3 full projection、migration IDをsealする。
confirmed close後のv2 resultはlegacy-v2 publish/nonempty whole-v2 compactionのcompressed normal form、v3 resultはそれらの後に
exactly one migration edgeとv3 publish/compactionを同じclosed-form proofで証明したstateに限定する。committed payloadはschema versionとgeneration以外の
bytes exact、diagnostic rowは全column/chunk/verdict exactであり、diagnostic row additionも禁止する。reachable v2なら
`store.sqlite-failure / database / opaque`を返してv2 stateをinstallし、reachable v3ならrecovered successとv3 stateをinstallする。
schema/data が
fully valid でも source descendant でない concurrent replacement は
`store.sqlite-failure / migration-recovery / concurrent-authority-change` とし、invalid census だけを
`store.corrupt / migration-recovery / unexpected-census`、mixed layout、orphan shadow table、marker/table disagreement、
unknown/ambiguous state を `store.corrupt / migration-recovery / mixed-or-ambiguous` とする。推測、retry write、silent
completion をしない。
closeが`SQLITE_OK`でない場合はconnection/runtime/VFSをquarantineしてreopen/unregisterせず、またconfirmed close後の
reclassification/reopenが失敗した場合も、`compact()` は `store.sqlite-failure / migration-recovery / opaque` を返し、元の
Store instance を poisoned state にする。その instance の result-returning 後続 operation は
`store.backend-unavailable / sqlite-connection / reopen-required` とし、stale process state を返さない。非 result API の
`compatibility()` は last independently validated source tuple の `direct_open=false` を返し、
`retained_generation_count()` は preexisting live process pin の exact count を返す。attempt 前から存在する handle/cursor
は元の immutable process generation pin の下で有効なままである。recovery は caller が同じ locator から新しい
`snapshot_store` instance を open する既存境界だけを使う。

migration 前から存在する handle/cursor は既に decode 済みの process-local immutable generation を最後の pin 解放まで
読み続ける。SQLite cursor は chunk table を lazy に参照しない。validated locator swap と同じ transaction で旧 durable
chunk rows を削除してよく、commit 後の orphan/retired generation row は corruption とする。別 connection が migration を commit した後の既存 v2 writer は schema/format epoch の transaction
内再検査で fence され、publish や hybrid write を行えない。migration 後に再 open した Store だけが v3 mutation
authority を得る。v3 の通常 compaction も同じ COW、full census、contiguous generation、pin、rollback 規則を使う。
通常 compaction は committed row だけに新 generation/chunk を作り、noncommitted row/chunk は元 generation、bytes、
checksum、typed diagnostic classificationだけでなくpublication ID、series ID、snapshot ID、sequence、nullable parent、state、
payload length/countと全chunk fieldをbyte/value exactに保持する。committed authorityがzeroなら`BEGIN IMMEDIATE`内でfull census後、
write/COMMITをしない。filesystemはrollback/finalize/confirmed close/reclassify、exact `:memory:` はhealthy rollback/finalize後に
sole connectionを保持するno-close success pathを使うため、どちらもCOMMIT outcome-unknown branchへ入らない。
通常 compaction は open 時の all-row/head/locator/VFS/file identity を established anchor とし、write lock 内で exact または
valid descendant を確認してから最初の write を行う。valid non-descendant は
`store.sqlite-failure / compaction-source / concurrent-authority-change` とする。precommit failure後でもconfirmed close/reclassificationの
authorized proofがnonempty whole-v3 compaction edgeを含めばidempotent compact success、含まなければ元のtrigger errorを返す。
COMMIT outcome-unknown は main file instance/directory entry、pre-v3 full authority-state canonical bytes+digest、locked census と
expected compaction projection bytes+digest の receipt を seal してconfirmed close後にtotal reclassifyする。exact expected candidate
projectionはopen-time pre-anchorがzeroで、lock前のconcurrent publishを今回のcompactがrewriteした場合も直接success proofになる。
それ以外のlater-compacted descendantはcompressed normal formにpositive locked-census populationのv3 compact runを要求し、
open-time pre-anchor rowの置換だけをobservable条件にしない。proofがこのnonempty v3 compaction edgeを含むexpected または
later-compacted valid descendant は recovered
success、exact pre または valid uncompacted descendant は `store.sqlite-failure / database / opaque`、valid non-descendant、
invalid、mixed はそれぞれ `compaction-recovery / concurrent-authority-change`、`store.corrupt / compaction-recovery /
unexpected-census`、`mixed-or-ambiguous` とする。reopen failure は Store instance を poison し、migration と同じ
reopen-required/non-result observer/pin 規則を使う。close non-OKはquarantineしreopenしない。ephemeral `:memory:` のuncertaintyは
finalize/一回closeを試み、close OKならsole DBを破棄、non-OKならconnection/runtime/VFSをquarantineしてStoreをpoisonし、
`store.sqlite-failure / compaction-recovery / opaque` を返す。implicit retry write は行わない。
ここで pin 後回収とは decoded process generation の lifetime であり、durable chunk row の cross-process pin protocol を
意味しない。将来 lazy SQLite cursor を導入する場合は別の durable retention authority を先に定義する。

### Release and authority binding

Release Bundle の `snapshot-format` axis version `3.0.0` は、direct Store `readable_format` tuple ではなく backend-qualified
feature と exact contract digest を束ねる release capability-bundle version である。memory の direct Store tuple 2.6.0、
SQLite current 3.0.0、SQLite predecessor 2.6.0 は各 feature に保持し、axis version と相互代入しない。current binaryは
memory direct、SQLite v3 direct、v2 read-only、explicit compact handlerの全truthful capabilityをaxis 3.0.0で提示し、pre-migration
DB stateだけをbackend-qualified `sqlite-store-artifact-v2.6.0-pre-migration` context featureにする。registered migrationの
2.6.0→3.0.0は`sqlite-store-readable-format` coordinateであり、release axis versionではない。このcontextとrequired handler
featuresが揃う場合だけmigration-requiredで、capabilityの抑制やmemory tupleのaxis代入を禁止する。SQLite qualified report は current v3.0.0、v2.6.0 read-only direct-open、
registered `compact-v2.6.0-to-v3.0.0` migration の結果を明示し、memory/SQLite/static/shared report-set digest と
cold-reopen projection に bind する。v2 migration evidence は compatibility evidence であり、v2 write support を
意味しない。これら四 case は exact revision、source tree、SQLite contract digest に bind した
`cxxlens-ng-sqlite-store-v3-qualification-${revision}` artifact として保存し、schema は
`schemas/cxxlens_ng_sqlite_store_v3_qualification_report.schema.yaml` とする。
現段階の report schema は planned shape であって release qualification evidence ではない。implementation activation は producer、
独立 relational validator と negative tests、schema/checker/report-set exact digest、chunk census/peak RSS/elapsed/disk measurements、
GR と release consumer の atomic binding を全て追加した時だけ行う。schema-valid な自己申告 boolean 単独を green evidence にしない。

## Consequences

- public callable を増やさず、既存 `compact()` が maintenance と migration の唯一の明示境界になる。
- `store.migration-required` は public Store error set への additive result state であり、旧来の
  「public results は全く不変」という記述は撤回する。signature、success semantics、cursor lifetime は不変である。
- v2 を暗黙に書き換えないため、operator は read を継続しつつ migration 時点を選べる。
- physical implementation は大きくなるが、single-row SQLite ceiling と SQL hex amplification を semantic/API
  versioning から分離できる。
- Alternative B（successor request/budget と両 backend 共通 payload cap）および parity weakening は選択しない。

## Verification

- authority/schema checker は v3 ID/version、table/column/key、8 MiB/16 MiB bounds、prepared BLOB、v2 read-only
  matrix、exact migration error、sole `compact()` trigger、single-transaction COW、same-semantic-digest、no implicit
  migration を exact に検査し、一項でも欠落・変更すれば reject する。
- Store unit は fresh v3、chunk boundary/order/corruption、v2 zero-mutation read parity、exact begin rejection、
  empty/populated/v1〜v5 migration、pin、multi-instance writer fence、ordinary v3 compaction を検査する。
- fault matrix は fresh initialization と migration の transaction/DDL/first-middle-last chunk/final-object-copy/marker/commit
  の各境界と subprocess crash を挿入し、
  cold reopen が exact logical-v2 schema/rows/payload/metadata/head projection または fully validated v3 のどちらかだけになることを検査する。
- qualification は `SQLITE_LIMIT_LENGTH` を超える実 canonical-v5 payload を bounded に生成し、memory と
  cold-reopened SQLite、static/shared の semantic/export/query parity、chunk census、peak RSS、elapsed time、disk
  bytes を exact revision/tree の evidence に記録する。
- `:memory:` example/install success、active-WAL single-snapshot read、cold-reopen canonical DDL digest、valid concurrent
  recovery descendant、publication-local chunk corruption と global orphan/retired chunk corruption を独立に検査する。

## Review disposition

Issue #200 で選択された fresh Option A decision と本 ADR、統合設計、SQLite/Snapshot/
Materialization contract/schema/checker/tests の exact binding は、二つの distinct independent
falsification review で ACCEPT となった。この結果により physical Store authority として Accepted
とし、Store v3 implementation は開始できる。implementation、required evidence、release qualification は未完了であり、
後続の検証なしに production-qualified とは扱わない。
