# ADR 0096: Clang 22 installed materialization を provider-owned JSON tool に閉じる

- Status: Accepted
- Date: 2026-07-19
- Issue: #182; machine-v2.1 amendment #181
- Design feedback: DF-0182 / #182; DF-0187 / #187; DF-0191 / #191; DF-0192 / #192; DF-0194 / #194; DF-0195 / #195; DF-0196 / #196; DF-0197 / #197; DF-0198 / #198; DF-0199 / #199; DF-0200 / #200
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
`cxxlens.clang22-materialization-request.v2` を受け、
`cxxlens.clang22-materialization-report.v2` response を返す。両者と tool semantics の machine authority は
`cxxlens.clang22-materialization-contract.v2` とし、machine contract document、`tool.interface_version`、request、report の exact version はいずれも
`2.1.0` とする。
transport は stdin 上の一つの JSON request と stdout 上の一つの JSON response に固定し、stderr は diagnostic-only とする。shell command
construction、stdout の複数 report、stderr prose による outcome 判定を禁止する。

process invocation は `argc == 1`、すなわち option/operand が一つもない形だけを受理する。`argv[0]` は diagnostic observation に限り、
path、prefix、binary identity の authority としない。unexpected argv は stdin を読まず、worker launch、Store open、file effect を行わず、
stdout zero bytes / exit 2 とする。bounded stderr diagnostic は許すが、JSON failure response と混同しない。

v2 request/response の version は exact `2.1.0` とし、missing/unknown field、unknown version、identity/digest mismatch を effect 前または
publication 前に拒否する。raw byte は BOM のない exact UTF-8 として一つの top-level JSON object だけを decode し、任意 depth の duplicate
member、invalid UTF-8、BOM、non-finite number、object 以外の top-level value、二つ目または trailing JSON value を拒否する。authority YAML
loader や permissive/last-key-wins decoder を request/response transport に流用しない。未実装・未 qualification の v1 artifact は v2 input/evidence
として解釈せず、2.1 rollout が supersede する。prior minor artifact を 2.1 input/evidence として読み替えず、2.1 に implicit migration、adjacent version
fallback、unknown-field preservation、caller-selected default を設けない。request は exact project catalog census、expected parent publication、
condition/interpretation、worker/provider/toolchain/environment identity、source/input/invocation digest と budget/security binding を失わず、tool が全
compile-unit task を bottom-up に導出する。
caller が task ID、descriptor digest、dependency group、publication result を差し替えることはできない。

request validation は raw byte 上限、strict top-level JSON object、`{schema, request_version}` envelope、version dispatch、選択した version の full
schema、derived identity/binding の順で行い、最初に失敗した boundary だけを response phase とする。従って supported version の shape failure を
`request-version` としたり、未知 version を現行 full schema へ通したり、複数 failure のうち後段を選ぶことを禁止する。

stdin は request-level で最大 1 GiB (`1073741824` bytes) とし、tool は decode 前に最大 `1073741825` bytes だけを読む。上限内なら response の
`raw_input_observation` は complete input の byte count と `sha256` digest を保持する。上限超過では exactly `1073741825` bytes の consumed prefix の
count/digest と `complete: false` だけを保持し、未読 suffix の size/digest を主張しない。publication 前の response construction failure は compact
failure を生成できる場合だけ zero-effect typed failure とする。正常な typed response bytes を stdout へ書けない transport failure だけは response 自体を
authority にできず exit 2 とする。Store commit 後の broken stdout を zero-commit failure と偽らず、committed Store record を唯一の recovery authority とし、
missing response は release evidence にならない。stdout に schema-valid failure response を書いた場合は exit 1、passed detailed response は exit 0 とし、
error kind は exit status や stderr ではなく response の typed field で決める。

1 GiB request を一つの DOM として保持せず、raw request は bounded chunk reader から一つの immutable private spool へ保存する。pass 1 は DOM を
作らず strict UTF-8、JSON lexical shape、任意 depth の duplicate、envelope、version dispatch を検証し、pass 2 は同じ spool を replay して selected
v2 schema、bottom-up binding、base64 の decoded string を検証する。`source.content_base64` は RFC 4648 standard alphabet、required padding、
zero discarded padding bits を満たす canonical decoded string 一つだけを受理する。raw JSON token の `=` と `\u003d` のような escape 差は同じ
decoded string として identity を変えず、同じ source bytes へ decode できても non-zero discarded padding bits を持つ spelling は full schema で
derived binding 前に拒否する。source spool の bytes/count/content digest/line index を source authority とし、sealed bytes から一意に再生成した
canonical Base64 と request decoded string の exact 一致を検証して task.v3 へ投影する。global catalog は一つの immutable value とし、task は
compact spool-backed index だけを保持して canonical order で一件ずつ task.v3 を構築、実行、seal、破棄する。

pass 1 の generic array count と non-envelope value-string length は raw 1 GiB ceiling から到達不能な lexical ceiling だけを持ち、selected schema の
path-specific `maxItems` / `maxLength` を generic scanner limit として合成しない。object member は duplicate-name ledger が resident であるため
4096 を明示 lexical resource ceiling、member name は UTF-8 256 bytes、depth は 64 とする。top-level `tasks` と
`trust_policy.task_sandbox_requirements` は selected schema がそれぞれ `maxItems: 4096` を所有する。4097 件目を観測しても残りの strict lexical scan と
version dispatch を完了し、より前段の lexical/version failure がなければ `request-schema` / `materialization.request-invalid` で拒否する。
trust requirement の `uniqueItems` は schema 検証済み `minimum` と `policy_digest` の bounded string-view projection を一回 sort し、隣接する exact tuple
だけを比較する O(N log N) とする。4096 件の catalog compile-unit uniqueness も固定 member の bounded view tuple で同様に判定し、巨大 canonical
JSON copy または attacker-controlled O(N^2) equality を作らない。

top-level `schema` と `request_version` は全 input の strict lexical validation を続行しながら、それぞれ expected literal の decoded UTF-8 length+1
bytes（43 と 6）までだけ保持する。長い値を raw request から `std::string` へ複製せず、完全 scan 後に envelope/version precedence を判定する。

`tasks.uniqueItems` は task ごとの canonical metadata/source payload を補助 spool に複製しない。各 task について raw SHA-256 32 bytes、logical
payload length、task ordinal、raw task offset/length だけの fixed 64-byte record を private spool に置き、seal 後に一回読み取った bounded compact
index を sort する。同じ digest/length の組だけ raw request を replay し、左 metadata の canonical form は collision-only private memfd へ seal して
seal 後の bounded reverse read で左 canonical bytes の exact size/content を再照合する。その closure 後に DOM を破棄し、右の一つの bounded metadata
window と exact compare する。`content_base64` は raw token spelling ではなく JSON escape decode 後の
string を fixed buffers で比較し、digest match だけを JSON Schema equality authority にしない。execution tuple uniqueness も fixed record と exact
three-string replay を用い、all-task payload retention と全 header の O(N^2) scan を禁止する。

DF-0197 の security addendum として、raw request、decoded source、task index、task.v3、report projection に使う private spool は Linux の
`memfd_create(..., MFD_CLOEXEC | MFD_ALLOW_SEALING)` で生成した private memfd だけとする。pathname、`mkstemp`、unlink 後の mutable inode への
fallback を禁止する。logical sealed state を公開する前に `F_ADD_SEALS` で
`F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL` を不可逆に追加し、続く `F_GET_SEALS` で全 required bits を再観測する。
その kernel seal 後に `fstat` の actual size と append census、sealed bytes の SHA-256 と append transcript の incremental SHA-256 を照合する。
logical sealed state は required bits、actual size、sealed content の全照合後にだけ公開し、seal 前の `/proc/<pid>/fd/<fd>` 経由の content/grow/shrink
drift は worker、Store、file effect 前の source-private no-response とする。
Linux memfd、`MFD_ALLOW_SEALING`、seal constants のいずれかが compile target にない場合、`memfd_create` が失敗した場合、seal の追加・取得が
失敗した場合、または一 bit でも欠ける場合は、最初の raw spool create/add/get/required-bit observation を capability gate とする。
worker、Store、file effect 前に拒否する。compile-time absence と missing-bit contradiction は no-response、actual create/add/get syscall failure は
raw observation 後の active phase だけ phase-authentic stable failure とする。
いずれも mutable storage へ downgrade しない。

