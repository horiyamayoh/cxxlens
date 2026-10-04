# ADR 0113: cxxmonster と monet-code の完成に必要な共通解析基盤

- Status: Accepted
- Contract-ID: `cxxlens.application-consumers.v2`
- Supersedes: ADR 0112 の単一 TU の実用例を完成範囲とする解釈
- Authority: 次世代統合設計の原則、relation registry、public API catalog

## 目的と完成範囲

cxxmonster への単一 TU の適用で、公開 query をアプリへ渡す経路の価値は確認できた。
次の完成範囲は cxxmonster v1 と monet-code v1 の規範仕様全体である。実装を依存順に
分けることは、完成範囲を最小例へ縮小することを意味しない。実装途中の機能を
完成済み・対応済みと表示しない。

独立 consumer は次の二つである。

| Consumer | cxxlens の結果を使う目的 | アプリ側が所有するもの |
|---|---|---|
| cxxmonster | プロジェクト構造と依存関係を Core32 と説明可能な形態へ変換 | census、component policy、32 遺伝子、10 morphotype、CXMIR、scene/LOD、Studio、画像、配布 |
| monet-code | C/C++ の計測・静的所見・変更影響・読解・比較 | 360 metric descriptor/evaluator、finding rule、cohort/statistics、decision、archive/series、offline report、plugin/config |

両アプリに AST producer を別々に持たせない。Clang の private adapter と共通の C++
意味解析は cxxlens が所有し、公開境界は値、relation、snapshot、query とする。
アプリの指標、描画モデル、risk ranking を kernel の意味規則に混ぜない。

## 調査による既存 capability と不足

| 領域 | 既存 capability | 完成に必要な追加・接続 |
|---|---|---|
| 入力 | compdb capture、GCC/MSVC replay、source closure、variant identity | project-wide work plan、全 TU/variant、response file、明示 path map、partial partition の継続 |
| 意味 kernel | conditioned claim、conflict、unresolved、closure、in-memory/SQLite store、query | 新しい C++ relation を同じ claim/query の契約へ組み込む |
| Clang observer | function/type/direct-call 観測と canonicalization | project header、namespace/record/field/variable/template/module、ownership、include/type-use/inheritance/override、PP |
| 関数本体 | lifetime を限定した compiler callback | syntax、CFG、def/use、reaching definitions、liveness、points-to、range/null、effects、モデル付き summary |
| cxxmonster | graph、Core32、fixed layout、visual grammar、scene、Studio 部品 | 完全な入力グラフを composer と説明画面に接続、全操作、ZIP、PNG、desktop 配布、Windows lower tiers |
| monet-code | typed fact、graph/clone/statistics/architecture/decision/finding/archive/report 部品 | 公開 query adapter、360 evaluator、全 derived table、全 report route、privacy/export/plugin を CLI の経路へ接続 |

monet-code の native CFG producer は移植元として使い、Clang object を公開したり、
monet-code の type に cxxlens を依存させたりしない。既存のグラフ・保存・表示部品を
再利用するが、単独 unit test の成功を利用者の経路が完成した証拠とはしない。

## 共通契約と identity

write scope は registry/public API/schema、private C++ adapter/analysis、SDK export、
対応する consumer adapter と普通の回帰試験である。source の書き換えは含まない。
新しい relation は schema-first で registry に宣言し、installed static tag と dynamic
descriptor が同じ contract を使う。各 relation は versioned、Clang provider は explicit
local execution とする。provider の認定・品質署名・Git SHA admission は使わない。

解析するのは正規化した全 compile unit/variant である。`--file` は source による filter
であり、その source の最初の command を選ぶ機能ではない。等価な command は全 origin
を保ってまとめ、相異なる command は別 partition とする。順序、physical root、時刻、
worker 完了順を semantic identity に混ぜない。header の occurrence と entity、physical
fact と variant fact を区別し、TU 数だけ同じ source を重複計測しない。

