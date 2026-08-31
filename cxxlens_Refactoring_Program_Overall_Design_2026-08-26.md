# cxxlens 再設計・簡素化・マルチツールチェーン解析プログラム 全体設計書

> **文書ID:** `CXXLENS-REFORM-ARCH-001`  
> **版:** `1.0.0-currentized`  
> **作成日:** 2026-08-26  
> **レビュー反映日:** 2026-08-26  
> **基準候補:** `codex/polish-hardening-2.0` HEAD `431cbd891db0`（main統合後に再確認）  
> **基準確認:** main統合後のmain workflow、変更固有試験、known consumer censusをPhase 0で再確認する  
> **文書状態:** acceptedな統合設計とmachine contractに従属する、Phase 0–1実施用の現行化済みプログラム設計（統合後再確認待ち）  
> **変更クラス:** `CH-0`（本書は既存の製品意味論・wire・persisted format・public stable surfaceを変更しない）  
> **対象:** cxxlensの過剰品質・過剰統制の排除、内部再設計、重点的hardening、GCC/MSVC製アプリケーション解析対応、機能開発再開  
> **改訂要旨:** 先行して実装された `cxxlens::sdk` 単一surface、薄いCLI、OpenSSL Ed25519 port、SQLite安全境界を基準に内容を更新した。ADR 0106に従い、製品runtimeのprovenance/coverage/unknown/安全receiptは保持し、開発・releaseの運用証跡、report、tag、milestone、checkpointは要求しない。

---

## 0. エグゼクティブサマリー

本プログラムは、現在のcxxlensが獲得した強い意味論と安全な境界を残しながら、開発速度・理解可能性・変更容易性を損ねている過剰な契約、重複authority、極端な物理実装保証、micro-target化、巨大なshadow validatorを段階的に解体するものである。

採用する実行順序は次のとおりとする。

```text
Phase 0  製品憲法・基準線・保全対象の確定
   ↓
Phase 1  過剰品質・過剰統制・重複authorityの排除
   ↓
Phase 2A リファクタリングとアーキテクチャ再設計
   ↓
Phase 2B 安定した高リスク境界だけの重点hardening
   ↓
Phase 3  GCC/MSVCで作られたアプリケーションの解析対応
   ↓
Phase 4  機能開発の本格再開
```

各Phaseは、必要最小限の詳細設計と、機械的な受入条件を持つ。**前Phaseのexit gateを満たすまで次Phaseのproduction実装を開始しない。** 次Phaseの非破壊的な調査、計測、feasibility spike、文書作成は許可するが、現行contractやproduction pathを先行変更してはならない。各Phase内部では小さなvertical sliceとしてmainを常に動作可能に保つ。Phase全体を一つの巨大branchへ隔離するbig-bang方式は採用しない。Issue、milestone、receipt、checkpointをPhase完了の前提にしない。

本プログラム中は、無関係な新機能開発を一時停止する。これは意図的な集中戦略であり、リスクを伴う。そのリスクは、現行コード・公開surface・代表ユースケース・実プロジェクトcorpusをPhase 0で凍結し、全Phaseで「Golden Journeys」として継続実行することで抑制する。

GCC/MSVC対応の現在の目標は、**cxxlensをGCC/MSVCでビルドすることではなく、GCC/MSVCでビルドされるC/C++アプリケーションを解析すること**である。対応は次の三層で行う。

1. 元コンパイラ・build systemから、呼出し、toolchain、dependency、PCH/module等の**native build evidence**を取得する。
2. GCC互換driverまたは`clang-cl`で、同じ入力を可能な限り忠実に再生する**semantic replay**を行う。
3. replayでは厳密性を確保できない高価値relationだけ、GCC native plugin等の**native semantic provider**で補強する。

精度は「このプロジェクトは全部近似」と一括表示しない。既存のsemantic guarantee modelを活用し、**claim・relation・condition fragment・interpretation domain単位**で、`exact`、`under_approximation`、`over_approximation`、`unknown`と検証modalityを保持する。ここで`exact`は、宣言済みのrelation version、scope、condition、interpretation、assumption、coverage/closureに対して厳密であることを意味し、別compilerの意味論との無条件な同値を意味しない。厳密に確定できる事実は厳密なまま返し、近似部分だけを明示する。

### 0.1 本書で確定する主要判断

| ID | 判断 |
|---|---|
| D-001 | 基準コードは、現行の `431cbd891db0` 系列をmain統合後に再確認したツリーとする。旧 `699cba8…` は履歴上の中間地点であり、現行baseline authorityではない。 |
| D-002 | 無関係な機能追加はPhase 4まで停止する。bug/security/regression修正と本プログラムに必要なmigration機能は例外とする。 |
| D-003 | `unknown != empty`、native object隔離、immutable snapshot、明示的toolchain/interpretation、deterministic semantic identityは製品憲法として維持する。 |
| D-004 | 再設計前の全面hardeningは行わない。責務と境界を確定した後に、外部・永続・不可逆境界だけを重点hardeningする。 |
| D-005 | GCC/MSVC対応は「解析対象toolchain対応」を先行し、「cxxlens自身のhost compiler対応」は延期する。 |
| D-006 | GCC/MSVCの近似精度はclaim単位で表現し、strict consumerが最低保証を選択できるようにする。 |
| D-007 | GCC native providerは、replayの限界を測定後、高価値relationを厳密化する手段として原則推奨する。 |
| D-008 | MSVC native exact providerは研究gateを設け、安定した公開手段が確認できない場合は実装しない。 |
| D-009 | Specを意味のauthority、Testを適合判定とする。小変更へIssue、ADR、receiptを強制しない。Git履歴とCI job logは通常の開発機能として扱う。 |
| D-010 | 既存の巨大contractやhardening実装は即時削除せず、Phase 1で価値・脅威・重複・利用実態を分類してから降格・統合・削除する。 |
| D-011 | 本プログラムが既定で許可するのはinternal変更、compatible additive変更、期限とmigrationを伴うcompatible deprecationまでとし、accepted stable axisのbreaking changeは別提案・別承認とする。 |
| D-012 | Phase gateはproduction実装に適用する。次Phaseのread-only調査やtimeboxed spikeは許可するが、gateの既成事実化には使わない。 |
| D-013 | Build captureはallowlistとredactionを前提とし、credential・token・secretを永続化しない。capture fidelityとsemantic fidelityを別々に表現する。 |
| D-014 | Authority移行中の一時的な二重化は、primary authority、parity検査、retirement条件、期限が明示された場合だけ許可する。 |

### 0.2 期待する最終状態

最終的なcxxlensは、次を同時に満たす。

- Compiler-native型をcoreへ漏らさず、frontend/providerを追加できる。
- 新しいrelationやproviderの追加が中央switch、巨大schema群、数十の手動同期を要求しない。
- 「見つからなかった」と「存在しないことを証明した」を区別する。
- 開発者modeでは軽くbuild/testでき、production hardeningは必要なprofileでのみ有効になる。
- Clang、GCC、MSVC対象コードについて、relationごとの保証強度を明示して解析できる。
- 通常の機能開発が再び中心となり、内部統制が製品開発を支配しない。

---

## 1. 文書の位置付けと適用範囲

### 1.1 本書のauthority

本書は、**リファクタリング・簡素化・マルチツールチェーン対応プログラムの順序、変更境界、phase gateに関する最上位設計**である。製品意味論そのものについては、acceptedな統合設計と各machine contractが、明示的にmigrationされるまで引き続きauthorityである。

判断対象ごとのauthorityは次のとおりとする。

| 判断対象 | Primary authority |
|---|---|
| 現行のrelation、truth/guarantee、identity、wire、persisted format、public stable surface | accepted integrated design / machine contract / catalog |
| 本プログラムの順序、feature freeze、変更クラス、phase gate | 本書 |
| 一つのPhase内のtarget architecture、migration、受入条件 | 承認済みPhase design |
| 作業単位、担当、進捗 | tracking / implementation issue |
| 適合判定 | contract test、acceptance test、Golden Journey comparator |

各Phaseの詳細設計が受理され、migrationが完了した領域から、現行の統合設計・catalog・ADR・schemaを段階的に置換またはsupersedeする。単なる新しい説明文、実装都合、test fixtureはaccepted contractを暗黙に上書きしない。supersedeには、旧authority、新authority、影響version axis、consumer migration、適用開始点を示す`authority retirement / migration map`を要求する。

```text
本書
  └─ Phase別設計書
       ├─ authority retirement / migration map
       └─ acceptance tests
```

### 1.2 対象範囲

- 製品の不変条件と非目標
- authority・schema・ADR・Issue運用
- CMake target、package、source layout、dependency direction
- relation、claim、query、store、provider、materializationの責務境界
- test/CI/release profile
- GCC/MSVC製アプリケーションのcapture・replay・native解析
- feature freeze中の能力保全
- migration、compatibility、risk management

### 1.3 非対象

- Phase 4以降に追加する個々のanalysis機能の詳細設計
- 今すぐのGCC/MSVCによるcxxlens本体build正式対応
- 全toolchain・全architecture・全OSの一括production support
- MSVC非公開内部APIへ依存した製品実装
- 既存version axisを本書だけで即時変更すること
- 本書作成時点でのrelease qualification宣言

### 1.4 調査上の制約

本書は基準commitのrepository tree、設計、schema、public header、CMake、test、CI、hardening差分を静的に読解して作成した。GitHub上では基準branch HEADにworkflow run/statusが見つからなかったため、**build・CTest・sanitizerが成功済みとは扱わない。** Phase 0の最初の作業は、基準commitの再現buildとtest baseline取得である。

`Public/Downstream Surface Census`とconsumer有無の判断は、repository内のfirst-party consumer、公開catalog、既知のdeclared downstream、検索可能な利用実態を対象とする。観測不能な全世界のconsumer不存在を証明したものとして扱わない。

### 1.5 規範表現

本書では、次のように解釈する。

- **MUST / MUST NOT:** 製品憲法、accepted stable contract、phase exitの必須条件。
- **SHOULD / SHOULD NOT:** 原則。逸脱する場合は同等以上の保証と理由をPhase designまたはADRへ記載する。
- **MAY:** 任意。採用しないこと自体は未完了を意味しない。
- 「候補」「例」「推奨default」は、それだけではstable contractを新設しない。

日本語の「しなければならない」「禁止する」はMUST/MUST NOT、「原則」「推奨」はSHOULDとして読む。既存accepted contractがより強い規定を持つ場合は、その強い規定を維持する。

### 1.6 Contract change guardrail

各Phase designは、影響するversion axisと変更クラスを明示する。