complete raw-input observation を保持できない最初の raw spool create/capture failure は exit 2 / no authoritative response とする。raw spool が既に
sealed された後の raw/global/task replay、task-index、source spool、task.v3 spool、identity digest、uniqueness auxiliary の create/append/seal/read/digest
に対して port が返した actual I/O/hash failure は stable code `materialization.spool-failure` を使用する。lexical raw/task-index は `json-decode`、selected-schema global/task/source/index/
uniqueItems は `request-schema`、full schema 後の source/task.v3/identity/task-index/execution uniqueness は `request-binding` に固定する。Base64 grammar、
shape、schema 所有 size/cardinality ceiling だけを `materialization.request-invalid` とする。valid request の infrastructure failure を
`materialization.request-invalid` や未登録の `materialization.io-failure` / `materialization.resource-limit` / `materialization.internal-failure` へ射影しない。
ここで stable failure は port が返した actual I/O/hash failure に限る。allocation kind、または成功を返した read/seal/digest の count、sealed state、
record census/ordinal/raw span、receipt/digest grammar が既に証明した authority と矛盾する場合は内部 corruption/invariant breach とする。
operation-authentic kind×operation matrix は `input_read×read`、`spool_write×(write|spool)`、
`spool_seal|spool_create|spool_rewind×spool`、`spool_read×(read|spool)`、`digest_update|digest_finalize×hash` だけを stable とし、
configuration/buffer-allocation と全 mismatched pair は no-response とする。selected replay の root member missing/extra、task/source/
`content_base64` missing/non-string は `request-schema` では `materialization.request-invalid`、`request-binding` では no-response とする。
collision reverse read の actual read failure だけは stable、successful write-drop、size/count drift、zero/over-report、content corruption は no-response とする。
proved maximum を越える task.v3、phase-opaque allocation、内部 corruption/invariant breach、または compact report を安全に構築できない failure は stable report code を持たない
source-private no-response signal とし、driver/runtime は exit 2 / stdout zero にする。

selected-schema replay の `67,108,864` byte window は raw token spelling の上限ではなく semantic JSON token replay の内部上限とする。
insignificant whitespace は捨て、JSON string は decoded UTF-8 から minimal JSON escaping だけで再生成し、integer spelling は同値の canonical
decimal に正規化する。global replay は `tasks=[]`、task metadata replay は `source.content_base64=""` を代入する。
closed/required/local-ref/`allOf` intersection/`oneOf` maximum の selected-schema derivation が証明する global canonical maximum は
`10,420,985` bytes、task metadata maximum は `8,463,179` bytes であり、`67,108,864` byte window に対する margin はそれぞれ
`56,687,879` bytes と `58,645,685` bytes である。derivation projection は
request schema 全体の parsed YAML→key-sorted compact canonical JSON fingerprint
`sha256:241fc96ae3a249e5a8851baa95e585460ad29378cb20d11cfcda33a69eaa9270` を含み、
`sha256:ff9baf9982f909d8a4f51c46f53637af6980a7d06728dfa65794ffc1eebf816d` に固定する。raw-spelling inflation（whitespace、
等価 Unicode escape、長い zero exponent）はこの semantic bound から除外し、1 GiB raw request reachability を狭めない。schema walk、
substitution、normalization、bound、parse result の drift と allocation failure は source-private no-response、port の actual I/O/hash failure
だけは既定 phase の `materialization.spool-failure` とする。

task は最大 4096、task ごとの decoded source は 16 MiB、全 source は 512 MiB、response は 1 GiB、JSON depth は 64、object member は 4096、member name は UTF-8
256 bytes、strong ID は 512 Unicode scalar / UTF-8 2048 bytes、logical path は UTF-8 4096 bytes、SQLite relative path は UTF-8 4095 bytes、
argv は最大 4096 items / item UTF-8 2048 bytes とする。retained-memory bound は一つの shared catalog、fixed parser/chunk buffer、一つの task-index
window、一つの decoded-source spool window、一つの output-validation window の和とし、raw 1 GiB request、aggregate source、全 task payload、
`task_count * catalog_count` copies を resident set に含めない。task index と bulk occurrence は private spool へ置くため、task 数に対する absolute RSS
independence は主張しない。

DF-0194 の pre-qualification erratum として、report construction は irreversible な Store publication boundary を挟む bounded two-phase とする。
publication 前には sealed materialization DAG と Store candidate を完成・検証し、publication-independent な detailed projection を bounded private spool
へ構築して独立検証し、final JSON framing、全 applicable detailed outcome、exact SDK record/receipt、bounded diagnostic の checked maximum を加えても
response 上限を越えない capacity を予約する。この phase で completed
schema-valid report、予測または placeholder の publication record/physical generation、架空の reopen receipt を主張してはならない。ここでの
construction failure は compact response 自体を完成できる場合だけ `report-construction` / publication-not-attempted / zero-commit とし、それも完成できなければ
exit 2 とする。

exactly one `publish()` call の開始をもって publication-attempted boundary を越える。その後は actual SDK outcome、publish が返した exact record、SQLite
close/reopen または memory Store の exact three-path receipt、最初の typed failure または verification mismatch だけから一つの final private report spool を
完成し、report v2 full schema と独立 semantic validator の両方を通してから stdout の最初の byte を書く。boundary 後の finalization、validation、allocation、
spool、stdout failure は commit の有無にかかわらず compact zero-effect response へ downgrade せず、exit 2 / no authoritative response / no release evidence
とする。commit 済みなら committed Store record だけを recovery authority とし、unknown または rejected outcome も phase-authentic Store proof なしに
zero-commit と主張しない。SQLite の post-attempt exit 2 は blind retry せず、exact selector、observed parent、candidate snapshot/publication を read-only で
調べる。stdout の authoritative unit は exactly one complete JSON response とし、short/partial write の bytes は non-authoritative、OS-level
all-or-nothing atomicity は non-claimed とする。memory backend の process-local Store は復元不能なので fresh run だけを許す。

この tool は installed process/machine surface であり、新しい public C++ Clang host bridge ではない。既存の
`process_execution_report`、`provider_session`、native SDK に Clang 22 task authoring、sealed result、Store publication method を追加しない。
一般化された public provider adoption API もこの decision では認可しない。

portable task、provider protocol、provider runtime はこの specialization より上流の generic dependency のまま維持する。各 generic contract から
本 contract への参照は discovery 用の `reverse-specialization-index` であり、`dependency: false` とする。従って明示 dependency DAG の向きは
materialization contract から generic contracts への一方向であり、generic validator や public runtime が Clang 22 specialization に依存することを
意味しない。

### Installed occurrence and effect root

installed physical occurrence の source-private manifest は schema `cxxlens.clang22-materializer-occurrence-manifest.v1`、fixed path
`share/cxxlens/materialization/clang22/occurrence-v1.json` とする。manifest payload は source revision/tree、package configuration、closed role/path/digest
inventory と payload digest を持ち、自身の file entry を inventory に含めず canonical payload の self-digest field を除いて hash する。従って
runtime manifest graph は非自己参照である。external install-artifact manifest は occurrence manifest bytes を含む relocated prefix 全体の独立
full-prefix witness のまま維持し、その full-prefix digest を request または semantic snapshot identity に混入させない。

manifest generation の source revision/tree は必ず pair で与える。controlled packaging が exact pair を明示注入しない場合は Git checkout から pair を
導出してよいが、install 時に configured pair と `HEAD` / `HEAD^{tree}` が二回の観測で一致し、staged、unstaged、untracked を含む porcelain status が
両観測で empty であることを fail-closed に確認する。configure 後の commit/tree drift、dirty checkout、片側だけの明示指定を occurrence provenance として
受理しない。

Linux runtime は `argv[0]` や `PATH` ではなく `/proc/self/exe` を open して actual executable object を測定する。deleted/non-regular object を拒否し、
その object が prefix-relative exact `bin/cxxlens-clang22-materialize` と同じ device/inode object であることから prefix dirfd を確立する。manifest、worker、
authority file はその dirfd から `openat2(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS)` 相当で open し、各 opened FD の hash 前後の
stable stat と regular-file verdict を確認する。各 role は measurement 中に exact byte stream を一つの private memfd へ copy し、digest/size を照合後、
`F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL` を適用して exactly one の immutable snapshot を保持する。worker/schema/DSO の後続
consumer へは同じ sealed inode の独立 read-only handle だけを渡し、installed path/mutable inode の再 open、同一 role の再 copy、未 seal bytes の使用を
禁止する。source revision/tree、configuration、closed role/path inventory、tool/worker/authority digests と request の expected occurrence-manifest digest を
worker launch または Store effect より前に exact compare する。unsupported kernel/filesystem/sealing assurance、snapshot copy/write fault、path escape、
shadow、symlink/magic-link、measurement 中の object replacement は fail closed とする。measurement 完了後の path replacement または installed inode の
mutation は retained snapshot bytes に影響せず、report は caller requested occurrence と runtime measured occurrence を別々に保持する。