native-only SDK の `borrowed_translation_unit::preprocessor()` は既存の `ast()` と同じ
callback-scoped な borrow とする。Clang type は explicit native SDK のみにあり、共通
relation/analysis/consumer API に露出しない。preprocessing record は compiler job が
所有し、macro/include の値を callback 中に detach する。

canonical declaration/USR と構造を組み合わせた identity を使う。表示名や pretty type
だけで entity を同定しない。local/anonymous entity には親・構造・source anchor を使う。
template/ABI/variant が一致しない entity を黙って一つにしない。外部宣言も entity と
して保持し、body がないことと宣言がないことを区別する。

source/include resolution は frozen project input を使う。toolchain/SDK の live input が
残る場合はその範囲を保証に明記する。response file と path map は入力として扱い、
compile command、plugin、shell script を実行しない。解析器は明示 argv による別 process
で起動でき、protocol/size/cancellation を境界で検査する。

## relation と解析 family

既存の `build.*`、`source.file/span/origin`、`cc.entity/declaration/type/type_component`、
`cc.call_site/direct_target` を維持する。次の family を versioned relation として追加する。
ID と列の確定は registry で行い、下表の family 名を未実装 descriptor と偽装しない。

| Family | 必要な値 | 使用先 |
|---|---|---|
| declaration detail/ownership | entity、parent、source、definition、access/linkage/attributes、parameter/template/public surface | hierarchy、object/function/template、UML、ABI impact |
| include/preprocessor | source/target、spelling、resolution、condition、macro definition/use、pragma/module、token origin | include graph、PP/config/portability、compile impact |
| typed semantic edge | type-use、inheritance、override、member access、state read/write、explicit candidate membership | dependency、object cohesion、state/config impact |
| syntax | entity/parent/source、statement/expression/operator/token class、constant、type | control/readability/function/clone、portable/rule predicates |
| body/CFG | body eligibility、entry/exit/block、edge kind、exception/cleanup/coroutine、frontier | cyclomatic/path/essential complexity、dominance、sequence candidates |
| dataflow | defs/uses、def-use、reaching/live sets、points-to、control dependence、range/null、summary | dataflow、taint、state/resource/lifetime/security/concurrency models |
| layout/model effect | size/alignment/field offsets、ABI fingerprint、versioned API/model effect with witness | padding/ABI、resource/lock/error/taint facts、counterevidence |

最初の compiler producer は `cc.entity_detail/edge`、`cc.syntax_node`、`cc.body`、
`cc.cfg_node/edge`、`cc.flow_fact`、`cc.layout_fact`、`source.include`、
`source.preprocessor_event` と既存の `cc.declaration/type/type_component` を公開する。
`cc.entity_detail.canonical_type` は compiler の型を structural type graph に結ぶ。
`cc.body.analysis_profile` は CFG の構築設定を identity に含める。
`clang22-cfg-eh-lifetime-v1` は EH、implicit/temporary destructor、lifetime、scope、
loop exit、new allocator、initializer、rich constructor を有効にし、定数条件の枝を消さない。
PP callback の追加は private native job 内で行い、評価された条件の true/false と
not-evaluated、skipped range、macro definition/expansion/undef、pragma を値として detach する。
実行されなかった directive の存在と、実行された directive の評価結果を混同しない。
未使用の defaulted member と dependent template の例外仕様は compiler が未評価のまま
保持する。観測のために instantiation を起こさず、`noexcept_unknown` と不足理由を保存する。
project observer の logical output は 128 MiB を上限とし、caller は下方向に制限できる。
これは project header を含む通常の入力の許容量であり、入力ごとの resource failure は
残りの partition の観測結果を無効化しない。

`cc.flow_fact` の `definition/use/call/dereference/constant/null/points_to` は CFG point と
source witness を持つ。`reaching_in/out` と `live_in/out` は JSON 配列、`def_use` は到達する
definition fact ID の JSON 配列を value に持つ。空配列はこの local analysis の集合であり、
alias、external effect、open CFG、budget の frontier があるときには意味上の absence proof
にならない。iteration budget 到達時は guarantee=unknown と不足理由を保存する。
条件付きの型配置は variant ごとの `cc.layout_fact` で示す。一般の `-D` を ABI context の
identity に混ぜず、実際の ABI 関連 builtin/option と compiler/target を用いる。