| Class | 意味 | 本プログラム内の扱い |
|---|---|---|
| `CH-0 internal` | public/semantic contractへ影響しない内部変更 | 通常scope |
| `CH-1 compatible-additive` | 既存consumerを壊さない追加 | 必要性とstabilityを明示して許可 |
| `CH-2 compatible-deprecation` | replacement/facade/deprecationを追加し、対象releaseでは既存stable consumerを維持する段階的整理 | migration、removal条件、期限を必須化。実際のstable surface削除は原則`CH-3` |
| `CH-3 breaking` | accepted stable API、relation meaning、wire、persisted format等の非互換変更 | 本プログラムへ暗黙に含めず、別提案・明示承認 |

Phase 1〜3の既定budgetは`CH-0`〜`CH-2`である。`CH-3`が必要になった場合、cleanupや内部再設計の副作用として混入させず、影響axis、代替案、migration、consumer impactを独立して審査する。

---

## 2. 現行コードベースの評価

### 2.1 維持すべき強い資産

現行cxxlensには、再設計後も維持すべき明確な資産がある。

| 資産 | 評価 | 方針 |
|---|---|---|
| Semantic Relation Platformという製品定義 | 単一lintやAST wrapperに閉じない強い核 | 維持 |
| typed/dynamic queryが同じLogical Query IRへlowerされる構造 | 利用者層を分けつつ意味を統一 | 維持・簡素化 |
| observation → assertion → canonical/derived claim | 出所と導出を区別できる | 維持 |
| `unknown`、coverage、closure、conflict、unresolved | 不完全情報を誠実に扱う差別化要因 | 維持 |
| immutable snapshotとfailed publication isolation | analysis再利用・再現性の基盤 | 維持 |
| public APIからLLVM/Clang native型を隔離 | GCC/MSVC provider追加にも有効 | 維持 |
| provider process isolationとbounded decode | 外部入力境界として妥当 | 維持 |
| runnable SDK examplesとnegative examples | feature freeze中のGolden Journey候補 | 昇格 |
| hardening branchの公開surface整理 | unadmitted APIを正規writerへ統合 | 基準化 |

特に、[Development architecture](docs/development/architecture.md)が示す`base → kernel → query → cpp`とnative SDK分離は、全面撤廃よりも責務再整理の土台として利用できる。現行の公開author SDKは `cxxlens::sdk` であり、`cxxlens` CLIはその薄い入口である。

### 2.2 複雑性の集中領域

現行の問題は、すべての箇所が悪いのではなく、特定領域へ複雑性が極端に集中していることである。以下のサイズはrepository treeのblob sizeを概算KiBへ換算したもので、品質の良否そのものではなく、調査優先度の指標である。

| 領域 | 代表ファイル | 概算規模 | 観察 |
|---|---|---:|---|
| Clang materialization contract checker | `tools/quality/check_ng_clang22_materialization.py` | 約513 KiB | 仕様validatorが独立したshadow product化している可能性 |
| SQLite contract checker | `tools/quality/check_ng_sqlite_store_contract.py` | 約207 KiB | 物理状態機械の検査責務が巨大 |
| Clang materializer worker bridge | `materializer_worker_bridge.cpp` | 約84 KiB | worker handoff・検証・result adoptionの責務分離が必要 |
| Provider runtime | `src/sdk/provider_runtime.cpp` | 約153 KiB | process、validation、trust、transport責務の集中 |
| Provider SDK implementation | `src/sdk/provider.cpp` | 約133 KiB | public SDKとwire/lifecycleの責務が広い |
| Rooted VFS | `materialization_rooted_vfs.cpp` | 約121 KiB | filesystem threat modelが大きな比率を占有 |
| Bounded Store core | `bounded_store_v6_internal.cpp` | 約114 KiB | v5/v6・memory/SQLite間の役割確認が必要 |
| Snapshot v5 codec | `snapshot_store_v5_codec_internal.cpp` | 約103 KiB | semantic formatとphysical recoveryの分離余地 |
| Query execution | `src/sdk/query_execution.cpp` | 約93 KiB | 意味論は重要、実行責務分割の候補 |
| Root CMake | `CMakeLists.txt` | 約52 KiB | source-private micro-libraryが多数 |

### 2.3 現行authorityの問題仮説

現在は、統合設計、Relation Registry、Logical Query Contract、Guarantee Contract、Store Contract、SQLite Contract、Provider Protocol、Provider Runtime、Clang materialization、Public API Catalog、Security Profile、Support Matrix、多数ADR、巨大Python checkerが相互に同じ判断を保持している。

この構造には次のリスクがある。

1. 一つの意味変更で複数authorityを手動同期する。
2. schema checkerが実装と同程度に複雑になり、二つの製品を保守する。
3. source text・target graph・fixtureの形に契約が結合し、正しいrefactorを拒否する。
4. 実際のconsumer価値より「contractを閉じること」が開発目的になる。
5. 過去の仮説的リスクへ永久にコストを払い続ける。

これはPhase 0で実測・分類する仮説であり、単にファイルが大きいという理由で削除してはならない。

### 2.4 現行build/test friction

現行root CMakeは通常構成でも次を要求・実行する。

- Git checkoutまたは明示されたexact revision/tree pair
- Clang 22 packageの厳密な選択
- materializer向けstatic ICU archiveの厳密なproof
- 多数のsource-private archiveとbridge target
- production sourceのtest-only再コンパイル
- glibc、`dlmopen`、OFD lock、SQLite interposerを用いたplatform-specific fault test

これらの一部はrelease/production profileには妥当でも、通常の編集・build・局所testへ常時要求する必要はない。Phase 1では、**意味契約を弱めずに、開発経路とproduction qualificationを分離する。**

### 2.5 hardening branchの扱い

基準branchの6 commitは、主に次を行っている。

- Public API catalogにない`snapshot_builder`互換経路を削除し、正規`stage → validate → publish` writerへ統一
- 削除surfaceがinstalled headerへ再出現しないnegative compile probe
- 全installed public headerの単独compile
- Provider Protocol decoderへのbounded mutation smoke
- semantic parameter、ownership、sanitizer warning境界の整理
- dependency closure checkerの誤検出抑制

これらは「低価値内部helperの過剰hardening」ではなく、public/install、wire decode、dependency boundaryを守る変更であり、本プログラムのbaselineとして妥当である。

---

## 3. 製品憲法

Phase 1以降の簡素化は、次の不変条件を侵してはならない。

### 3.1 必須不変条件

#### C-01 Native lifetime isolation

Compiler-native object、pointer、reference、ABI handleは、frontend/provider callback境界を越えて保存・所有・thread移送してはならない。coreにはdetached valueだけを渡す。

#### C-02 Unknown is not empty

`unsupported`、`unavailable`、`failed`、`truncated`、`stale`、open world、closure不足をempty successへ変換してはならない。

#### C-03 Absence requires a basis

存在しないこと、exhaustive set、anti-join、unreachable等を確定する場合は、対象scopeに適用可能なclosureまたは同等の完全性根拠を要求する。根拠がない場合、positive factは返してよいがabsenceは`unknown`とする。

#### C-04 Immutable publication

公開済みsemantic snapshotはimmutableであり、failed/cancelled/rejected materializationが既存snapshotを破壊してはならない。

#### C-05 Stable semantic identity

Semantic identityはabsolute checkout root、pointer、timestamp、PID、thread ID、arrival order、unordered iteration、display proseへ依存してはならない。

#### C-06 Explicit toolchain and interpretation

Production compiler、target、ABI、build variant、semantic provider、interpretation domain、fallbackは明示する。Clang replayをGCC/MSVC native exactとして暗黙に扱わない。

#### C-07 Conflict preservation

Same-domain conflictとcross-domain differential disagreementをfirst-wins、priority、arrival orderで消してはならない。

#### C-08 Plan-first irreversible effect

Source mutation・artifact publication等の不可逆effectは、plan、precondition、independent validation、dry-run可能性、transactionを経由する。

#### C-09 Bounded external inputs

Wire、JSON、filesystem、persistent store、build capture等の外部入力は、decode前またはallocation前に合理的なresource boundを持つ。環境変数、command line、response file等を保存する場合はallowlistとredactionを適用し、credential、token、secretをdiagnostic、snapshot、cacheへ永続化しない。

#### C-10 One semantic authority per concept

同じsemanticsを複数の手書き文書・schema・checkerへ重複保持しない。派生物は生成する。migration中の一時的なold/new併存は、primary authority、parity検査、divergence時の扱い、retirement条件、期限を明示した場合だけ許可する。

### 3.2 簡素化可能なもの

次は製品憲法ではなく、Phase 1で撤廃・profile化・延期できる。

- 小変更ごとのADR、Issue、fault matrix
- 通常buildにおけるexact source revision/tree必須化
- 全local executionでのbinary digest、signature、sandbox certification必須化
- SQLiteのすべての稀なWAL/SHM topologyをdefault productで独自証明すること
- 一つのcontractをYAML、schema、Python checker、C++ source text assertionへ重複投影すること
- Testのためだけにproduction ownership boundaryを細分化するmicro-target
- 未採用use caseを想定した完全なprotocol recovery・resume・spill保証
- 全resultへfull provenance graphをeagerかつinlineで常時埋め込むこと。ただし、accepted Semantic Guarantee Contractが要求するlossless contributor identity、drill-down可能性、provenance reachabilityは維持する。
- Experimental能力をstable compatibility surfaceとして扱うこと

### 3.3 Product resultの最低要件

高水準analysis/recipeは、適用できる範囲で次を区別する。

```text
proved / disproved / unknown / partial / conflicting
```

これらは高水準のpresentation outcomeであり、既存のtruth support、approximation、coverage、closure、conflictを置換する新しいtruth enumではない。`partial`はunder/over approximation、coverage不足、unresolved等から導出し、`conflicting`は保持されたsame-domain conflictまたは明示したdifferential disagreementから導出する。

ただし、すべての内部primitiveがこの完全なreport型を持つ必要はない。情報を失わない責務は、semantic boundaryとpublic result compositionへ集中させる。actionableなnext stepを確定できない場合は、推測でcompletion planを捏造せず、既知の不足理由と観測可能な回復条件だけを返す。

---

## 4. プログラム実行原則

### 4.1 完全直列・内部incremental

Phase間のproduction実装は直列とする。一方、Phase内部は小さなvertical sliceとして進める。次Phaseのread-only調査、toolchain feasibility確認、benchmark設計、throw-away spikeは先行できるが、accepted contract、installed surface、production path、persisted dataを変更してはならず、spike codeをそのままproductionへ昇格させない。