SQLite effect root は process start 時の current working directory を request parse 前に一度 dirfd として capture した値とする。request path は absolute、
root/drive、empty、`.`、`..`、NUL、backslash、double separator、normalization change を持たない canonical relative UTF-8 とする。database 本体だけでなく
WAL、SHM、journal も source-private rooted SQLite VFS から captured dirfd beneath に解決し、lexical check や SQLite の ambient CWD fallback だけで
安全性を主張しない。`openat2` beneath/no-symlinks/no-magiclinks と leaf no-follow を満たせない環境は Store effect 前に拒否する。report の
`rooted-vfs-v1` receipt は root observation digest、exact relative path、parent/leaf resolution verdict を保持するが、snapshot semantic identity と
memory/SQLite parity identity からは除外する。

この VFS は process-local の named non-default VFS として登録し、返却した `snapshot_store` は source-private の type-erased backend lifetime token を
SQLite connection の完全な破棄まで保持する。破棄順序は SQLite connection close、VFS unregister、captured root dirfd close、SQLite library release の
順を必須とし、opener の破棄だけでは Store が参照する VFS authority を失わせない。main database の exact relative path authority は active open count で
管理し、最後の main-database close で新規 authority acquisition を revoke し、pending open と in-flight lease がすべて drain した後だけ entry を除去する。
backend lifetime bridge は installed public header に callable signature を追加せず、shared tool が kernel DSO を参照するための source-private
default-visibility access class に閉じる。この internal DSO linkage symbol は catalog 対象の public C++ API または supported external ABI ではない。
access class は `snapshot_store` の bridge-specific な唯一の friend type とし、exactly one static `open_sqlite` member だけを持つ。method 単体の
visibility override を禁止し、required static/shared build-test は rooted VFS target の link と実行を両構成で検証する。
named file namespace は authenticated active main database とそこから導出した WAL、SHM、journal に閉じ、
named `xOpen` は main database、main journal、write-ahead log 以外を拒否する。anonymous `xOpen` は temporary database、transient database、temporary
journal、subjournal の closed set だけを private transient object として受理する。`xShmMap` / `xShmUnmap` は active authenticated main-database handle
だけに許可し、anonymous temporary または named non-main handle からの SHM path construction/deletion を拒否する。`xAccess` / `xDelete` は active main
database またはその known sidecar だけを認証し、closed database、suffix-only path、unknown suffix を拒否する。

SDK の `sqlite3_open_v2` が返した raw connection は return 直後から stack RAII guard が所有し、open-error diagnostic の materialization と heap owner
construction の両方を通じて保持する。heap `sqlite_database` owner の構築完了後だけ ownership を noexcept transfer し、allocation exception が backend
lifetime token より長く raw connection を残すことを禁止する。

SQLite に渡す database name は public request bytes そのものではなく、`/cxxlens-rooted-vfs-v1/` synthetic name と exact relative path の連結だけとし、
named callback はこの prefix のない name を拒否する。leading `:` / `file:` reserved name は SQLite URI parsing より前かつ Store effect 前に拒否する。
`xDlOpen` / `xDlSym` / `xDlClose` / `xDlError` は dynamic-library load、symbol lookup、close、ambient diagnostic を委任せず、null / no-op /
bounded empty diagnostic に閉じる。`xSetSystemCall` / `xGetSystemCall` / `xNextSystemCall` も replacement と discovery を公開せず、NOTFOUND または
null に閉じる。

named `SQLITE_OPEN_DELETEONCLOSE` は file effect 前に拒否し、DELETEONCLOSE を必要とする closed transient role は anonymous pathname-unreachable
private memfd だけで処理する。opener は request path state、receipt、synthetic name を SDK / SQLite effect 前に割り当て、VFS の main database open は
pending reservation、file create、noexcept active commit の順とする。失敗した reservation は cancel し、pending entry は sidecar authority を与えない。
`xOpen` は入口で
`sqlite3_file.pMethods = nullptr` と output flags zero を公開し、全 fallible work と construction が成功した最後にだけ method table を設定する。全 C ABI
callback は C++ exception を外へ漏らさず、C++ construction と owned resource に ownership leak を残さず、callback-specific typed SQLite result に閉じる。
partial handle state は後続 `xClose` / `xShmUnmap` が所有して解放する場合を許す。
既に認証済みの filesystem syscall effect は transactional rollback を一律には主張せず、`SQLITE_FCNTL_HAS_MOVED` の allocation または再認証 failure は
`SQLITE_OK` と moved one に保守的に閉じる。failure output は `xOpen` の pMethods null / flags zero、`xAccess` の exists zero、`xFullPathname` の
first byte NUL、`xShmMap` / `xFetch` の pointer null、`SQLITE_FCNTL_HAS_MOVED` の moved one だけを fallible work より前に初期化し、他の error output は
unspecified とする。

active main database と known sidecar の認証には、check から filesystem effect 完了まで last-close revocation と直列化する authority lease を必須とする。
最後の close は新規 lease を原子的に拒否し、既に取得済みの lease は effect 完了後に drain する。named open と `xAccess` は nonblocking open と
regular-file verdict を必須とし、FIFO、device、directory を database/sidecar として扱わない。`xDelete` は captured root 内の authenticated known path を
authority とし、path unlink の直前に current leaf の regular-file observation を要求する。quiescent な non-regular leaf は除去しないが、concurrent leaf
rebind に対する exact-object deletion guarantee は主張しない。
named path の parent directory は captured root dirfd 自身の duplicate、または captured root dirfd から `openat2` beneath で取得時に認証し、leaf effect はその retained authenticated parent-directory
capability 相対に限定する。parent capability 取得後の concurrent ancestor rename / mount relocation はその capability を revoke せず、effect 時点にも当該
directory が current namespace の captured-root path 下から到達可能であることは主張しない。取得時に beneath でない parent、または identity が一致しない leaf
への authority は与えない。
開かれた named journal / WAL handle は base path を capability として固定せず、`xRead`、`xWrite`、`xTruncate`、`xSync`、`xFileSize`、`xLock`、
`xUnlock`、`xCheckReservedLock`、`xFileControl`、`xShmLock` の前に active base-database lease を再取得する。`xShmMap` / `xShmUnmap` は main database
only とし、`xSectorSize`、`xDeviceCharacteristics`、`xShmBarrier`、`xFetch`、`xUnfetch` は effect を持たない advisory callback とする。最後の
main-database close 後は既存 sidecar FD の effectful callback を拒否し、`xClose` だけは無条件に owned resource を解放する。
`SQLITE_FCNTL_HAS_MOVED` は opened main-database FD identity と captured root から再解決した current path identity を比較し、missing、rename、replacement、
再認証 failure を moved と報告する。

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
task.v3 の `source.content_base64` は sealed source bytes から導出する unique canonical RFC 4648 spelling とし、request decoded string、source
size/content digest/line index と独立 cross-bind する。raw JSON escape 差は task.v3 bytes/digest を変えず、noncanonical decoded spelling は task codec
でも task-input digest の生成または acceptance より前に拒否する。
v3 payload は shared `cxxlens-canonical-tuple-v1` の signed `int64` integer domain を使うため、request budget の各正整数は
`1..INT64_MAX` に限定する。public `execution_budget` の `uint64` surface を狭めるのではなく、この installed JSON v2 から shared codec へ
lossless に写像できない値を worker launch 前に `materialization.request-invalid` で拒否する。

#### Provider Protocol 1.1 task input transfer

exact task.v3 logical bytes と `task_input_digest` を一 frame の payload limit へ押し込めず、Provider Protocol `1.1` の required feature
`task-input-chunks-v1` で物理的に分割する。materializer は protocol minor 1 とこの feature を必須とし、minor 0 や inline `open_task` payload へ
fallback しない。minor 0 の exact five-frame host transcript は generic public compatibility profile として維持するが、installed materializer の
production execution authority ではない。