CFG と flow の identity は entity、body、variant、versioned analysis semantics から作る。
compiler address、可変表示番号、host pointer は使わない。candidate set を確定 edge の
列挙と混同しない。model が不明な call、inline assembly、dependent body、未知の alias、
budget 打ち切りは明示 frontier とし、空集合や安全証明に変換しない。

モデルは versioned C/C++ standard library、C/POSIX/Windows API と user-supplied summary
を対象とする。意味が不明なライブラリの behavior を名前だけで捏造しない。識別条件は
canonical declaration、signature、namespace/header/origin と explicit model policy を持つ。
flow-sensitive intraprocedural の固定点と call SCC summary を分離し、conservative な
candidate、heuristic finding、証明済み property を別値として返す。

## 結果と不足情報

| 結果 | 意味 | consumer の扱い |
|---|---|---|
| proved | 対象 domain と assumptions の下で証明された命題 | proof/witness と domain を表示 |
| disproved | 反例が得られた命題 | 反例 path と source を表示 |
| unknown | 必要な body/model/input/closure がない | reason と取得・追加・再解析 action を表示 |
| partial | 一部 partition のみ観測・解析できた | valid partition を保存し不足 partition を表示 |
| conflicting | 同一条件で両立しない claim がある | conflict を保存し勝者を選ばない |

query export は列だけでなく claim、condition、coverage、closure、unresolved、conflict、
differential disagreement、guarantee、provenance を保持する。独立 relation scan を使い、
inner join で未知の target/body を落とさない。空の scan に relation-wide closure を勝手に
与えない。source/header/TU/variant/body/analysis ごとの coverage を残す。

installed SDK に `sdk/query_transfer.hpp` の値所有 decoder を設ける。両 consumer が
private parser や provider transcript に依存せず、同じ公開型の row と全 side channel を
受け取るためである。`decode_application_queries` は supplied `relation_engine` の
descriptor に対する独立 scan の Logical IR、digest、column type、snapshot/publication、
bound contributor projection、guarantee fragment digest を検査する。任意の query plan の
実行、provider の認定、Store への claim adoption は行わない。値は Clang、filesystem、
process と無関係であり、並行 read と consumer 側の保存が可能である。
duplicate/missing member、誤った型、未知 schema/relation、切れた JSON、binding の破損、
caller の byte/row/depth/metadata budget 超過は typed error とする。`closed`、partial、
conflicting を decoder の成功へ丸めず、完全な結果と不足・証明情報を独立に保持する。
consumer ごとの bounded decode と canonical round trip、破損・resource の通常試験を行う。

`standard_relation_descriptors()` は installed SDK から現行の registry descriptor を
descriptor ID 順の immutable span として返す。consumer はこれを自分の registry に追加
してから query transfer を読み込める。三つの frontend observation は従来通り dynamic
relation であり、Clang object や native SDK への依存は持たない。metadata の共有先を
portable kernel に移し、provider と consumer が同じ descriptor binding を使う。
追加の consumer registry、独自の JSON descriptor 解釈、品質認定は必要ない。
descriptor 数を機能完成の主張に使わず、versioned relation の受け渡しにだけ用いる。

TU-wide な extraction limitation は frozen input basis の partition に一度保存する。
canonical assertion ごとの partition はその claim の出典と参照不足を保持し、同じ
TU-wide diagnostic を重複して載せない。独立 scan は両方の coverage を返す。

claim batch の照合は全行の総当たりを避ける。reference は target relation/column projection
と厳密な scalar storage value で索引化し、候補に従来の interpretation、condition universe、
全 source condition を含む単一 target の規則を適用する。container element、hard failure、
soft unresolved の意味は維持する。functional conflict は同じ descriptor/semantic key の
組だけを照合し、異なる key の全組合せを生成しない。索引の host address や列挙順は
identity と出力に入れず、従来と同じ canonical merge と不足情報を返す。