```text
悪い例:
  Phase 2を巨大branchで全面rewrite → 数か月後に一括merge

採用:
  一つの責務境界を新実装 → golden parity → consumer移行 → 旧経路削除
  これを繰り返し、Phase gateで全体完了を判定
```

### 4.2 Feature freeze

Phase 0からPhase 3完了まで、無関係な新機能を追加しない。

許可される変更:

- security、data loss、crash、明確なregressionの修正
- baselineを再現するためのtest/fixture修正
- 本プログラムのmigration adapter、compatibility facade、observability
- Phase 3のGCC/MSVC対応能力
- documentation、build、CIの簡素化

禁止される変更:

- 新しいanalysis recipe
- 新しいsemantic domainの拡張
- 利用者要求のないrelation追加
- 将来機能のためだけのextension point
- 機能backlogを先に詳細Issueへ分解すること

新機能案は`Feature Escrow`という単一backlogへ題名と価値だけを記録し、Phase 4まで設計しない。

### 4.3 Mainを常にgreenに保つ

- 現行のmain履歴を基準とし、独自のsource snapshot tagやgreen baseline tagを作らない。
- 基準時点に再現可能なdeterministic failureがある場合、architecture変更を混ぜずにbaseline stabilizationを行う。既知failureを曖昧な例外として恒久容認しない。
- 変更はsmall/atomic commitとする。
- direct-to-mainは、影響testをローカルで通し、revert可能な小変更に限る。R3、広域migration、public compatibility変更では短命branch/PRを優先する。
- whole-phase branchは作らない。
- 必要なら短命なwork branchを使う。
- phase boundaryは設計文書、Git履歴、試験結果で表す。tag、milestone close、phase receiptを完了条件にしない。
- 履歴rewriteや巨大squashで移行過程を消さない。
- `green`は、対象laneの変更固有試験とmainに登録された全決定的CTestが成功し、Golden Journeyとcontract conformanceが成立する状態を意味する。testの無効化やscope除外で見かけ上greenにしない。release qualificationは別workflowの責務である。

### 4.4 「削減」自体を成果にしない

LOC、target数、schema数はdiagnostic metricであって目的ではない。成功は次で判定する。

- 代表ユースケースを壊さない。
- 普通の変更で触るauthority数が減る。
- 層の責務と依存方向が理解できる。
- provider、relation、analysisを追加する実コストが下がる。
- testが製品挙動を検査し、source layoutを固定しない。

### 4.5 Phase gateの運用原則

1. Exit criterionは、test、生成物、測定値、review済みmatrixのいずれかへ対応付ける。
2. criterionを満たせない場合、Issue closeや説明文だけでwaiveしない。criterion自体が不適切なら、gate判定前にPhase designを改訂し、理由と変更クラスを残す。
3. 新たに見つかった非critical cleanupは、現Phaseのaccepted scopeを無制限に拡張せず、次backlogへ送る。製品憲法違反、data loss、security、明確なregressionだけは例外とする。
4. Gate通過は「完全無欠」を意味しない。未解決riskは、実装を止めるものか、後続設計へ渡すものかを通常の設計・Issueで明示する。専用のExit Recordやreceiptは作らない。
5. 次Phaseの調査結果は前Phaseのgateを既成事実化せず、前Phaseの未達を隠す根拠にしない。

---

## 5. Target Architecture

### 5.1 論理構成

```text
┌────────────────────────────────────────────────────────────┐
│ Applications / CLI / CI / IDE / AI agents                 │
└──────────────────────────────┬─────────────────────────────┘
                               │
┌──────────────────────────────▼─────────────────────────────┐
│ Recipes / Analyses / Migration plans                       │
└──────────────────────────────┬─────────────────────────────┘
                               │
┌──────────────────────────────▼─────────────────────────────┐
│ Typed & Dynamic Logical Query                              │
└──────────────────────────────┬─────────────────────────────┘
                               │
┌──────────────────────────────▼─────────────────────────────┐
│ Semantic Core                                              │
│ relation / claim / condition / guarantee / conflict        │
└───────────────┬──────────────────────────────┬─────────────┘
                │                              │
┌───────────────▼──────────────┐  ┌────────────▼─────────────┐
│ Store Port                   │  │ Materialization Core     │
│ memory / sqlite profiles     │  │ plan / partition / reuse │
└──────────────────────────────┘  └────────────┬─────────────┘
                                               │
┌──────────────────────────────────────────────▼─────────────┐
│ Provider Runtime & Protocol                                │
│ local / verified / hardened execution profiles             │
└──────────────────────────────────────────────┬─────────────┘
                                               │
┌──────────────────────────────────────────────▼─────────────┐
│ Frontend Providers                                         │
│ Clang native | GCC replay | clang-cl replay | GCC native   │
│ MSVC build evidence | future MSVC exact research           │
└────────────────────────────────────────────────────────────┘

Build capture is a side input:
Build system / compiler invocation / source closure
  → Compiler-neutral Project & Toolchain Model
  → Provider task
```

### 5.2 コンポーネント責務

#### Base

- strong ID、semantic version、canonical encoding
- errors、budgets、generic diagnostics
- compiler/toolchain非依存

#### Semantic Core

- relation descriptorとregistry
- detached row/cell validation
- observation/assertion/canonical/derived claim
- condition、interpretation、guarantee
- conflict/differential disagreement
- provider、store、frontendの実装詳細を知らない

#### Query

- typed/dynamic builder
- versioned Logical Query IR
- logical validation
- reference executionとresult composition
- physical optimizationはinternal

#### Store

- immutable snapshot semantic model
- publication/series abstraction
- Memory reference backend
- SQLite Standard backend
- Optional Hardened backend/profile
- semantic identityとphysical recoveryを分離

#### Build Capture

- compile unit、original invocation、environment、toolchain、variant
- source closure/dependency graph
- CMake/Ninja/compilation database、GCC wrapper、MSBuild/Build Insights等のadapter
- fieldごとの`observed / derived / redacted / unavailable`状態とcapture coverage
- secretを除外するallowlist/redactionと、machine-local raw evidenceのretention policy
- semantic providerを選択しない

#### Materialization Core

- requested relation、partition、input fingerprint
- provider planning
- incremental reuse
- coverage/unresolved accounting
- Store publicationへのtyped handoff

#### Provider Runtime

- provider manifest/capability
- process/in-process execution
- bounded transport
- cancellation、budget、terminal state
- execution profileごとのtrust/sandbox

#### Frontend Provider

- compiler-native observation
- source normalization
- provider-localまたはcanonical relationへのprojection
- relation単位のguarantee/capability
- native objectをcallback外へ出さない

#### Recipes / Analyses

- consumer questionへ答えるvertical slice
- core private APIへ依存しない
- required relation/capability/guarantee policyを宣言

### 5.3 物理packageの原則

Exact target名はPhase 2Aで確定するが、次を原則とする。

1. Public packageは利用者役割ごとに少数とする。
2. Internal targetは「独立したdeployment、ABI、lifetime、再利用ownership」がある場合だけ作る。
3. Focused testのためだけにproduction sourceを別archiveへ切らない。
4. Compiler-specific sourceは`frontends/<family>/<major>`へ閉じる。
5. Store semantic layerとSQLite VFS/recoveryを別責務にする。
6. Clang materialization専用request envelopeをgeneric materialization authorityとして使わない。

### 5.4 Strangler migration

全面rewriteは行わない。各boundaryを次の順で置換する。`golden parity`は、diagnostic prose、timing、temporary path、unordered row orderのbyte一致ではなく、canonical semantic identity、truth/approximation、coverage/closure、unresolved、conflict、publicly observable behaviorの一致で判定する。

```text
new interfaceを追加
  → old implementationをadapter経由で接続
  → golden parityを確認
  → consumerをnew interfaceへ移行
  → old pathを削除
  → compatibility facadeの要否を判断
```

Old/new pathを一時併存させる場合、authoritative write/read path、dual-run comparator、divergence時のfail policy、retirement条件、最長存続Phaseを先に定める。silent fallback、first-wins、期限のない二重実装は認めない。

---

## 6. Authority・Spec・ADR・Issueの再設計

### 6.1 Authority hierarchy

今後のauthorityは次の四層へ整理する。

| 層 | 役割 | 手書き可否 |
|---|---|---|
| Product Charter / Phase Design | 原則、scope、非目標、重大判断 | 手書き |
| Machine Contract | public wire、schema、relation、formatの一意なauthority | 手書きは各概念1個 |
| Generated Projection | C++ tag、JSON Schema、docs table、fixture skeleton | 自動生成 |
| Tests / Implementation | contractへの適合を確認 | 実装 |

同じfield、enum、version、invariantを複数authorityへ手動複写しない。`一概念一authority`は一つの巨大fileへ集約することを意味しない。domainごとにcontractを分割してよいが、各conceptのnormative ownerは一意にする。

### 6.2 Artifact分類

すべての文書/schema/checkerを次へ分類する。

- **Normative:** 実際のcompatibility/semanticsを所有する。
- **Generated:** Normative sourceから再生成できる。
- **Explanatory:** 人間向け説明。semanticsを所有しない。
- **Test fixture:** 入出力例。一般契約を所有しない。
- **Historical:** archive。新実装を拘束しない。
- **Retire candidate:** consumerもauthorityもない。

Phase 1では`Authority Retirement Matrix`を作り、各artifactに分類・replacement・削除条件を付ける。

### 6.3 ADRの適用範囲

ADRを必須とするのは、後戻りが高価な次だけである。

- Public semantic identity
- Wire protocol major
- Persisted format/migration
- Native lifetime/ABI boundary
- Security/trust model
- Irreversible source/artifact effect
- Cross-version compatibility policy

通常のrefactor、private helper、内部target分割、test追加、additive experimental APIへADRを要求しない。

### 6.4 Issue policy

Specが意味、Issueが作業、Testが受入を担う。

**Issueを作る条件:**

- 独立した成果物またはexit criterionがある
- 並行作業可能
- 他作業をblockする
- 単独review単位として意味がある
- public migrationや互換性へ影響する

**Issue不要:**

- private rename/helper抽出
- dead code削除
- warning修正
- test fixture追加
- typo/documentation correction
- 同じimplementation issue内の小さな追補

### 6.5 作業項目の最小形式

```markdown
## Outcome
このPhaseで利用者・開発者に何が変わるか

## Scope
実施する責務・成果物

## Non-goals
今回は行わないこと

## Inputs
参照するPhase design / baseline

## Contract impact
影響version axisと`CH-0`〜`CH-3`。影響なしの場合は`CH-0`

## Acceptance
機械的・観察可能な完了条件

## Dependencies
前Phase gateとblocker

## Risks
主要な失敗モード
```