minor 1 の host sequence は exact
`hello_ack, schema_negotiate, open_task, input_descriptor, input_chunk*, credit, close` とする。`open_task` payload は empty、
`input_descriptor` control は exact `{task_id,input_digest,total_bytes,chunk_bytes,chunk_count}`、各 `input_chunk` control は exact
`{task_id,input_digest,chunk_index,offset,byte_count}` とし、payload だけが logical task.v3 の連続 slice を持つ。chunk index と offset は 0 から
contiguous、non-final payload は descriptor の `chunk_bytes` と exact 一致、final payload は positive remainder、empty logical input は
`total_bytes == chunk_count == 0` と empty SHA-256 を要求する。全 chunk の ordered concatenation の byte count と streaming SHA-256 が
`open_task.task_input_digest` と一致し、shared seal と task.v3 decode/bottom-up binding が完了するまで `task_accepted` へ進まない。

canonical chunk size は 1 MiB (`1048576` bytes)、logical task input は最大 64 MiB (`67108864` bytes)、chunk count は最大 64 とする。
既存の一 frame 16 MiB limit は変更しない。missing、duplicate、reordered、overlap、extra、short、truncated、descriptor/chunk/payload/final digest
mismatch と limit 超過は task acceptance 前に拒否する。host encoder、worker decoder、process runtime、conformance validator は transition、digest、
length、budget を所有する一つの incremental state machine を共有する。raw host frames と input spool は diagnostic occurrence に限り、ambient path、
FD、environment、shared memory side channel を task authority にしない。既存 minor-0 public vector API はこの incremental core 上の bounded wrapper とし、
production path で full logical input vector を再 materialize しない。

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
producer identity は executable/interface/distribution/source revision/tree だけへ投影し、installed binary、configuration、measured occurrence は含めない。
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

#### Sealed transcript receipt, coverage planes, and closed guarantee

success transcript は一 task ごとに exact four coverage records を持つ。generic transport plane は
`{kind: task, id: <exact provider task ID>, state: covered, reason: ""}` の一件、Clang 22 semantic plane は canonical order の
`cc.call-extraction`、`cc.entity`、`frontend.clang22.observation` 各一件で、いずれも同じ exact task ID、`covered`、empty reason とする。
generic shared validator は specialization-blind のまま transport task record を要求して全 record を losslessly retain し、specialization seal が
semantic 三件を追加検証する。missing、duplicate、extra、renamed、wrong-task、non-covered、nonempty covered reason を拒否する。

report は二 plane を別々の recomputable record set として保持する。per-task transport count は 1、semantic count は 3、global transport count は
exact task count、global semantic count は exact `3 * task_count` とする。semantic guarantee の coverage census は三 semantic records だけを数えるが、
transport record を破棄したり semantic plane へ代入したりしない。task side-channel/result と global summary は両 plane の digest を明示的に bind する。

各 task の runtime-private receipt は、decode/move 前に観測した exact provider stdout frame stream の
`raw_frame_stream_bytes` / `raw_frame_stream_digest`、decoded frame count、domain `cxxlens.provider-frame-transcript.v2` の
`frame_transcript_digest`、domain `cxxlens.provider-sealed-transcript.v1` の `sealed_transcript_digest` を持つ。後二者は immutable seal を構築する
同じ shared validation pass から導出する。public `process_execution_report::semantic_digest()` はこれらの alias、raw/frame/sealed authority、または
materialization adoption authority ではない。raw stream/frame は diagnostic occurrence であり、sealed decoded value から row/claim を構築する。

claim/report guarantee は closed profile `cxxlens.clang22-materialization-guarantee-profile.v1` が所有する。profile digest は同名 domain で exact
profile fields から導出し、assumptions は exact `[]`、verification modalities は registered-symbol grammar を通る canonical order の exact
`[clang22.materialization-sealed.v1, provider.transcript-sealed.v1, sdk.claim-envelope-validated.v1]` とする。caller/report builder による追加、削除、
reorder と `future-modality` のような未知値を拒否する。`successful-publication`、`query-parity`、`store-reopen` は claim guarantee の
prepublication modality ではなく、publication 後の occurrence evidence/report receipt にだけ置く。exact guarantee はこの closed profile、両 coverage
plane、zero non-exact census、blocking unresolved zero、exact task/six-batch/full-span/provenance seal がすべて成立する場合だけ生成する。

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
task は transport coverage、semantic coverage、unresolved、evidence、closed-profile guarantee fragment の component digest を持ち、`side_channel_digest` を
`cxxlens.clang22-task-side-channels.v1`、`task_result_digest` を `cxxlens.clang22-task-result.v1` で導出する。後者は
execution/selected catalog/final relation ID、terminal、input-transfer receipt、raw/frame/sealed transcript receipt、canonical/observation group digest、
side-channel digest を bind する。

adoption の `task_result_set_digest` は `cxxlens.clang22-task-result-set.v1` domain で task execution key 順の task result を、raw-frame
set は `cxxlens.clang22-raw-frame-set.v1` domain で同じ physical task 順の frame count/digest を bind する。global transport coverage、semantic
coverage、unresolved、evidence はそれぞれ `cxxlens.clang22-global-transport-coverage.v1`、`cxxlens.clang22-global-coverage.v1`、
`cxxlens.clang22-global-unresolved.v1`、`cxxlens.clang22-global-evidence.v1` domain で、
自身の digest を除く typed summary fields と semantic-task-key-sorted component leaves から再計算する。semantic task key は exact
`[provider_task_id, task_input_digest, selected_catalog_compile_unit_id, compile_unit_id]` とし、`provider_execution_id` を除外する。
evidence kinds は exact sorted key set、
kind counts の和は total count とする。guarantee digest は `cxxlens.clang22-materialization-guarantee.v1` domain で closed profile ID/digest、typed
fields、四 global side-channel digest、同じ semantic-task-key-sorted guarantee fragments、exact order の observation descriptor censuses を bind する。このため
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
report 外で `report_digest` に hash する。各 invocation は companion
`cxxlens-clang22-materialization-execution-receipt.json` に actual exit status、exact stdout byte count、stdout SHA-256、parsed response count、
stderr SHA-256 を保持する。qualified evidence は exit 0、exactly one response、stdout bytes と report artifact の byte-for-byte 一致を要求する。
configuration ごとの `report_set_digest` は configuration と exact backend order `[memory, sqlite]` の
`{backend, report_digest, execution_receipt_digest}` から canonical に導出する。これらの digest は materialization report 自身の field ではない。

### Internal sealed adoption boundary

shared typed transcript validator は accepted batch の decoded rows、descriptor/task/group binding、ordered batch digest、coverage、unresolved、
evidence、guarantee、provenance、terminal を tool-private の immutable `sealed-materialization-result` に value-own する。この value は全 batch と
両 mandatory group が seal され、span/claim/hard-reference/coverage validation が完了した後だけ生成できる。

raw frame、stdout bytes、diagnostic JSON、成功 terminal 文字列は diagnostic/evidence であり、row reconstruction、claim construction、Store adoption
の authority ではない。downstream code が frame を再 decode して sealed result を作り直すこと、private codec を複製すること、diagnostic prose
substring で control flow を決めることを禁止する。sealed value は tool 内部だけで消費し、public C++ type として export しない。

### Proposed DF-0200 move-only claim and Store amendment

DF-0200 の SQLite Option A は Issue #200 の fresh user decision として選択済みだが、本 amendment と ADR 0097 は
distinct independent review 中である。review が exact binding を受理するまでは Issue #181 の blocked claims-to-Store
implementation を開始しない。受理後も実装と四象限 qualification を先取りして completed と扱ってはならない。
Snapshot Store checker は self-contained な hardcoded ingress object だけを検証し、materialization checker が downstream から exact resolution ID と
codec/completeness/counter/SQLite-decision binding を照合する。generic Store checker から materialization contract への reverse dependency は持たない。

- **D1:** public sdk::claim_batch::commit の resident control flow は production incremental validator と独立な bounded qualification reference とする。
  共有可能なのは canonical codec、identity、field validator だけである。pre-refactor canonical bytes/verdict corpus と current public API に対し、
  added/existing、new-existing refs/conflicts/differentials、existing-existing non-reclassification、one-shot/split、permutation、duplicate、
  metadata-distinct の exact differential を要求する。固定 TSV は10 case / success 9 / error 1、raw SHA-256
  `f05513d05b0b57788b6f94d9c1a477c88d589b64dd8232d88a5c6c6022a84836` とし、専用 C++ CTest が全 case で current public API の
  full input/success-or-error/output/verdict bytes を照合する。production path 比較は accepted activation に残す。