unknown の completion action は、missing source の供給、path map、compile command/SDK
の修正、model の追加、budget の増量等、利用者が実行できる操作である。未実装 feature
は開発上の gap として扱い、利用者側 input が不足しているようには表示しない。

Local flow frontiers are also typed `cc.flow_fact.v1` rows: `kind=frontier`, the
owning function and compile unit, an observed CFG node, a stable reason in
`value`, and `guarantee=unknown`. The diagnostic side channel remains intact.
This lets consumers restrict a missing alias/effect/fixed-point prerequisite to
the actual body and variant instead of treating an omitted fact as zero or
discarding every body's values because one TU contains a frontier.

## monet-code の全 family への対応

360 の descriptor は個別の qualifying predicate/formula を持つ。共通の generic count を
全 metric に流用しない。以下は入力の依存関係であり、metric 数の削減ではない。

| Family（各20項目） | 必要な入力・実行 |
|---|---|
| health | inventory/command/parse/body/relation/config coverage と frontier |
| size | physical source と syntax の scope、全 grain の集約 |
| preprocessor | include/macro/condition/token/origin |
| control | syntax、closed CFG、SCC/dominator/path（budget 明示） |
| readability | source/token、identifier/comment/operator、scope |
| function | declaration/signature/parameters/body/return/call |
| object | members/access/inheritance/override/type-use/layout/field access |
| graph | typed include/call/type/component graph、SCC/layer/centrality/closure |
| dataflow | defs/uses、reaching/live/alias/control/slice/taint/range/null/summary |
| resource | versioned effect model、CFG/flow/lifetime、finding cluster |
| reliability | errors/exceptions/cleanup/noexcept/assertions/range/null、findings |
| concurrency | shared state/atomic/lock/order/thread model、frontiers/findings |
| security | external entry/source/sink/validation、flow/range/model/findings |
| portability | dialect/ABI/size/layout/PP/API/extension facts、all variants |
| template | primary/instantiation/specialization/constraints/dependent coverage |
| clone | Type1/2/3 token/AST candidate と verified similarity/union |
| testability | public entry/seams/coupling/CFG と explicit coverage/test-map import |
| statistics | compatible grain/cohort の median/MAD/IQR/percentile/residual |

external SARIF、coverage、test map、architecture policy は monet-code が import し、明示
mapping と provenance を付ける。存在しない履歴から churn/author/age を作らない。
外部 coverage がなければ関連値は not_available であり zero ではない。

## 製品接続と利用者の完成条件

monet-code は全49 FR を満たす経路を持つ。analyze から全 grain の360 evaluator、
7 finding family、clone/architecture/cohort、8種類の impact、6 reading track、attention、
UML へ接続する。canonical CSV/JSON/JSONL/SARIF を immutable snapshot/series へ atomic
publish し、compare/timeline、privacy 4 profile、annotation、全 export と
doctor/report/verify/migrate/recover/gc/plugin/config を完成させる。offline report は
Overview、360 Metric Wall、focus chart、entity tabs、typed graph/matrix/SCC/path/layer、
検索・filter・戻る・bookmark を提供し、file:// で外部通信なしに使える。

cxxmonster は全 project の input evidence を CXMIR に保存し、component policy、Core32、
10 morphotype、fixed layout、primitive lineage/conservation、3 LOD を同じ値から生成する。
Studio は open/drag-drop/recent、camera/preset、hover/select、isolate/hide/explode、relation
layer/feature overlay、entity/source/member/call navigation、Why-this-shape、4 view を
提供する。PNG/canonical SVG と CLI の analyze/compose/build/validate/inspect/export/doctor、
canonical ZIP と directory、Electron desktop、relocatable package を実際の経路へ接続する。

## 依存順の実装