Issueは並行作業、外部consumer migration、または独立したreview単位として有用な場合だけ作成する。Issueごとのreceipt、exact SHAコメント、学習checkpoint、巨大checklistは要求しない。Git履歴、CTest、GitHub Actionsのjob logを通常の開発機能として扱う。

### 6.6 Authority migration protocol

Authorityを統合・置換する変更は、次の最小手順で行う。

1. 現行authority、generated projection、consumer、checkerをinventoryする。
2. Old authorityをprimary、新authorityをcandidateとして宣言する。
3. Candidateからprojectionを生成し、既存fixtureでsemantic parityを検査する。
4. Consumerを一つずつcandidateへ移行し、old/new divergenceをfailとして扱う。
5. 最後のconsumer移行後にcandidateをprimaryへ昇格する。
6. Old authorityをHistoricalまたはRetiredへ変更し、探索・生成・CIから除外する。これは製品のprovenanceやruntime safety receiptを削除することを意味しない。

Dual-writeで二つのsemanticsを独立生成しない。移行中も意味のsourceは一つであり、もう一方はprojectionまたはcomparison targetとする。

---

## 7. Risk-based Quality Model

### 7.1 Risk tier

| Tier | 例 | 必須設計 | 必須試験 |
|---|---|---|---|
| R0 Internal/Pure | private helper、pure transformation | なしまたは短いコメント | unit/compile |
| R1 Product Behavior | recipe、query operator実装、capture adapter | Phase spec内のbehavior | positive/negative/integration |
| R2 Public Semantic Boundary | public API、relation、provider capability | machine contract、compatibility | conformance、parity、negative |
| R3 High-risk Effect/Format | wire major、persistent format、process sandbox、source mutation | ADR、state/effect model、migration | fault/crash/resource/security |

`fault test`、`phase-authentic outcome union`、`crash matrix`を全変更へ要求せず、R3へ集中させる。複数tierに該当する変更は最も高いtierを採用する。たとえばcapture adapterがuntrusted file/process入力を扱う場合、純粋な変換部分がR1でも境界全体はR2またはR3として扱う。

### 7.2 Execution profiles

#### Development profile

- Git checkout外でもbuild可能
- package provenanceはoptional
- local provider execution
- MemoryまたはSQLite Standard
- fast deterministic tests
- Developmentで得た結果をVerified/Hardenedとして表示しない

#### Verified profile

- executable/toolchain digest
- exact provider capability
- install/relocation checks
- standard sandboxまたはprocess isolation

#### Hardened profile

- threat modelで必要な場合のみ
- extended filesystem/VFS checks
- crash/recovery/fault suite
- release/nightly qualification

安全機能を削除するのではなく、必要な利用形態へ費用を割り当てる。Profile名は単なるbuild optionではなく、対応するcapability、test lane、failure policyを満たした場合だけ名乗る。Hardened固有testの失敗はDevelopment作業を不必要にblockしないが、shared codec、semantic publication、data integrity等の共通invariant違反はprofileを問わずblockする。

### 7.3 Test lanes

```text
Local affected tests
  → Main deterministic suite
      → Nightly/periodic heavy suite
          → Release qualification
```

| Lane | 内容 |
|---|---|
| Local | 変更対象unit、contract、Golden Journeyの一部 |
| Main | deterministic unit/integration、public/install、architecture fitness |
| Periodic | sanitizer、stress、mutation/fuzz、large corpus、crash matrix |
| Release | support matrix全対象、package、relocation、migration、real projects |

重いtestを削除する前に、mainからperiodic/releaseへ移せないかを判断する。

### 7.4 Shadow validatorの解体

巨大な`check_ng_*` scriptは、次へ分解する。

1. Schema validation
2. Generated artifact reproducibility
3. Behavioral conformance
4. Source-layout assertion
5. Fixture generation

Essential semanticsはruntime/compiled conformance testへ移す。Source textやtarget名をparseして構造を固定するassertionは原則削除する。Checkerが製品実装をもう一つ再実装している場合、single shared codec/validatorへ統合する。ただし、provenance reachability、unknown/closure、persisted format migration等のaccepted semanticsを「shadow validator削減」の名目で失わない。

---

## 8. Phase 0 — 製品憲法・Baseline・保全対象

### 8.1 目的

簡素化で削ってよいものと、製品価値として守るものを、実コード・利用経路・testで確定する。Feature freeze中のarchitecture driftを防ぐ。

### 8.2 成果物

1. `Product Constitution`
2. `Current Architecture & Authority Inventory`
3. `Public/Downstream Surface Census`（観測可能scopeと限界を明記）
4. `Golden Journey Specification`（canonical comparator規則を含む）
5. `Real-project Corpus Specification`
6. `Contract Preservation Map`（version axis、change class、facade/retirement）
7. `Phase 1 Detailed Design`

上記は設計・機械的検査の入力であり、実行ごとのbaseline report、quality report、checksum、receipt、artifactを生成・保存しない。実行結果は変更固有試験、CTest、main workflowの終了コードで判定する。製品runtimeが返すclaim/provenance、coverage、unknown、materialization report、SQLite/source-closure receipt、provider trustはこの除外に含まれない。

### 8.3 Baseline作業

- `431cbd891db0` 系列をmainへ統合した後の履歴を基準とし、独自のsource snapshot tagを作らない。
- static/shared、Memory/SQLite、installed consumerをbuildする。
- deterministic CTestを全件実行する。
- 再現可能なbaseline failureは、環境不足、unsupported、製品defect、test defect、flakeへ分類する。製品/test defectはarchitecture変更を混ぜずに修正する。
- Flakeは失敗を無視せず、再現条件、検出test、修正方針を持つ。Phase 0 exit時に未分類flakeを残さない。
- build/test時間、target数、public header数、machine contract数、checker数、通常変更のfan-outは診断情報として必要な場合だけ取得する。数値を品質完了条件にしたり、別reportへ保存したりしない。
- current support matrixは既存のschemaをauthorityとし、複製したbaseline artifactを作らない。

Baselineの目的は、current treeでどの試験が成立しているかと、環境要因・unsupported・product defectを区別することである。release qualification、sanitizer、stress、maximum scale、real-project package判定はPhase 0/1のbaselineとは別のrelease workflowで扱う。

### 8.4 Golden Journeys

最低限、現行examplesから次を正式な能力保全fixtureへ昇格する。

| Journey | 守る能力 |
|---|---|
| Generated typed query | static tagとLogical Query IR |
| Runtime dynamic query | runtime schemaとtyped path parity |
| Relation/claim batch | observation、identity、reference、conflict |
| Portable provider | external relation、coverage、evidence |
| Clang 22 native provider | AST lifetime isolationとsource normalization |
| Snapshot Store | Memory/SQLite semantic parity、writer lifecycle |
| Query execution | result side channel、ordering、budget |
| Flagship call-search recipe | end-to-end consumer value |
| Installed consumer | package、relocation、header closure |

Golden outputは、意味上必要なcanonical projectionを保存する。すべての内部byte、timing、exact logを固定しない。Comparatorは最低限、semantic identity、relation/claim内容、truth、approximation、condition、interpretation、coverage、closure、structured unresolved state/reason、conflict、public ordering guaranteeを比較する。Temporary root、PID、timestamp、diagnostic prose、unordered iteration orderは比較対象から除外する。

意図的な差分は、単にgoldenを更新して吸収せず、変更クラス、consumer impact、migration理由を記録する。`CH-3`相当の差分は本プログラムの通常migrationとして承認しない。

### 8.5 Real-project corpus

Phase 0では少なくとも次を選ぶ。

- 小さなCMake C++ project
- macro/template/virtual callを含むproject
- multi-TU project
- current real-project integration fixture

Phase 3開始時にGCC-built corpusとMSVC solutionを追加する。Corpusはrevision、toolchain、license、build commandを固定し、通常test時にnetwork accessを要求しない。Credential、社内path、個人情報を含むfixtureは採用しない。

### 8.6 Exit gate

- `431cbd891db0` 系列を統合した現行mainのsource treeをfreshに再構成でき、定義済みのstatic/shared・installed・決定的CTestがgreen
- Golden Journeysが自動実行可能で、canonical comparator規則が固定
- 列挙したin-scopeのpublic/admitted/accidental/internal surfaceが100%分類済みで、未分類項目がゼロ
- 列挙したin-scope authority artifactが100%分類済みで、各artifactにowner分類・replacement・retirement条件のいずれかがある
- Known first-party consumerとdeclared downstreamのsurface利用が対応付け済み。観測限界も明記
- 憲法のkeep/remove判断が承認済み
- Phase 1の削減対象にreplacementまたはretirement条件と`CH-0`〜`CH-2`分類がある。`CH-3`候補は別提案へ分離
- Phase 0成果物に運用専用report、receipt、checkpoint、tag、milestone依存がない

---

## 9. Phase 1 — 過剰品質・過剰統制の排除

### 9.1 目的

製品意味論を維持しながら、変更コストを増加させる重複authority、過剰なgovernance、default強制hardening、build frictionを排除する。

### 9.2 Workstream A: Authority consolidation

- Relation Registryをrelation semanticsの一意なsourceにする。
- Generated C++ header、documentation table、schema projectionを自動生成する。
- Query/Provider/Storeについてもconceptごとに一意なnormative sourceを定め、派生物を生成する。Domainを一つの巨大machine contractへ集約することは要求しない。
- Integrated designはprinciple/architectureへ縮小し、field-level exact contractを持たない。
- ADR indexにsuperseded/activeを明示する。
- Archiveを新authority探索対象から除外する。

### 9.3 Workstream B: Governance simplification

- AGENTS/CONTRIBUTINGをrisk tierベースへ改訂する。
- 小変更のIssue/ADR不要を正式化する。
- 「全新surfaceでfull completion plan」を、public/consumer-facing変更だけへ限定する。
- Direct-to-mainは維持可能だが、phase-wide giant commitは禁止する。
- Phaseの完了判定は、設計上のexit条件と実際の試験結果を通常の開発会話・Git履歴で確認する。専用のphase exit report/receiptは作成しない。

### 9.4 Workstream C: Development path simplification

- 通常source buildからexact Git revision/tree必須を外す。
- Package/release時だけprovenanceを必須化する。
- Static ICU proofやexact external runtimeを、該当materializer targetをbuildする場合だけ要求する。
- Core/query-only buildを独立presetにする。
- Fast test laneを作る。
- Unsupported platformはcore configure全体を失敗させず、該当capabilityをstructured unavailableにする。

### 9.5 Workstream D: Store profile separation