- **D2/codec:** one-task move-only consume を維持し、cxxlens.df-0200.partition-event-stream.v1 の full-byte streamを使う。big-endian の exact header、
  kind/key-length/payload-length/key/payload/checksum frame、exact trailer、7 event projectionを固定し、unknown/missing/reordered/truncated/trailing bytes を拒否する。
  canonical tuple tag/type/cardinality、collection order/dedup、全 field order、および stream/frame/trailer/task/partition/global と各 typed record の
  dedicated digest domainを固定し、各 aggregate は u64 count と length-framed full canonical projectionを bindする。claim input の exact duplicate は
  独立 claim law で collapse、metadata-distinct occurrence は preserveし、final full-event duplicateだけを拒否する。
- **D3:** validated request と sealed execution journal/task receipts を external completeness authority とし、stream header は cross-check のみとする。
  task/partition/event/claim/row/coverage/unresolved census+digest、segment/run/merge manifest、byte/record/seal receiptを Store が独立比較し、whole-partition dropを
  拒否する。selected request は provider dispatch前に canonical taskごと exactly one entry bindingを sealする。独立 pre-encoder oracle は immutable task
  resultから full event projectionを列挙し、task receiptは自身のtask ID/ordinal、successful-seal、Provider Runtime raw/frame/sealed receiptへbindする。
  task receiptはfinal journalを参照せず、全 task seal後の execution-journal receipt-setだけがordered task sealsをbindする非循環DAGとする。task resultは
  receipt sealとstream sealの両方が完了してから破棄する。stream+end/trailerの相関 omissionはfixed receipt、receipt/task seal/final setまで編集する omissionは
  immutable final setまたはselected-entry cross-checkで拒否する。semantic-version component は u32、v5 collection count は u64、1M/10M decoder cap は bounded streaming で除去する legacy guard とする。
  u64超は partition_stage / store.counter-overflow / materialization-v5-collection-count / empty detail から store-stage /
  materialization.store-failure、draft discardedへ写像する。
- **D4:** recordはkind/lengths/key/payload/checksum全体で、segment/spool splitを禁止する。preappend u128 check、half-open canonical offsets、segment then
  next-spool rollover、u128 aggregate census、8 MiB超 singleton run、二 comparator cursor合計64 KiB、16+1+1=18 FDを固定する。known limitはI/O前、
  private spool ENOSPCはproved in-range後だけとする。
- **D5:** spool-failureはprivate prepublication spoolだけである。2026-07-22 の accepted activation はphase matrix、reverse closure、partition_stage overflow tuple、
  report-schema digestをatomicに更新した。request 2.1.0 shape は不変で、report 2.1.0 は private spool-failure、13/19-file occurrence inventory、
  sandbox array boundだけを追加する。SQLite writer_publish ENOSPC/SQLITE_TOOBIGは既存の store.sqlite-failure/database/opaque と
  publication_outcome_unknown、返却済handle後の検証failureはsafeなら committed_unverified を保つ。exit 2 はresponse-unsafe failureまたは
  successful receipt/arithmetic contradictionだけである。
- **D6/accepted Option A; report-schema activation applied, qualification pending:** report schema digestは sha256:f321e25f72bf8c6312dfe1e36fe6b6573239db697c2cfabd60e2c0546f9ee98b。
  SQLite v2 single BLOB/MAX_LENGTH 1,000,000,000 はlimit-adjacent passed parityを満たさないため、ADR 0097 の
  `cxxlens.sqlite-semantic-store.v3` / 3.0.0、8 MiB chunk、16 MiB runtime floor、exact v2.6 read-only dual-read、
  `compact()` deterministic COW migration を採用する。v2 `begin()` は exact
  `store.migration-required / sqlite-physical-format / cxxlens.sqlite-semantic-store.v2-to-v3` で no-effect rejection とする。
  request 2.1 shape、logical v5、physical-generation 以外の payload bytes、semantic/publication/snapshot ID、public signature、
  accepted request set、cursor lifetime は不変である。`store.migration-required` は additive Store result state であり、B と parity weakening は rejected とする。

falsification は independent frozen/public oracle、codec/census/whole-partition-drop、framing/rollover/u64/u128、exact Store tuple、spool faults、
publication_outcome_unknown/committed_unverified、4096-task、backend parity、v3 chunk corruption、v2 zero-mutation read、migration fault/crash matrix、
single-BLOB limit 超の cold reopen を含む。全 evidence が揃うまで既存実装を production authorityとしない。

### One transactional publication

tool は request の全 compile unit、両 dependency group、base/span/canonical claim、coverage/unresolved/evidence/guarantee/provenance を一つの
snapshot draft に stage する。一 request の target backend は memory または SQLite の exactly one とする。全 hard reference と independent
validator が通った後だけ、expected-parent CAS を伴う一回の snapshot transaction で publication する。worker/task/group/span/claim/coverage failure と、
commit 前と証明できる stale/counter/identity/hash/corrupt-authority failure は draft 全体を破棄する。SQLite の phase-opaque
`store.sqlite-failure` は close/reopen 後の candidate absence も commit phase を証明しないため、candidate の absence/presence や Store open failure にかかわらず
invocation outcome unknown とする。candidate absence は recovery evidence ではあるが zero-commit proof ではない。
`publish()` が handle を返した後の current/open/cursor/export/report transport failure は commit を rollback できず、typed response を書ける場合は
`committed_unverified`、書けない場合は exit 2 とする。いずれも prior head unchanged を捏造せず、release evidence にしない。

同じ semantic request を各 backend に与えた matched run は memory と SQLite で同じ snapshot/partition/query digest を生成しなければならない。
SQLite evidence は close/reopen 後の再検証を含む。一方の backend、static/shared configuration、compile unit、mandatory group だけが成功した
report を production evidence として採用しない。

#### Registry, engine, and Store identity projection

v2 は次の三つの Registry identity を別の field として保持し、相互に代用しない。

1. `authority_digests` 中の Relation Registry file digest は raw authority artifact bytes の `sha256` である。
2. `registry.authority_registry_digest` は accepted Registry 全 21 relation の semantic projection を canonical JSON にした bytes の `sha256` である。
3. `engine.engine_registry_digest` は、この tool が実際に admit する base 6 + worker output 6 descriptor の runtime inventory digest である。

engine inventory は各 accepted descriptor の canonical `descriptor_id` と `runtime_descriptor_digest` を UTF-8 byte order で sort し、各 row を
`descriptor_id + "=" + runtime_descriptor_digest + "\n"` として連結する。その payload に
`semantic_digest("cxxlens.relation-registry.v1", payload)` を適用する。relation name を descriptor ID の代わりに使うこと、duplicate canonical
descriptor ID、missing/extra/reordered binding、full authority digest の代入を拒否する。`relation_registry::build()` も ADR 0017 と同じ projection を使う。

engine generation と named policy は次の exact projection から再計算する。

```text
engine_generation_id = ID("engine-generation", [
  "cxxlens.clang22-materialization-engine.v2",
  worker.provider_id,
  worker.provider_version,
  worker.semantic_contract_digest,
  engine_registry_digest
])

interpretation_policy_digest = semantic_digest(
  "cxxlens.clang22-interpretation-policy.v1",
  tuple(["cxxlens.clang22-interpretation-policy.v1", "cc.clang22-canonical-1"]))

trust_policy_digest = semantic_digest(
  "cxxlens.clang22-installed-native-worker-trust.v1",
  tuple([policy ID, execution profile, provider ID/version/semantic contract,
         protocol major/minor, exact required features [task-input-chunks-v1],
         required qualification, worker sandbox digest,
         sorted unique task {minimum, policy digest} tuples]))
```

request は `snapshot_series_selector` の exact seven fields
`[catalog_id, channel_id, engine_generation_id, condition_universe_id, relation_registry_digest,
interpretation_policy_digest, trust_policy_digest]` を持ち、`series_id` を SDK の `snapshot_series_selector::id()` と同じ
`ID("snapshot-series", fields)` で再計算する。channel は caller-selected explicit dimension で default を持たない。全 task は selector と同じ
condition universe と named interpretation domain を使用する。selector の `relation_registry_digest` は engine digest であり authority Registry digest
ではない。

semantic request v2 は registry/engine/policy/complete selector、`genesis`、`partial_policy`、`transaction_count`、`reopen_before_success` を含む。
materialization request identity fields、derived `series_id`、backend、SQLite path、expected-parent CAS token は除外する。従って fresh genesis の
memory/SQLite は同じ semantic request を持ち、SQLite append は genesis request と同じ digest であると主張しない。