1. 全 project/variant の公開 query export、partial partition、header と identity。
2. schema と compiler producer: declarations、PP、typed edge、syntax、CFG、flow/layout。
3. model/summary と必要な analysis: alias/range/null/taint/resource/error/lock/lifetime。
4. monet-code の旧 pin/認定 bridge を置換し、全 evaluator と derived table を接続。
5. cxxmonster の全 graph/Core32/説明・操作・export/archive を接続。
6. 両製品の全 CLI/report/desktop/privacy/comparison/plugin と native platform/scale を検証。

依存が独立した作業は先に進めてよいが、未完成の下流経路を完了と判定しない。仕様の
vNext（source rewrite、cloud/collaboration/VR、clone Type4、runtime truth 等）は今回の
範囲外である。v1 必須の Windows、scale、accessibility を vNext へ移してはならない。

## 通常の検証

実在する複数 TU/共有 header/複数 variant の fixture と両リポジトリ自身を解析し、
positive/negative/fault、header dedup、conflict、missing model、cancel/budget、order/root
relocation の試験を行う。query と保存物から UI/CLI の利用者操作まで検証する。
metric は独立した formula/oracle corpus、Core32 は fixed arithmetic/conservation、
archive は corruption/crash/互換性、表示は browser/desktop/keyboard/200% を検証する。
Ubuntu と Windows の native path、S/M/L capacity、bounded shard/DOM/heap も対応宣言の
前に対象環境で実行する。試験ログは普通の command/CI log でよく、認定 manifest、
tested SHA 台帳、確認者 record、試験証跡を検証する仕組みを追加しない。

## Body enumeration for consumer metrics

`cc.body.ast_node_count` と `ast_state` は、function body の statement/expression
を callback 内で最後まで走査した結果を表す。parameter の default expression と
nested function の body は当該 function の計数へ加えない。source span を持てない node
があれば state=partial とし、未観測を zero に変換しない。CFG の eligibility と AST の
enumeration state は別であり、dependent CFG でも AST を観測できる場合がある。

`cc.syntax_node.flags` は statement/expression、implicit、および直接の親に対する
role_body/condition/then/else/initializer/increment/lhs/rhs/callee/argument/return_value
を値として保持する。argument_N は zero-based な parameter position とする。
entity detail は default_argument、mutable_lvalue_reference、local_variable、consteval
を追加する。これらは compiler が観測した性質であり、API model や安全性の推定ではない。

consumer は body と同じ condition/interpretation と source range に属する node を
集め、宣言 cardinality と実際の distinct ID 数、親参照と root を照合してから
finite body domain の計数を使う。宣言 cardinality が欠ける、矛盾する、partial である
場合は不足・conflict を保持する。project-wide absence claim の closure を、この局所
enumeration から推測しない。
Local variable declarations are enumerated during the same active body walk.
`cc.body.local_variable_count` counts explicit `VarDecl::isLocalVarDecl()` nodes,
including static locals and excluding parameters and nested function bodies.
Consumers must match this cardinality against distinct owned declaration facts
before presenting a count or zero. Expression nodes used as statements carry
both `expression` and `statement`; conditions and initializers do not gain the
statement flag merely because Clang derives `Expr` from `Stmt`.

Type descriptors 1.1 add the named structural profile, exact component-signature
preimage and separate structure state. The profile `clang22-structural-type/1`
uses the exact retained UTF-8 bytes in semantic digest domain
`cc.clang22.type-components.v1`; constructor, qualifiers and nominal entity remain
separate typed fields. Child references lead to retained type rows. Value
components also retain canonical preimages (constant-array decimal extents use
`cc.array-extent.v1`). A missing preimage or partial structure is unavailable for
consumer canonical equality; spelling remains display data. Existing identity
projections are unchanged and the additional field bundle is all-or-none.
Monet must construct conditioned keys from these actual structures, and must
never union signatures from distinct variants into a new global identity.

Declaration details retain the actual C/C++ language, named owning module (or
`<none>`) and compiler calling convention (`clang22.cc/<numeric>` for functions,
`<none>` otherwise). These fields and structural type profiles supply Monet's
canonical dimensions; the consumer must not assume C++, default calling
conventions or a module-free declaration when input is unavailable.