Phase 1ではSQLite safety codeを即削除しない。次へ分類する。

| Profile | 目的 | 既定 |
|---|---|---|
| Memory Reference | semantic correctnessとtest oracle | 開発既定候補 |
| SQLite Standard | 一般利用の永続store | 製品既定候補 |
| SQLite Hardened | adversarial/crash-sensitive環境 | optional/periodic qualification |

各VFS/WAL/SHM contractを、一般的data integrity、特定threat model、speculative counterexample、test-onlyへ分類する。StandardでSQLite自身のtransaction/recoveryへ委ねられるものは独自state machineを外す。Hardenedに必要なものは明示profileへ残す。Profile分離後も、immutable publication、failed publication isolation、semantic digest、migration incompatibility、prior snapshot preservation等のaccepted semantic invariantは共通に維持する。

### 9.6 Workstream E: Provider profile separation

- Local trusted provider
- Verified binary provider
- Sandboxed/hardened provider

を分け、developer modeでsignature/certification/full sandboxを必須にしない。ただしprovider identity、semantic contract、structured failureは維持する。Local trusted結果をVerified/Hardenedへ自動昇格せず、untrusted providerをLocal trusted経路へsilent fallbackしない。

### 9.7 Exit gate

- 一概念につきhand-maintained normative authorityが1個
- 通常buildがGit checkout、Clang materializer、static ICUへ不要に依存しない
- small internal changeのrequired artifact fan-outが明確に減少
- risk tier別test policyが運用可能
- Golden Journeysにsemantic regressionなし
- 削除/降格したcontractについて、known first-party/declared downstream consumerが存在しないかmigration済みで、観測限界が記録済み
- Completed scopeに期限なしのdual authorityがない
- Accepted stable axisの`CH-3`変更が混入していない。必要なものは別提案へ分離
- Phase 2A target architecture詳細設計が完成

本プログラムの実施対象はcxxlens repository内に限定する。auto-aha、cxxmonster等の既知downstreamはread-only censusで旧surface依存を確認するだけで、Phase 1では変更しない。このため、cxxlens内の条件がすべて成立しても、旧 `cxxlensProviderSDK` / `cxxlens::provider_sdk` 等へ依存するdownstreamが残る間は、上記gateの「consumer migration済み」を満たさない。未達をwaiveしてPhase 2A production実装へ進まない。downstream移行が別途許可された場合にのみ、consumer migrationとinstalled package回帰を追加してgateを再判定する。

---

## 10. Phase 2A — リファクタリングとアーキテクチャ再設計

### 10.1 目的

責務境界を再構成し、Clang materialization・Store・Provider Runtimeへ集中した複雑性を、compiler-neutralなcoreと明示的profileへ分離する。

### 10.2 Migration wave

#### Wave 1: Dependency graph and package boundaries

- conceptual target DAGをCMakeへ反映
- compiler-specific dependencyがcoreへ逆流していないことをfitness test化
- micro-targetのownership理由を監査
- public packageをcore、author/provider、frontend-nativeに整理

#### Wave 2: Generic build capture

現行Clang requestのproject/toolchain/variant/invocation/source部分をcompiler-neutral modelへ抽出する。

```text
Original invocation
Raw capture
  → normalized semantic option model
  → effective replay invocation
  → toolchain_context / build_variant / compile_unit
```

Original argvは監査用evidenceとして必要な場合に保持するが、secret redactionとretention profileを適用し、defaultでは全environmentを保存しない。Semantic identityは明示したcanonical fieldsで作る。Unknown option、読み取れないresponse file、失われたenvironment、redacted fieldはcapture coverage/unresolvedへ残す。

#### Wave 3: Generic materialization task

- Clang固有tool/worker名をgeneric task envelopeから外す。
- requested relation、input bindings、condition、interpretation、budget、source closureをgeneric化する。
- Frontend-specific extensionはversioned capabilityへ閉じる。
- Store publication authorityとfrontend worker ingressを分離する。

#### Wave 4: Semantic Store unification

- v5/v6、candidate/report、memory/sqliteの重複責務をinventoryする。
- 一つのsemantic writer lifecycleへ統合する。
- Physical backendはsemantic manifestを再計算・検証するが、semantic modelを再実装しない。
- Standard/Hardened SQLiteを共通port上へ置く。

#### Wave 5: Provider runtime decomposition

- protocol codec
- lifecycle state machine
- process port
- selection/trust policy
- result adoption

を分離し、同じvalidationをSDK harnessとproduction runtimeで共有する。

#### Wave 6: Query execution decomposition

- IR decode/validation
- logical operator execution
- physical scan/join
- guarantee/result composition

を分ける。Typed/dynamic parityとresult side channelは維持する。

### 10.3 Compatibility strategy

- Admitted public surfaceはcompatibility facadeを提供するか、明示major migrationとする。Defaultは`CH-0`〜`CH-2`であり、major migrationは独立した`CH-3`提案とする。
- Accidental/unadmitted surfaceにはshimを追加しない。ただしclassification根拠とknown consumer censusを残す。
- Compatibility facadeには対象consumer、開始version、removal条件、最長存続Phaseを定め、恒久的な二重実装にしない。
- Existing snapshot/schemaを読める期間とmigration pathをPhase 2A specで決める。
- New internal architectureを理由にsemantic IDを変えない。
- ID変更が必要な場合は別version axisとmigrationを用意し、本プログラムへ暗黙に混入させない。
- Old/new pathのdual-runでは一方をprimary authorityとし、divergenceを記録してfailする。first-winsで差を隠さない。

### 10.4 Exit gate

- Target dependency directionをCIで検査
- Core/query/store public surfaceにClang/GCC/MSVC native型なし
- Generic capture/taskで現行Clang Golden Journeyを実行
- Semantic writer pathが一つ
- Memory/SQLite parity維持
- Completed scopeの旧materialization path consumer移行完了。残存facadeには期限とremoval条件がある
- Completed scopeにprimary不明のdual authority、silent fallback、未判定divergenceがない
- 巨大shadow checkerの主要semanticsがshared validator/testへ移行
- Phase 2B hardening対象boundaryが固定

---

## 11. Phase 2B — 重点Hardening

### 11.1 目的

再設計後に残った安定boundaryだけを、risk tierに応じてhardeningする。削除予定の内部構造を堅牢化しない。

### 11.2 対象

- Wire/JSON/binary decode
- External provider process
- Filesystem/source closure
- Persistent store formatとmigration
- Compiler-native lifetime
- Installed package/public header
- Source mutation/artifact publication

### 11.3 原則

- Threat modelを先に書く。
- Fault matrixはR3 boundaryだけ。
- Resource boundは外部入力とuntrusted growthへ集中。
- Same validatorをruntime/testで共有。
- Mutation smoke/fuzzはcodec単位で集約する。
- Rare platform-specific testsはperiodic/release laneへ移す。
- Hardened固有の環境・threat test failureがStandard profileの開発を不必要にblockしない。ただしshared codec、publication、migration、data integrityのinvariant failureは全profileをblockする。

### 11.4 Exit gate

- R3 boundaryごとにowner、threat、invariant、test laneが明示
- Public/installed headersが独立compile
- Protocol decodeがbounded、deterministic、exception-safe
- Provider crashがprior snapshotを破壊しない
- Store corruption/migrationがfail-closed
- Native pointer escapeが構造上不可能
- Standard development pathの負担がPhase 1 budget内

---

## 12. Phase 3 — GCC/MSVC製アプリケーション解析

### 12.1 Goalと非Goal

**Goal:** GCCまたはMSVCでproduction buildされるC/C++アプリケーションについて、元build contextを再現可能にcaptureし、意味relationを精度ラベル付きで生成・queryできるようにする。

**非Goal:**

- GCC/MSVCでcxxlens本体を正式buildすること
- 全GCC extension・全MSVC extensionの即時完全対応
- 全relationを最初からnative exactにすること
- MSVC非公開内部APIへ製品依存すること

### 12.2 Support axes

単一の`compiler_provider_major`でsupportを表現せず、次へ分離する。

| Axis | 例 |
|---|---|
| Host OS | Linux / Windows |
| Project build system | CMake/Ninja / Make / MSBuild |
| Production compiler | GCC 15 / MSVC 19.xx |
| Capture adapter | compile DB / wrapper / Build Insights |
| Semantic frontend | Clang GCC-mode / clang-cl / GCC native |
| Target ABI | Itanium C++ ABI / MSVC ABI / target triple |
| Store profile | memory / sqlite-standard / hardened |
| Fidelity | relation別 exact/under/over/unknown + modalities |

### 12.3 Phase 3.0 — Compiler-neutral model拡張

現行`build.toolchain_context`はfamily、exact version、target triple、builtin headers、sysroot、ABI、plugin/spec digestを既に持つ。この資産を活用し、次を追加または明確化する。

- production compiler identity
- analysis frontend identity
- driver mode
- target architecture/ABI
- include search and builtin macro census
- language standard/extensions
- PCH/module/header unit inputs
- response/config files
- environment allowlist
- unsupported/passthrough option accounting
- source encoding/path semantics

Production compilerとanalysis providerを同一IDへ混ぜない。

#### Capture evidence handling

- 各fieldを`observed / derived / redacted / unavailable`で区別し、capture completenessを一括boolにしない。
- Command line、response/config file、environmentはallowlistで収集し、credential、token、password、private key、proxy secret等を永続化・log出力しない。
- Redactionによりsemantic replayへ影響する場合、そのfieldとscopeをunresolvedにし、secret値をplaceholderで推測しない。
- Machine-local absolute rootは監査evidenceとして別管理し、semantic identityへ直接混ぜない。
- `capture-exact`は取得したoriginal artifactの内容に対する完全性を意味し、compiler semantic relationの`exact`とは別に評価する。

### 12.4 Phase 3.1 — GCC capture + Clang replay

#### Capture

- `compile_commands.json`またはbuild wrapperからGCC invocationを取得する。Compilation database単独ではenvironment、generated response file、wrapper side effectまで完全とは限らないため、`original complete`を自動宣言しない
- response file、working directory、environment allowlist、target、sysroot、include path、macrosをcaptureし、取得不能部分はfield-level unresolvedにする
- original compiler versionとbuiltin header identityを取得
- source closureをoriginal buildまたはdependency outputから取得

#### Replay

- ClangのGCC-compatible driverでeffective invocationを構成
- GCC optionを分類する
  - exact mapping
  - semantics-preserving mapping
  - approximation mapping
  - unsupported
  - output-only/nonsemantic
- Unknown/unused optionをsilent ignoreしない
- Parse failureやextension gapをunresolvedへ保存