memory backend は process-local fresh Store に対する `genesis: true`、null expected parent、sequence 1 だけを許可する。SQLite genesis は exact selector
の head absence と fresh transaction を確認し sequence 1 とする。SQLite append は exact selector の既存 committed/non-corrupt head を開き、request の
expected parent と一致させ、sequence を parent + 1 とする。wrong/missing/corrupt/cross-series parent は commit せず失敗する。static/shared ×
memory/SQLite release matrix は fresh genesis を使い、四 run の selector、series、partition set、snapshot ID、sequence、parent、publication ID を同一にする。

#### Claim-to-partition and publication evidence

tool は physical installation から独立した materializer semantics、worker semantics、project/catalog、exact engine inventory、semantic task context と
task input digest の sorted census から
`semantic_digest("cxxlens.clang22-direct-materialization-basis.v1", canonical tuple)` を一つ導出する。configuration、installed binary/prefix、backend/path、
series/parent/publication、timestamp、task arrival order はこの direct basis から除外する。observation v2 row は worker producer とこの direct basis で
`make_assertion()` する。canonical worker row は同じ basis の hidden worker assertion を validation/provenance input とし、materializer producer と
`cxxlens.clang22-canonical-adoption-transform.v1` digest で `make_canonical_claim()` する。base row は materializer-owned hidden assertion を同じ basis で
作り、`cxxlens.clang22-base-ingestion-transform.v1` digest で canonicalize する。hidden assertion は Store batch に commit しない。

両 transform digest は各 domain と同名 tag、materializer semantics digest、engine registry digest の canonical tuple に semantic-digest-v2 を適用する。
materializer semantics は tool executable/interface/distribution/source revision/tree と sorted authority `{path,digest}` tuples を
`cxxlens.clang22-materializer-semantics.v1` domain で bind し、physical installation fields を除外する。direct basis はその digest、worker の
provider ID/version/semantic contract/protocol、project ID/catalog ID/catalog digest、engine generation/inventory、semantic task context と task input digest の
sorted tuplesを同名 `cxxlens.clang22-direct-materialization-basis.v1` domain で bind する。

`make_canonical_claim()` は precursor claim content と transform semantics から `cxxlens.canonical-input-basis.v1` direct basis を claim ごとに再生成する。
従って final claim を common basis に強制しない。final claim occurrence は Store と同じ次の八項目で group する。

```text
[relation_descriptor_id, guarantee.scope, claim_condition.canonical_form(), interpretation,
 producer.semantic_contract, claim_input_basis_digest(input_basis),
 guarantee.approximation, guarantee.assumptions]
```

condition は originating task の request-level universe と exact singleton condition fragment、scope は `project_id`、assumptions は closed profile の
exact empty list に `"assumption-set:" + semantic_digest("cxxlens.clang22-assumption-set.v1", canonical tuple)` を適用した strong ID、verification modalities は
profile が固定する exact three registered symbols とする。同じ semantic content に複数の provenance occurrence があれば losslessly report し、Store claim set の unique content count と
evidence occurrence count を区別する。claim batch の conflict と differential disagreement は一件でも publication を拒否する。

mandatory descriptor/task/group が zero-row の場合も reopened snapshot が exact zero を証明できるよう coverage-only empty partition を stage する。
その basis は `cxxlens.clang22-empty-partition-basis.v1` domain で direct basis、descriptor ID、condition canonical form、interpretation、producer semantics、
stage transform semantics を bind して導出する。non-empty partition は claim の actual basis を使う。

qualified publication は provider coverage が全 task/group/descriptor で covered、blocking/nonblocking unresolved が共に zero、Store unresolved が zero の
exact path に限定する。各 non-empty partition は originating task の `materialization.task` と group の `materialization.dependency-group` coverage unit を
持ち、base partition は `materialization.base-descriptor` coverage unit を持つ。empty partition も対応する coverage を持つ。coverage canonical form を
sort/deduplicate し、closure certificate はこの direct materialization unitでは exact zero とする。

coverage key は delimiter text ではなく、task を `ID("materialization-task", semantic task context tuple)`、group を
`ID("materialization-dependency-group", [task key, dependency group ID])`、base descriptor を
`ID("materialization-base-descriptor", [task key, descriptor ID])` で導出する。各 Store unit は state `covered`、empty reason とし、SDK の
`snapshot_coverage_unit::canonical_form()` で identity/order を再計算する。

detailed response は detached row の exact canonical form と descriptor を
`ID("materialization-claim-row", [descriptor ID, exact row canonical UTF-8 bytes])` に bind する。row 内 descriptor との不一致、同じ ref の異 bytes、
orphan/unreferenced row を拒否する。各 claim は SDK と同じ semantic key、assertion、content、producer、direct/derived basis、provenance、guarantee を
self-contained envelope に保持し、その full occurrence を singleton `claim_batch_content_digest([claim], [], [], [])` で再計算する。report-private
`claim_ref` は `ID("materialization-claim-envelope", [hidden_precursor|stored_final, singleton batch digest])` とする。hidden precursor は cc/base の
assertionだけ、stored observation は assertion、stored cc/base は canonical claim とし、各 hidden precursor と stored canonical claim の間に exactly one
transform edgeを要求する。hidden claim は Store/final batch に入れない。

materialization origin は SDK occurrence identity に混ぜず、
`ID("materialization-claim-association", [stored claim ref, exact semantic task context, sealed/base row digest, source evidence digest or empty])` として別に保持する。
complete final claim batch は全 stored final occurrence を一回だけ含み、unresolved/conflict/differential disagreement を exact zero とする。partition は八
identity fields、stored full-occurrence refs、sorted unique content IDs、SDK occurrence count、origin association count、coverage/unresolved を別々に保持し、
content dedup と origin loss を同一 count で隠さない。

さらに Store の exact snapshot manifest と invocation publication record、reopened descriptor/partition/row/annotation/coverage/unresolved/closure/cursor/export projection
を保持する。`current(selector)`、`open_publication(candidate)`、`open(candidate snapshot)` の三 path は固定 lookup input、typed status、full returned
publication record と semantic projection digest を別々に記録する。前二 path は candidate の semantic publication fields と一致させるが、外部 compaction
による validated physical-generation increase は許す。`open(snapshot)` は別 series の SDK resolver record を返し得るため publication record の byte equality
を要求せず、requested snapshot ID、committed/non-corrupt record、三 path の semantic snapshot projection を照合する。public SDK にない record census を
tool が完全であると自己申告してはならない。opaque fixture ID、`paths_agree`、`snapshot_identity_recomputed` のような boolean だけを証拠として受理しない。

#### Operation-first Store outcome mapping

Store failure classifier の key は `{authenticated operation, backend, exact SDK code, exact SDK field, authority-declared stable detail}` とする。
diagnostic prose や SQLite message substring から phase、code、commit state を推測しない。`configuration`、`store_open`、`head_current`、
`writer_begin`、`partition_stage`、`closure_stage`、`writer_validate` の publication 前 failure は、最初の exact SDK tuple を保持した
phase-authentic compact failure / publication-not-attempted / zero-commit とする。
compact `store_draft_state` の対象は SDK writer object ではなく、一 invocation の logical unpublished materialization draft である。
この logical draft は Store open 成功時に adoption boundary を越える。以後の `head_current` を含む publication 前 failure では、
`snapshot_store::begin()` が未呼出または失敗して writer begin count が zero でも、compact authority を発行する前に invocation の
logical unpublished draft 全体を release して `discarded` とする。`not-created` は Store open 成功前だけであり、SDK writer receipt の
有無から draft state を推論してはならない。
`head_current` の SDK failure は access path `current-selector` を保持する。exact `store.current-not-found` は head observation `absent`、
それ以外の SDK error は `sdk-error` とし、いずれも `observed_head_publication` は null とする。`sdk-error` を absence に読み替えたり、
架空の publication record を生成したり、path を消去してはならない。他の publication 前 operation の compact cause は access path null とする。

SQLite `writer_publish` で detailed outcome を生成できる exact tuple は次だけである。`empty` は stable detail の empty string、`opaque` は
SDK が返した exact diagnostic bytes/count/digest を occurrence evidence として保持することを意味する。

| SDK code | Exact field/detail | Detailed outcome |
| --- | --- | --- |
| `store.publication-conflict` | `exact-series-id` / empty | `rejected_stale` |
| `store.counter-overflow` | `publication_sequence` または `physical_generation` / empty | `rejected_store_failure: counter_overflow` |
| `store.hash-collision` | `exact-candidate-snapshot-id` / empty | `rejected_store_failure: hash_collision` |
| `store.snapshot-ambiguous` | `exact-snapshot-id` / empty | `rejected_store_failure: persistence_corrupt` |
| `store.sqlite-failure` | `database` / opaque | `publication_outcome_unknown: persistence_io` |
| `store.corrupt` | `sqlite` / `backend`, `column-count`, `publication-row`, `series-head-count`, `series-head`, `series-head-sequence` | `rejected_store_failure: persistence_corrupt` |
| `store.corrupt` | `exact-publication-id` / `authority-record`, `duplicate-publication-id`, `parent`, `parent-sequence` | `rejected_store_failure: persistence_corrupt` |
| `store.corrupt` | `exact-series-id` / `duplicate-sequence`, `series-roots`, `series-head-cas` | `rejected_store_failure: persistence_corrupt` |

`publish()` が handle を返した時点で、その exact returned publication record が commit proof である。その後の `store_reopen`、
`verify_current`、`verify_open_publication`、`verify_open_snapshot`、`verify_projection` の SDK error または expected/actual projection mismatch は、
最初の cause と全 successful projection を保持して `committed_unverified` とし、successful SDK call の mismatch に架空の `store.*` code を付けない。
projection mismatch の closed stage set は publication/snapshot binding、descriptor/partition/row/annotation/coverage/unresolved/closure/cursor/export、
および三 path 間の semantic projection equality とし、各 retained `closure_digest` の mismatch も exact path と expected/actual digest を保持する。
expected digest は report の Store binding から再構築した exact handle projection、actual digest は exact access path の retained successful handle projection
から再計算する。cause の二 digest がこれらと一致しない、または retained projection を rebind しただけの値は拒否する。三 path equality は固定 path 順で
expected semantic projection と最初に異なる retained semantic projection digest を actual とする。
recovery observation で元の invocation outcome を再分類しない。

`store.transaction-state/publish/""`、`store.corrupt/publication/identity`、`store.publish-stale-parent`、memory backend の任意 publish error、および
上表にない SQLite `writer_publish` tuple は contract/implementation invariant breach として exit 2 / stdout zero authoritative response とする。
これらを schema-valid rejected outcome へ丸めず、memory の precommit error や unlisted tupleから zero commit を推測しない。

### Typed response, errors, and release binding

report v2 schema version `2.1.0` は top-level `response_kind` を discriminator とする closed union である。両 branch は schema/version、failed/passed result、generated
time、process exit status、bounded `raw_input_observation` を持つ。

- `detailed` branch は full request binding 後に sealed materialization DAG が完成した場合だけ生成する。passed response は exact Store commit と三 path の
  reopened verification を持つ。failed detailed response も同じ leaf-to-partition/snapshot validation を先に通し、その後の outcome を disjoint な
  `rejected_stale`、`rejected_store_failure`、`publication_outcome_unknown`、`committed_unverified` のいずれかに閉じる。前二者だけが invocation zero commit を
  主張できる。phase-opaque SQLite I/O error は candidate absence を含む全 recovery visibility で `publication_outcome_unknown` とする。publish が full record を
  返した後の path/error/projection mismatch は committed attribution を維持したまま `committed_unverified` とし、成功した path projection と最初の typed
  SDK error または expected/actual digest mismatch を保持する。Store call が成功した mismatch に架空の `store.*` error を付けない。
- `compact_failure` branch は incomplete pipeline の phase-authentic response である。strict decode/request validation 前なら raw-input-only、full request
  validation 後なら exact request-bound とする。failure phase/code、typed diagnostic、worker launch count、Store draft state、head observation
  (`not-observed`/`absent`/`present`/`sdk-error`)、publication attempted、committed transaction count、prior-head preservationを持ち、detailed execution section を
  混在させない。input-limit、JSON decode、request schema/version/binding、installation binding、worker launch/transcript、materialization validation、
  Store open/stage、report constructionの last authenticated boundary より後の identity/evidence を主張しない。

detailed report は exact request/tool/worker/provider/task/catalog/descriptor/group/input/toolchain/environment/expected-parent identity、各 task/group verdict、
publication identity、memory と reopened SQLite の parity、coverage、unresolved、evidence、guarantee、provenance、structured diagnostics と全 retained
child digest を保持する。observation equivalence census とその production-exact verdict、および leaf から
global provenance/base-guarantee/Store publication cross-binding まで再計算可能な in-report DAG を必須とするが、top-level semantic/report digest は
保持しない。schema-valid な任意 digest を semantic evidence として受理しない。少なくとも次の stable error family を区別する。

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
exact version `2.1.0` の materialization report を各 combination exactly one、合計四件要求する。各 configuration の production tuple evidence digest は、その
configuration の memory/SQLite report 二件の external canonical report-set digest と installed worker/tool bytes、request artifact/
external report artifact/contract digest を bind する。
各 run は実 process の exit status、stdout exact byte count と `sha256`、parsed response count、stderr bytes の `sha256` を harness receipt に保持する。
stdout bytes と report artifact bytes は byte-exact に同一でなければならず、passed は exit 0/response one、schema-valid failure は exit 1/response one、
exit 2 は response zero とする。fixture を直接 validator に渡しただけの結果や stderr/exit から report outcome を合成した値は execution evidence ではない。
report がない、重複する、configuration/backend/revision/worker/tool/task/store digest が一致しない、memory と close/reopen SQLite の semantic
snapshot/canonical-export/query digest が一致しない、三 descriptor census のいずれかで
`non_exact_equivalence_count != 0`、blocking unresolved、または
full-span precondition failure がある、digest DAG を bottom-up に
再計算できない、または transaction が publication まで完了していない場合は `not-qualified` とし、GR production tuple を生成しない。

各 run は measured occurrence manifest、external full-prefix witness、strict argc、rooted SQLite VFS、Protocol 1.1 input-transfer receipt、
raw/frame/sealed runtime receipt、transport/semantic coverage 二 plane、closed guarantee profile を exact bind する。request-only occurrence assertion、
public process semantic digest、raw frame、postpublication query/reopen receipt をその代用にしない。scale evidence は one-task、4096-task、16 MiB
source、512 MiB aggregate source、1 GiB raw request、arbitrary short read を含み、retained memory が one shared catalog + fixed buffers + one
task/index/source/output window の式に従うことを測定する。aggregate occurrence retention、spool failure 後の effect、minor-0 fallback は production
evidence として拒否する。

authority-only rollout 中は、production-scope の materializer assignment 自身が exact
`scope.clang22-installed-adoption-gap` / `#181` / `[DF-0182, DF-0187, DF-0191, DF-0192, DF-0195, DF-0196, DF-0197]` の `tracked-gap` である場合に限り、normal release evaluation が request/report/set の
exact zero state を typed `tracked-gap-empty` として受理する。この状態では materializer executable 一件だけを installed-file census から除外し、
schema、worker、他の required file は除外しない。zero state に一件でも request/report が混じる、または exact four-pair evidence が先行する場合も
assignment と evidence が一致しないため拒否する。

Issue #181 が同 assignment を `included` / `qualified` として完了させた後は、他の assignment に tracked gap が残り全体の scope closure が
`classified-with-gaps` であっても、この例外を解除する。materializer executable と exact four co-located request/report pairs、two report-set
digests を必須とし、それらがない evaluation は `not-qualified` とする。owner、feedback、scope、qualification がこの二状態のどちらにも exact に
一致しない中間状態は fail closed に拒否する。

元の installed boundary と observation v2 authority は Issue #182 の authority-only unit である。Issue #181 の preflight で見つかった
DF-0191/DF-0192 に対する machine v2、SDK registry conformance、Store projection と、DF-0195/DF-0196/DF-0197 に対する sealed report leaf、
installed occurrence/effect、Protocol 1.1 chunked task transfer の release/checker amendment は Issue #181 の独立した authority-first checkpoint とする。
独立反証 review が exact binding を受理して該当 DF を accepted / may-proceed にするまでは blocked scope の runtime/tool implementation を再開しない。受理後、runtime と
四件の installed detailed report evidence を同じ Issue #181 が完成させる。それまでは production scope の tracked gap と Clang 22
production-support prohibition を維持する。

## Rejected alternatives