ClangはGCC compatibilityを重視するが、未実装・意図的非対応のGCC extensionも存在する。したがって「Clangでparseできた」だけでGCC exactを名乗らない。

#### Initial fidelity

- Build invocation/toolchain/source digest: `capture-exact`（取得artifactと宣言scope内）
- Captured dependency edge: `capture-exact`（original compiler outputのscope内）
- Clang AST declaration/call/type: replayed guarantee
- GCC-specific extensionを含むscope: partial/unknown
- Absence/exhaustiveness: closure条件を満たす場合だけexact

### 12.5 Phase 3.2 — MSVC capture + clang-cl replay

#### Native build evidence

- MSBuild/binlogまたはC++ Build Insightsからcompiler invocation、tool path/version、working directory、input/outputを取得する。各sourceが提供しないenvironmentやsemantic detailは補完推測せずunresolvedにする
- `/sourceDependencies`からdirect/indirect header、PCH、module、header unit依存を取得
- `/scanDependencies`等のstandard module dependency outputを必要に応じて利用
- Visual Studio toolset、Windows SDK、target architecture、runtime library、language mode、PCH/module設定をcapture

#### clang-cl replay

- Visual Studio Native Tools environmentまたは明示されたtoolchain environmentで実行
- MSVC compatibility version、target ABI、Windows SDK/includeを固定
- `-Werror=unknown-argument`等で未知optionをsilent ignoreさせない
- cl.exe固有pragma/declspec/intrinsicのunsupportedをrelation scopeへ反映
- Delayed template parsing等のmodeをproduction compiler contextに合わせる

#### Initial fidelity

- Invocation、tool version、working directory、source dependency: `capture-exact`（original evidenceの宣言scope内）
- clang-clでのAST relation: replayed/qualified
- MSVC-specific ABI/layout/intrinsic: native evidenceがなければunknownまたは限定approximation
- Link/object/PDBから直接観測できるfact: source semanticsと混同せず別relation/domainでexact

### 12.6 Phase 3.3 — GCC native provider

GCC replayの精度を測定後、次の条件を満たす場合に実装する。

- 高価値relationでreplay disagreementが実用上無視できない
- GCC plugin APIから安定して抽出可能
- Major別package保守コストが受容可能
- GPL-compatible plugin要件と配布形態を確認済み
- Downstream実装よりcore公式providerとして吸収する価値がある

構成例:

```text
cxxlens-gcc-worker-<major>
  GCC plugin / compiler callback
      → detached provider-local observations
      → generic provider protocol
      → canonical relation qualification
```

GCC pluginはcompiler eventへcallbackを登録できる一方、plugin headerとGCC versionへの結合が強い。したがってmajor-specific workerとし、coreへGCC tree/GIMPLE型を漏らさない。

最初から全relationを実装しない。次の高価値subsetを候補とする。

- declaration/entity identity
- direct call target
- type/layout/ABI fact
- macro/source origin
- CFGまたはoptimized GIMPLE relation（必要なuse caseのみ）

### 12.7 Phase 3R — MSVC exact research gate

MSVC exact semantic providerは、実装作業ではなく期限を区切った research work として扱う。Timeboxの期間、調査対象、成果物、No-Go条件はPhase 3 detailed designで先に固定し、追加調査を理由にfeature freezeを延長し続けない。

調査対象:

- 公開・supportされたcompiler event/trace
- Build Insights SDKのsemantic粒度
- source dependency/module outputs
- object/PDB/browse information
- Visual Studio language service/extensibilityの利用条件
- third-party/downstream implementationのライセンスと安定性

Go条件:

- 公開・再配布可能・version管理可能な手段がある
- relation subsetとscopeを明示できる
- compiler updateに対するconformance戦略がある
- private ABI reverse engineeringを製品必須にしない

No-Goの場合:

- MSVC native build evidence + clang-cl replayを正式解とする
- native exactがないrelationは保証を正直に下げる
- 研究コードをproduction branchへ残さない

### 12.8 Downstream provider吸収基準

| 条件 | 必須 |
|---|---|
| Native型がprovider境界を越えない | Yes |
| Canonical relationまたはprovider-local relationが明示 | Yes |
| Interpretation domainとguaranteeが明示 | Yes |
| Conformance corpusを通過 | Yes |
| Compiler/version support policy | Yes |
| Licenseと再配布条件 | Yes |
| Maintenance owner | Yes |
| Unsupportedをemptyにしない | Yes |

条件を満たさない実装はexternal providerのまま利用できるようにし、coreへ無理に吸収しない。

### 12.9 Phase 3 Exit gate

- GCC-built corpusをcaptureし、Clang replayでGolden queryを実行
- MSVC-built corpusをcaptureし、clang-cl replayでGolden queryを実行
- Original compiler evidenceとreplay claimが別domain/provenanceで保持
- Unknown option/extension、欠落environment、redacted replay inputがsilent omissionにならない
- Capture artifactとlogに対するsecret/credential検査が成功
- Capture fidelityとsemantic fidelityが別々に公開される
- Relationごとのsupport/fidelity matrixが公開
- Strict policyでexact/qualified claimだけを選択可能
- GCC native providerのGo/No-Go判断完了。Goならinitial subset実装済み
- MSVC exact researchのGo/No-Go判断完了
- Host compiler portabilityを実施していなくても解析goalを満たす

---

## 13. 精度・厳密性設計

### 13.1 「全部かもね」を避ける原則

近似frontendを使用しても、全結果へ一括で曖昧な但し書きを付けない。保証は最小の意味単位へ付ける。

```text
Snapshot
  └─ Relation partition
       └─ Claim / row
            ├─ condition fragment
            ├─ interpretation domain
            ├─ approximation
            ├─ verification modalities
            ├─ assumptions
            └─ provenance
```

### 13.2 既存guarantee modelの利用

既存モデルはapproximationを二軸で表現している。

| 値 | Positive soundness | Scope completeness | 利用例 |
|---|---:|---:|---|
| `exact` | Yes | Yes | native exact、equivalenceが証明されたreplay |
| `under_approximation` | Yes | No | 見つけたpositiveは信用できるが漏れ得る |
| `over_approximation` | No | Yes | 対象を覆うがfalse positiveを含み得る |
| `unknown` | No claim | No claim | 近似関係を主張できない |

これは「confidence 60%」より扱いやすい。特にbug findingでは、`under_approximation`のpositiveを、宣言済みscope・interpretation・assumption内でsoundなpositiveとして提示しながら、漏れだけを明示できる。`exact`を名乗るには、relation/version、scope、condition partition、interpretation、assumption、coverage、必要closure、overlapping conflict/unresolvedの条件をすべて満たす。

### 13.3 Verification modalities

既存の`frontend_replayed`、`compiler_verified`、`link_verified`、`runtime_observed`、`differentially_verified`を基本とする。Phase 3詳細設計で、native build evidence用のmodality追加が必要かを判断する。

重要なのは、modalitiesを単一の強弱順へ並べないことである。例えばruntime observationとcompiler replayは別種類の根拠であり、一方が常に強いわけではない。

### 13.4 Consumer policy

Consumerは最低保証policyをrelationごとに選べる。異なるmodalityやinterpretationを単一の全順序へ潰さず、policyが受理したclaimと除外したscopeをresultへ残す。

| Policy | 採用するclaim | 用途 |
|---|---|---|
| Native strict | native exactのみ | 高リスク変更、absence証明 |
| Exact or cross-verified | exactまたはrelation-specific equivalence確認済み | CI gate |
| Sound positives | exact + under-approx positive | bug探索 |
| Exploratory | over/unknownを含む | 調査、候補生成 |

Filterでclaimを落とした場合、result completenessを自動的にexactへ格上げしない。除外したscopeはunresolved/coverageに残す。

### 13.5 Relation-level support matrix例

| Relation | GCC replay | GCC native | MSVC evidence | clang-cl replay |
|---|---|---|---|---|
| build.compile_unit | exact capture | exact | exact capture | exact capture |
| source dependency | original outputならexact | exact | `/sourceDependencies` scopeでexact | replayed |
| declaration/entity | qualified replay | native exact候補 | 未提供 | qualified replay |
| direct call target | qualified/under | native exact候補 | 未提供 | qualified/under |
| type/layout/ABI | target依存、しばしばpartial | native exact候補 | object/debug evidence候補 | target依存 |
| macro/source origin | replayed | native exact候補 | dependency/preprocess evidence | replayed |
| absence/exhaustive target | closure次第 | closure次第 | 通常unknown | closure次第 |

実際の表はfixtureとdifferential testでrelationごとに確定する。表中の`exact capture`はoriginal evidenceの取得scopeに対するcapture fidelityであり、AST、ABI、call target等のsemantic exactnessへ自動伝播しない。

### 13.6 Differential qualification

同じsource/build variantについて、native providerとreplay providerを比較する。

```text
GCC native claim
        ↕ differential comparison
Clang GCC-mode replay claim
```

Agreementは自動的に普遍的exactを意味しない。Compiler version、language feature、target、relation scopeを含むqualification tupleへbindする。

---

## 14. Feature Freeze中のリスク管理

### 14.1 主なリスク

Featureを作らない期間が長いほど、実用性から離れたarchitectureを作る危険がある。

### 14.2 Mitigation

#### Golden Journeysを設計reviewerにする

各architecture判断は、少なくとも一つのGolden Journeyを簡単にするか、必要な安全性を説明できなければ採用しない。

#### Capability preservation matrix

各Phaseで次を更新する。

- Existing capability
- Current public entry
- New entry
- Parity status
- Intentional removal reason

#### Real-project replay

Toy fixtureだけでなく、代表projectのsource closure、materialization、queryを定期実行する。

#### Feature Escrow

新機能案を捨てず、一行backlogとして保存する。ただしPhase 4まで設計・issue分割しない。

#### Freeze exit条件を固定

「もっと綺麗にできる」を理由にPhaseを延長しない。各Phase開始時にaccepted scopeと試験・resource envelopeを固定し、途中で見つかった非critical cleanupはbacklogへ送る。Exit gateを満たしたら次へ進む。

### 14.3 Freeze中の変更判定

```text
この変更は現在の能力を保全・単純化・移行するか？
  Yes → 実施候補
  No
   └─ GCC/MSVC解析対応に必要か？
        Yes → Phase 3 scope
        No  → Feature Escrow
```

---

## 15. Phase 4 — 機能開発再開

### 15.1 再開条件

- Phase 3 exit gate完了
- Target architecture上のextension pathが実証済み
- Ordinary featureで必要な文書/test burdenがrisk tier相応
- Feature backlogを現製品goalで再評価可能