- **Clang-22-specific public C++ host bridge**: provider 固有 task と publication を stable SDK に露出し、option 2 の installed machine boundary を崩す。
- **general public task-builder/adoption API**: 再利用可能だが、証明済みの一つの installed vertical より広い public semantics を先に固定する。
- **raw frame/test-only reconstruction**: shared validator と registry/store adoption を迂回し、diagnostic bytes を authority にする。
- **arbitrary transcript/guarantee report leaves**: executed shared seal から再計算できない fixture value や postpublication modality を claim authority にする。
- **minor-0 inline payload fallback または request limit 縮小**: task.v3 identity と accepted source bounds の一方を暗黙に弱める。
- **request/PATH/argv0-only installation binding**: actual executable、sibling worker、authority bytes と Store effect root を実行境界で認証できない。
- **worker が `source.span` を第七 output として送る**: source ingestion ownership と exact six descriptor contract を崩し、base/canonical responsibility を混在させる。
- **task/group/backend ごとの partial publish**: project catalog census、hard reference closure、memory/SQLite parity、one-snapshot evidence を失う。

## Compatibility and rollback

- public C++ signature、symbol、ABI、callable inventory は変更しない。catalog-local identity の Doxygen/contract clarification だけを行い、既存
  portable/provider-runtime/native SDK は引き続き protocol/conformance surface とする。
- descriptor name を使った旧 engine registry digest の SQLite series は、その digest を実装した compatible legacy binary/engine では current/read が可能な
  既存 history として維持するが、corrected descriptor-ID digest の engine から append/migrate しない。両 digest は別 selector/series authority であり、
  alias、silent rehash、cross-digest parent adoption を禁止する。legacy history の新規 write は停止し、明示 migration authority がない限り read-only とする。
- observation v2 は v1 と semantic-compatible と仮定しない。v1 transcript/report は v2 adoption input にならず、silent upgrade path を持たない。
- JSON contract v1 の意味変更は patch で行わない。required field、identity、group、publication、error semantics の変更は明示的な新 contract/version とする。
- materialization machine v1 は implementation/qualification 前に v2 で supersede する。v1 request/report を v2 として読み替えず、v2 response を v1
  release evidence に downgrade しない。
- v2 schema の exact version は 2.1.0 とし、prior minor request/report/contract を暗黙に受理しない。Provider Protocol minor 0 の public
  five-frame vector compatibility は維持するが、materializer は minor 1 + `task-input-chunks-v1` だけを使用し fallback しない。
- DF-0199 の canonical Base64 rule は protected `main` 未 merge、production materializer 未実装、qualification 未実施の active v2.1 unit 内の
  correction として version `2.1.0` を維持する。canonical または qualified な外部 2.1 producer/consumer が見つかった場合はこの判断を reject し、
  successor version と明示 migration boundary を要求する。
- implementation rollout 前は tool/report を production authority として advertise しない。rollout 後に障害があれば new materialization を停止し、unpublished
  draft/report を破棄して prior snapshot head と既存 public SDK を維持する。published history の rewrite や v1 report の reinterpret は rollback に使わない。

## Consequences

- Clang 固有の installed task authority と publication path を一つの versioned machine surface に限定できる。
- shared transcript validation の結果を losslessly retain しながら、raw frame と public API の authority inflation を防げる。
- catalog input identity と final relation identity を acyclic に分離し、暗号学的 fixed point や opaque placeholder を要求せず base claim を構築できる。
- source span の復元不能な hidden assumption を observation v2 full bundle と host-owned materialization で解消する。
- protocol 1.1 chunk transfer により task.v3 identity を変えず 16 MiB frame ceiling 内で 64 MiB logical input を認証できる。
- two-plane coverage、runtime-private receipts、closed guarantee profile により shared seal と report leaf の間の欠落・fixture placeholderを除去できる。
- measured occurrence、rooted SQLite VFS、operation-first Store mapping により installed process と effect/outcome の attribution を閉じられる。
- whole-project one-transaction と memory/reopened-SQLite parity が static/shared release evidenceへ直接結び付く。
- Issue #181 は DF-0191/DF-0192 と DF-0195〜DF-0197 の independently reviewed v2.1 machine/Store/provider authority と SDK registry projectionを変更せず、tool、runtime/store
  integration と四件の release evidence を同一 contract に実装する必要がある。

## Verification

contract/checker は exact JSON version 2.1.0、strict argc、two-pass spool、bounded raw-input response、phase-authentic failure union、catalog census、12-descriptor engine admission、complete
seven-field selector、claim/partition/snapshot/publication identity、six worker descriptor/group binding、full span bundle、raw-frame non-authority、sealed
result completeness、Provider Protocol 1.1 chunk transfer、two-plane coverage/runtime receipts/closed guarantee、measured occurrence/rooted VFS、operation-first
Store outcomes、one expected-parent transaction、memory/reopened-SQLite parity、typed response/errors、static/shared matrix を positive/negative fixture
で検証する。
negative fixture は missing/extra task/group/descriptor、v1 observation、partial span bundle、source bundle のない canonical row、span identity
mismatch、unsealed batch、dropped
coverage/unresolved/evidence/guarantee/provenance、raw-frame reconstruction、stale parent、partial publication、backend/configuration/
external-report-artifact digest mismatch を
個別に拒否する。さらに catalog-local/final relation ID の implicit alias、entry digest mapping drift、catalog entry/task census drift、同じ generic task ID を
共有する二つの valid execution の誤 deduplicate、同一 task/input/execution tuple duplicate をそれぞれ反証する。
raw input count/digest/complete drift、limit+1 以外の oversize prefix、raw-only failure への request binding 捏造、request-bound failure の binding drift、
phase/code/effect mismatch、`not-observed` head を absent とする主張、compact response への detailed section 混入、incomplete detailed failure、failed response
の release evidence 採用も拒否する。Registry file/authority/engine digest の alias、descriptor ID duplicate/name-based inventory、engine generation/policy/
selector component mutation、mixed task universe、old series ID、non-genesis memory、cross-series SQLite parent を個別に拒否する。partition の八 identity field、
claim/basis/coverage/unresolved、empty-basis、manifest、sequence/parent/publication record の一項だけを old ID のまま変える case と reopened Store drift を
各 identity DAG layer で拒否する。
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

CLI/occurrence negative は unexpected argv が stdin/worker/Store effect 前に exit 2 / stdout zero となること、request-only revision/tree/configuration/
tool/worker/authority spoof、wrong executable object、deleted/non-regular self、build-tree/PATH shadow、occurrence-manifest self entry、external full-prefix
witness drift、relocation後の sibling replacement、symlink/magic-link/escape、hash 中の FD replacement を拒否することを検査する。SQLite effect negative は
absolute/dot/dotdot/empty/NUL/backslash/normalization path と、database/WAL/SHM/journal の parent/leaf symlink escape、unsupported rooted VFS を effect 前に
拒否する。

Protocol negative は missing/duplicate/reordered/overlap/extra/short/truncated input descriptor/chunk、task/input mismatch、payload/final digest drift、
1 MiB/64 MiB/64 の各 limit adjacent、minor 0 fallback、task acceptance before seal、ambient input side channel を拒否する。request tests は arbitrary short
reads、duplicate member at arbitrary depth、one-task/4096-task、16 MiB source、512 MiB aggregate、1 GiB raw input、spool failure と retained-memory formulaを
検証し、raw request/DOM/task payload/catalog の aggregate resident copies を許さない。

report-leaf negative は transport task record と semantic exact-three の missing/duplicate/extra/rename/wrong-task/state/reason drift、二 plane の substitution、
raw frame byte/count、frame transcript、sealed transcript、input-transfer receipt の一 leafだけの mutation、public process semantic digest alias を拒否する。
closed profile では assumptions nonempty、modality missing/reorder/extra、`future-modality`、registered-symbol grammar violation、`successful-publication`、
`query-parity`、`store-reopen` の claim modality 混入を拒否する。

Store outcome negative は全 prepublication operation と exact SQLite writer-publish tuple、publish-returned handle、postpublication path/mismatch を operation-first
に列挙し、opaque diagnostic prose からの code/phase inference、successful call mismatch への架空 error、recovery observation による outcome reclassificationを
拒否する。`store.transaction-state/publish/""`、`store.corrupt/publication/identity`、`store.publish-stale-parent`、memory publish error、unlisted SQLite
tuple が schema-valid zero-commit report にならず exit 2 / no response になることを検査する。

authority-only PR は独立反証 review 中だけ design feedback を `proposed/blocked` に保ち、machine/relation/release/acceptance authority の整合が
受理された後に `accepted/may-proceed` と exact authority/resolution/review refs を記録する。Issue #181 の implementation PR は installed
static/shared tool を actual source で実行し、memory と close/reopen SQLite の snapshot/query parity、
prior-head preservation、external exact report artifact/report-set digest、GR `not-qualified`/`qualified` transition を同一 exact-head SHA で検証する。