### 15.2 開発方式

各機能はend-to-end vertical sliceとして追加する。

```text
Consumer question
  → required input capture
  → relation/provider
  → query/analysis
  → result guarantee
  → runnable example / real fixture
```

API数、relation数、checker数を成果にしない。

### 15.3 優先候補

現行READMEの方向性を再評価し、次から実用価値の高いものを選ぶ。

- semantic graph
- CFG/control exit
- use-def/value flow
- alias/effect/invalidation
- interprocedural summary/model pack
- proof-carrying rewrite/artifact
- cross-provider semantic consensus

Phase 0のFeature EscrowとPhase 3で得たtoolchain supportを踏まえて順位を決める。

---

## 16. Migration・Compatibility・Versioning

### 16.1 Version axisを混同しない

内部architecture変更、distribution release、public source API、relation major、query IR、provider protocol、store formatは別axisである。

- Internal refactorだけでdistribution majorを上げない。
- Public admitted API破壊はsource API majorで扱う。
- Relation identity/meaning変更はrelation major。
- Wire incompatibleはprotocol major。
- Persisted incompatibleはstore format migration。

### 16.2 Stable / Experimental / Internal

| Stability | Promise |
|---|---|
| Stable | documented compatibilityとmigration |
| Experimental | release内で変更可能、明示opt-in |
| Internal | compatibilityなし、install/exportしない |

新しいGCC/MSVC providerはExperimentalから開始し、relation-specific conformanceが揃ったsubsetだけStableへ昇格する。

### 16.3 Existing public API

Public API Catalogにadmittedされたsurfaceは、次のいずれかを選ぶ。

- そのまま維持
- New coreへのthin facade
- Deprecation期間とmigration guide
- Major変更

Catalogにないaccidental surfaceは、hardening branchの`snapshot_builder`と同様、原則shimを作らない。

### 16.4 Persisted data

- Baseline snapshot/store formatのread support期間を明示する。
- Migrationはdeterministicで、semantic digest parityを検査する。
- Migration不能なartifactはstructured incompatibilityを返す。
- Silent fallback、古いsnapshotへの自動退避を行わない。

### 16.5 Compatibility envelope

- Stable surfaceの`CH-3`変更は、本プログラムのcleanup/refactorへ内包しない。
- Experimental surfaceは変更可能だが、support matrix、release note、consumer fixtureで境界を明示する。
- Internal surfaceは自由に変更できるが、install/exportへ漏れていないことを検査する。
- 一つの変更が複数version axisへ影響する場合、各axisを個別に分類する。Internal architecture変更を理由にrelation、wire、store、source APIのmajorを連動させない。
- Deprecationはwarningを出すこと自体を成果にせず、replacement、migration example、removal条件、最長存続期間を持つ。Accepted stable surfaceの実削除は、既存contractが明示的に期限付きでない限り`CH-3`として別判断する。

---

## 17. Metrics・Acceptance・Phase Gate

### 17.1 Developer friction metrics

- Ordinary changeで手動更新するauthority数
- Affected files/targets数
- Configure prerequisites
- Local affected-test turnaround
- Main deterministic suite turnaround
- New provider/relationを追加するcore source diff

### 17.2 Architecture metrics

- Dependency cycle
- Coreからcompiler-native includeへのedge
- Deployment理由のないinternal target
- Duplicate codec/validator
- Public header closure
- Hotspot fileのresponsibility count

LOCやfile sizeだけで合否を決めない。

### 17.3 Product behavior metrics

- Golden Journey pass rate
- Memory/SQLite semantic parity
- Unknown reasonのactionability
- Conflict/differential preservation
- Root/jobs/order/backend determinism
- Prior snapshot failure isolation

### 17.4 Multi-toolchain metrics

- Compile unit capture success
- Replay parse success
- Unsupported option rate
- Relation coverage by fidelity
- Native/replay disagreement rate
- Exact absenceを主張できるscope
- Target/ABI/PCH/module fixture coverage

### 17.5 Verification mapping

Phase 0で、各exit criterionを変更固有試験、決定的CTest、installed consumer、または設計レビューのいずれかへ対応付ける。対象lane、scope、未実行の試験、unsupported/environment unavailableを明示するが、閾値を満たしたことを示すQuality Budgetや、実行ごとのreportを保存しない。

Duration、resource、fan-outは将来の改善判断に役立つ診断情報であり、単独でproduct completionを証明しない。unsupported caseを分母から隠して見かけ上の改善を作らず、測定不能はpassではなく`unavailable`として扱う。

### 17.7 Gate adjudication

- 各exit criterionは`pass / fail / not-applicable / accepted-risk`のいずれかとし、根拠となるtest、設計、または公開された設定を一つ以上示す。
- Product Constitution、accepted stable contract、Critical riskに関わるcriterionは`accepted-risk`へ落とせない。
- Criterionが不要または誤りと判明した場合、gate判定前にPhase designを改訂する。実装完了後の後付け免除は行わない。
- `not-applicable`にはscope根拠を要求し、単なる未実装をN/Aにしない。
- Accepted riskは影響、owner、次の判定点を通常の設計・Issue・Git履歴へ記載する。別のreceipt体系は新設しない。

---

## 18. Risk Register

| ID | Risk | Impact | Mitigation | Trigger/Decision |
|---|---|---|---|---|
| R-01 | Feature freeze中に実用性から乖離 | High | Golden Journeys、real corpus、exit gate | Journeyが新設計で不自然なら設計を戻す |
| R-02 | 簡素化でunknown/closure等の核を削る | Critical | Product Constitution、semantic parity | 憲法違反はmerge不可 |
| R-03 | Phase 1が終わらないcleanupになる | High | retirement matrixとexit criterion | 新規削減案は次backlogへ |
| R-04 | Phase 2がbig-bang rewriteになる | Critical | strangler migration、main green | parityなし旧経路削除禁止 |
| R-05 | SQLite hardening削減でdata integrity低下 | Critical | Standard/Hardened profile、threat classification | 必要threatをStandardが満たせなければretain |
| R-06 | GCC/MSVC replayを誤ってexact表示 | Critical | interpretation/guarantee分離、strict policy | relation qualificationなしexact禁止 |
| R-07 | clang-clが未知MSVC optionを無視 | High | unknown argumentをerror化、option census | unsupportedはunresolved |
| R-08 | GCC plugin version coupling | High | major-specific worker、conformance | maintenance cost超過ならoptional external |
| R-09 | GCC plugin license/redistribution | High | 実装前legal/license review | 不適合ならofficial distributionしない |
| R-10 | MSVC exact APIが存在しない/不安定 | Medium | research gate、hybrid正式解 | No-Goを許容 |
| R-11 | Support matrix組合せ爆発 | High | support axes分離、relation-level capability | 一括「MSVC supported」を禁止 |
| R-12 | Downstream吸収でcoreが肥大化 | High | provider conformance/owner/license gate | 条件未達はexternal維持 |
| R-13 | Issue/Spec儀式が再増殖 | High | minimal template、small change no issue | artifact fan-out metric悪化でpolicy修正 |
| R-14 | Long-lived branchがmainと乖離 | High | current main、small commits、必要時のみ短命PR | phase-wide branch禁止 |
| R-15 | Compatibility維持が再設計を阻害 | Medium | admitted/accidental区別、facade期限 | explicit major migrationを選択可能 |
| R-16 | Heavy tests削減でregression増加 | High | lane移動、periodic suite、Golden corpus | flake/escape増加ならlane再調整 |
| R-17 | Baselineがred/ambiguousなまま比較基準化 | High | current mainのfresh buildとfailure分類 | 未分類failureがあればPhase 0 exit不可 |
| R-18 | Authority移行中にold/new semanticsが分岐 | Critical | primary指定、dual-run parity、期限付きretirement | divergenceはfail、first-wins禁止 |
| R-19 | Build command/environmentからsecret漏洩 | Critical | allowlist、redaction、retention profile、secret scan | secret検出時はpublication/cacheを拒否 |
| R-20 | Gateが完璧主義または後付けwaiverで形骸化 | High | measurement contract、scope固定、spec改訂手順 | 非critical追加cleanupはbacklog、後付け免除禁止 |
| R-21 | Metric改善のためtest/corpus/scopeを除外 | High | denominator固定、Golden comparator、N/A根拠 | scope縮小時はmetric再baselineとreview |
| R-22 | Capture fidelityとsemantic fidelityの混同 | Critical | `capture-exact`を別表示、relation単位qualification | capture exactだけでsemantic exact昇格禁止 |

---

## 19. やらないこと・禁止事項

1. 全面書き直し。
2. 既存意味論を「複雑だから」という理由だけで削除。
3. Clang replayをGCC/MSVC native exactと呼ぶ。
4. GCC、MSVC、Windows、static/shared、全backendを一度にformal support。
5. 全downstream providerをcoreへ統合。
6. MSVC private compiler ABIをproduction必須にする。
7. 新architectureが固まる前に追加hardeningを無制限に行う。
8. 全変更へADR、Issue、fault matrixを要求する。
9. Checkerでsource file名・target名・実装順序を製品semanticsとして固定する。
10. CIが遅いという理由だけでtestを消す。まずlaneを移す。
11. LOC削減率をprogram successとする。
12. Feature freezeを「永久に綺麗にする期間」へ変える。
13. `exact`をscope、interpretation、assumption、coverage/closureなしに普遍的なcompiler同値として表示する。
14. Temporary dual authority、compatibility facade、old/new pathをprimary・parity・retirement期限なしで残す。
15. Raw environment、credential、token、secretをcapture artifact、diagnostic、cache、Golden fixtureへ保存する。
16. Test/corpus/denominatorを狭めてmetricやgateを見かけ上greenにする。

---

## 20. Phase別文書と作業単位

### 20.1 推奨repository layout

```text
docs/reform/
  README.md
  phase-0/
    product_constitution.md
    golden_journeys.md
  phase-1/
    simplification_design.md
    authority_retirement_matrix.md    # 必要な領域だけ追加
  phase-2/
    target_architecture.md
    dependency_and_package_design.md
    migration_map.md
    compatibility_and_dual_run_plan.md
    targeted_hardening_policy.md
  phase-3/
    multitoolchain_analysis_design.md
    gcc_capture_replay.md
    msvc_capture_replay.md
    gcc_native_provider_feasibility.md
    msvc_exact_research.md
    fidelity_support_matrix.md
  phase-4/
    feature_restart_policy.md
    prioritized_capability_roadmap.md
```

Phase 0/1の分類と設計は、上記の文書と既存のmachine contractへ記載する。inventoryやauthority retirement matrixが大きくなる場合でも、運用専用の保存artifact、実行receipt、quality reportへ変換しない。

### 20.2 Phase workstreams

| Workstream | Outcome |
|---|---|
| Phase 0 Product Constitution & Verification Baseline | 守るものと削れるものを確定 |
| Phase 1 Simplified Product Governance | authority、build、test負担を軽量化 |
| Phase 2A Compiler-neutral Architecture | target architectureへ移行 |
| Phase 2B Targeted Hardening | stable boundaryを必要強度へhardening |
| Phase 3 GCC/MSVC Application Analysis | multi-toolchain対象解析を正式化 |
| Phase 4 Feature Development Restart | vertical sliceで通常開発再開 |

### 20.3 作業項目の作成方針

1. Overall design、Product Constitution、Phase 1 designをレビューする。
2. 独立したconsumer migration、R3変更、または並行作業が必要な場合だけ作業Issueを作る。
3. 実装が進んでいない将来Phaseの子Issueを大量生成しない。
4. Issueの存在やcloseをPhase gateの根拠にしない。根拠は設計と実際の試験結果にする。

### 20.4 各Phaseの作業単位

- 一つのauthority consolidation
- 一つのpackage boundary移行
- 一つのconsumer path移行
- 一つのbackend/profile分離
- 一つのcapture/provider vertical slice

「ファイルを10個直す」のような実装手段ではなく、観察可能なoutcomeで切る。

---

## 21. 未決事項と推奨default

本書を開始するうえでblocking questionはない。次の事項はPhase 0/3の正式判断対象とし、それまでは推奨defaultを使う。

| 未決事項 | 推奨default |
|---|---|
| SQLite Hardenedを同repoに残すか | Phase 2までは同repoのoptional profileとして保持。価値・保守費用測定後に分離判断。 |
| Public API 2.0.0のcompatibility期間 | 現行の `cxxlens::sdk` を単一の正規surfaceとする。retired package/targetのv1 compatibility shimは再導入しない。将来のdeprecationは対象consumer・期限・移行先を別途定める。 |
| Initial GCC/MSVC relation subset | build/source/entity/declaration/direct-call/typeの順。absence/CFGは後続。 |
| GCC native providerを実装するか | Replay differentialで高価値gapが確認された場合はGo。 |
| MSVC exact providerを実装するか | 公開stable extraction pathが確認できた場合だけGo。 |
| Feature freeze中の新規利用者要求 | Feature Escrowへ記録。security/critical regression以外はPhase 4まで延期。 |
| direct-to-mainかPRか | Solo/agent作業はsmall direct-to-main可。高リスクR3と外部reviewが有益な変更のみPR推奨。 |
| Distribution version | Internal phase完了だけでは上げない。public axis変更時に判断。 |
| Breaking contract change | Defaultは`CH-0`〜`CH-2`。`CH-3`は別提案・明示承認とし、refactorへ混入させない。 |
| Baseline failure | 現行mainをfreshに再構成し、環境不足・unsupported・product/test defect・flakeを分類する。独自snapshotやqualification reportをgate基準にしない。 |
| Raw argv/environment retention | Allowlistとredactionを既定とし、full environmentは保存しない。Semantic replayに必要な欠落はunresolvedにする。 |
| Phase gate criterionが不適切だった場合 | Gate判定前にPhase designを改訂する。Issue closeによる後付けwaiverはしない。 |
| Temporary dual authority | Primary、parity、divergence policy、retirement条件、最長存続Phaseを必須とする。 |

---

## 22. 最終受入条件

本プログラムは、次をすべて満たしたとき完了する。

1. 製品憲法が少数・明確で、簡素化後も機械的に守られる。
2. 一つのsemanticsを複数の手書きauthorityへ同期しない。
3. Core、capture、provider、frontend、store、query、analysisの責務が分離される。
4. Clang materialization固有構造がgeneric coreを支配しない。
5. 普通の開発経路が軽く、hardening費用はrisk/profileに応じて支払う。
6. Golden Journeysとreal corpusが全Phaseを通して維持される。
7. GCC/MSVC製projectのbuild contextをnativeにcaptureできる。
8. GCC-compatible/clang-cl replayをsilent omissionなしで実行できる。
9. Exactなclaimとapproximateなclaimが混在しても、個別に保証を判断できる。
10. GCC native providerとMSVC exact researchについて、根拠あるGo/No-Go判断が完了する。
11. Phase 4で、新しい実用機能を過剰な儀式なしにvertical sliceとして追加できる。
12. Accepted stable contractの`CH-3`変更がcleanup/refactorの副作用として混入せず、必要な変更は独立したmigration判断として扱われる。
13. Completed scopeにprimary不明・期限なしのdual authority、silent fallback、未判定old/new divergenceが残らない。
14. Build captureがsecretを永続化せず、capture fidelityとsemantic fidelityをconsumerが別々に判定できる。

---

## 付録A. 現行source mapと調査根拠

### A.1 Repository sources

- [README](README.md)
- [Development architecture](docs/development/architecture.md)
- [Next-generation integrated design](docs/design/cxxlens_next_generation_integrated_design_ja.md)
- [ADR index](docs/design/adr/README.md)
- [Catalog/registry index](docs/design/catalogs/README.md)
- [AGENTS](AGENTS.md)
- [CONTRIBUTING](CONTRIBUTING.md)
- [Build and test](docs/development/build-and-test.md)
- [Root CMake](CMakeLists.txt)
- [Quality workflow](.github/workflows/quality.yml)
- [Release workflow](.github/workflows/release.yml)
- [Public API catalog](schemas/cxxlens_ng_public_api_catalog.yaml)
- [Relation Registry](schemas/cxxlens_ng_relation_registry.yaml)
- [Semantic Guarantee Contract](schemas/cxxlens_ng_semantic_guarantee_contract.yaml)
- [Snapshot Store public header](include/cxxlens/sdk/store.hpp)
- [Query public header](include/cxxlens/sdk/query.hpp)
- [Claim public header](include/cxxlens/sdk/claim.hpp)
- [Clang 22 native SDK](include/cxxlens/provider/clang22.hpp)
- [Clang materialization request](schemas/cxxlens_ng_clang22_materialization_request_v2_2.schema.yaml)
- [Source closure transport](schemas/cxxlens_ng_source_closure_transport.yaml)
- [SQLite Store contract](schemas/cxxlens_ng_sqlite_store_contract.yaml)
- [SDK tutorials](docs/tutorials/README.md)

### A.2 External primary sources

- [Clang Compiler User’s Manual — GCC/MSVC compatibility and clang-cl](https://clang.llvm.org/docs/UsersManual.html)
- [Clang Driver Design — GCC-compatible driver goals](https://clang.llvm.org/docs/DriverInternals.html)
- [GCC Plugin API](https://gcc.gnu.org/onlinedocs/gcc-15.2.0/gccint/Plugin-API.html)
- [GCC Plugins building](https://gcc.gnu.org/onlinedocs/gccint/Plugins-building.html)
- [MSVC `/sourceDependencies`](https://learn.microsoft.com/en-us/cpp/build/reference/sourcedependencies?view=msvc-170)
- [MSVC `/scanDependencies`](https://learn.microsoft.com/en-us/cpp/build/reference/scandependencies?view=msvc-170)
- [C++ Build Insights overview](https://learn.microsoft.com/en-us/cpp/build-insights/get-started-with-cpp-build-insights?view=msvc-170)
- [C++ Build Insights `Invocation` class](https://learn.microsoft.com/en-us/cpp/build-insights/reference/sdk/cpp-event-data-types/invocation?view=msvc-170)

---

## 付録B. 用語

| 用語 | 本書での意味 |
|---|---|
| Authority | Semanticsやcompatibilityの最終決定元 |
| Claim | Relation rowとcondition、interpretation、producer、basis、guaranteeを結合した意味主張 |
| Closure | 指定scopeでabsence/exhaustivenessを確定する根拠 |
| Capture | 元build system/compilerから実際の入力・環境・依存を取得すること |
| Replay | 別frontendでcapture済みbuild contextを再生すること |
| Native provider | 元compiler内部の公開機構から直接semantic observationを取得するprovider |
| Interpretation domain | どのsemantic authority/modelの下でclaimが成立するかを表すID |
| Golden Journey | 製品価値をend-to-endで保全する代表実行経路 |
| Feature Escrow | Freeze中の新機能案を詳細設計せず保留する単一backlog |
| Standard profile | 一般利用に必要な品質・安全性 |
| Hardened profile | 明示threat modelへ対応する追加保証 |
| Exit gate | 次Phaseへ進むための観察可能な完了条件 |
| `capture-exact` | Original build evidenceの宣言scope内で、取得したartifact/fieldが完全であること。Compiler semantic relationの`exact`とは別 |
| Semantic exact | Relation/version、scope、condition、interpretation、assumption、coverage/closure等のpreconditionを満たす意味保証 |
| Contract change class | `CH-0`〜`CH-3`でcompatibility impactを分類する本プログラム上の区分 |
| Golden comparator | Canonical semantic behaviorを比較し、timing・temporary path・diagnostic prose等の非意味差分を除外する比較規則 |
| Current verification baseline | 現行mainのfresh build/testで、環境不足・unsupported・product/test defect・flakeを分類した比較基準。独自tagやreportを意味しない |

---

## 付録C. Phase 0–1開始時の作業

1. 現行mainへ先行統合された変更を、freshなexact LLVM/Clang 22.1.0 static/shared buildとmain workflowで確認する。
2. 再現可能なfailureを環境不足、unsupported、product/test defect、flakeへ分類する。architecture変更と同時に直さない。
3. 本書と `docs/reform/` のProduct Constitution、Phase 1 simplification designをレビューする。
4. Public/authority/source/test censusをcxxlens repository内で実施し、観測可能なfirst-party/downstream scopeを明記する。
5. Golden JourneysをCTest labelとcanonical semantic comparatorで自動実行可能にする。
6. Contract Preservation Mapとauthority retirement分類を作る。運用専用report、receipt、checkpoint、checksum、tagは作らない。
7. `cxxlens::sdk`、薄い `cxxlens` CLI、OpenSSL Ed25519 port、SQLite/source-closure/providerの安全receiptを現行資産として保全対象にする。
8. Phase 1の各削減対象を `CH-0`〜`CH-2` に分類し、`CH-3` 候補を別提案へ分離する。
9. cxxlens外のknown downstreamはread-only censusに留め、auto-aha/cxxmonsterの旧surface依存が残る間はPhase 1全体gateを通過させない。

この順序では、いきなり契約・コードを削らない。**まず現状を再現し、守る能力を固定し、削除の根拠を得る。**
