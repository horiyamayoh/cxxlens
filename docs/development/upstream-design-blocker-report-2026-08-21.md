# cxxlens 上流設計ブロッカー報告

作成時刻: 2026-08-21T10:41:05Z
再確認時刻: 2026-08-21T12:00:00Z
対象リポジトリ: `/home/dhuru/22_cxxlens/cxxlens`
対象 `main`: `c99996cc235b078a429fb03e2e8e07bf14e5caa4`
対象 tree: `765a4571979eecaac54c703166ed72b1d44dfbc6`

この文書は、上流設計エージェントへ渡すための非規範的な設計検討資料である。ここに書いた内容だけで ADR、Provider Protocol、issue 状態、release qualification を Accepted または完了に変更してはならない。

## 1. 現在の本当の阻害要因

現在 open の issue は次の9件である。

`#277, #261, #205, #202, #201, #200, #185, #183, #173`

小さな実装進展はあるが、製品完成を止めている本質的な問題は次の三つである。

1. 高リスクの設計契約が Proposed または review-required のままで、未解決 P1 を含む契約を実装・有効化できない。
2. SQLite、source-closure、Store、NG1 の機能は依存関係が直列で、下流だけを先に完成させると意味のない証拠や危険な仮実装になる。
3. release evidence の受け渡しが設計されておらず、Heavy/Nightly と #167/#179 の結果を #173 の authenticated input として同じ候補 SHA に安全に束ねられない。

## 2. 現在確認できている CI 証拠

- Autonomy fast `32472126887`: `c99996c` で success。
- Autonomy heavy `32472570862` と `32472968019`: いずれも `c99996c` で success。current-candidate evidence の採用可否は、後続の authenticated handoff が exact run/artifact を再検証するまで確定しない。
- Quality `32472127102`: `c99996c` で completed failure。通常の build/install/contract/SQLite/Wave 0/full lane は success で、失敗は `production-scope-closure` の fail-closed 境界に限定される。これは source regression の証拠ではないが、release qualification の入力にもならない。
- exact-main Nightly `32462866761`: 全 lane success だが、対象は古い `eec2a7351dae4b613b343d67e79c73fac041a2d4` であり、`c99996c` の証拠ではない。
- 旧 SHA の Quality `32462842314` は、reusable Nightly が `workflow_call` で skip され、`production-scope-closure` が fail-closed した。
- 旧 SHA の release evaluation `32467603378` は evaluator 自体は success だが、結果は `not-qualified`、`gr_issued=false`。不足は `exact-successful-heavy`, `exact-successful-nightly`, `authenticated-gr-report`, `authenticated-terminal-scope-report` である。

authority は Nightly の有効 event を `schedule` と `workflow_dispatch`、`workflow_call` を不適格 event と定義している。したがって、`workflow_call` の成功を release evidence として扱ったり、skip を green に変えたりしてはならない。

## 3. issue ごとの残存問題

### #173: aggregate release qualification

#173 は aggregate decision の owner であり、#167 の release report と #179 の terminal production-scope report を代作してはならない。

本当の設計課題は、別 workflow で生成された Heavy/Nightly と、別 owner の #167/#179 report を、current `origin/main` の SHA/tree/toolchain に対して認証付きで一つの release bundle に結合することである。単に artifact をコピーするだけでは provenance が足りない。

上流設計で決める必要があること:

- producer run ID、workflow identity、candidate SHA、tree、toolchain、artifact digest の結合形式
- Nightly を `workflow_call` として release eligible にしないまま、dispatch run の artifact を安全に handoff する方法
- stale candidate、同じ SHA の重複 run、cancel/retry、artifact 差し替え、report owner の取り違えを拒否する state machine
- #167/#179 の authenticated report が無い場合に絶対に qualified へ進まない terminal matrix

### #261: source-closure wire transport と compiler-VFS binding

ADR0101 の closure identity/VFS 部分は Accepted。ADR0102 の request 2.2/task v4/protocol 1.2 は Proposed、live registry は protocol 1.1、message ID 1--23 のままである。

直近の preactivation candidate `a7d8057bbc2b7d2bbe0d3711f586476b55a2a269` は履歴上の候補であり、現在は PR #354 の head `85d3456797ca6258a534e1dc6cbf14436722c3a5` が修正候補である。PR #354 は cross-task rebinding と authority ownership/digest の修正を含むが、current main への exact-head independent review と受理 receipt はまだ完了していない。

- transfer 内の task ID と extension 外側の task ID を比較しておらず、別 task の有効 transfer を rebinding できる。
- 新しい input schema/checker の ownership が work-unit manifest に反映されず、authority digest が drift する。

この二つを修正して再レビューする必要がある。その後も codec、worker state machine、task-local spool、実際の provider worker への wiring、ADR0101 VFS mount、installed/native/cxxmonster E2E が残る。ambient path、physical checkout、copy-header fallback は解決策にしてはならない。

### #201 -> #205 -> #202: SQLite lifecycle

この三つは同じ SQLite lifecycle authority に属し、依存順を逆転できない。

- #201 は、WAL/SHM を読むだけの接続、zero-effect 証拠、outer custody、logical read receipt の順序を証明する必要がある。現 review は rejected/P1 残存。
- #205 は、writer promotion と reader zero-effect proof を一つの predicate に混ぜず、nested mapping lease の pin、native success 後の promotion、revoke/hide/cleanup、fork/ABA を分離する必要がある。ADR0104 は Proposed、最新 review は `Accepted: NO`。
- #202 は、#201 の閉じた logical-read receipt からだけ WAL-to-rollback normalization に入るよう validator を強化済み（`c99996c`）。これは production normalization の実装・有効化ではない。ADR0104 の production activation は依然 fail-closed。

上流設計に必要なのは、reader zero-effect と writer effect の二つの state machine、phase ごとの field availability、custody/lease の crash/effect matrix、nested mapping の最小 witness、native SQLite callback の曖昧応答を quarantine する規則である。

### #200: Store incremental claim adoption

ADR0103 は Proposed で、exact review に P1 が残っている。`a8d55bd4b1f9eacd1eb1ed9eaab179b369c06d27` は既存の reference/production comparison を checker と negative test に束縛した bounded evidence slice であり、spool-backed adoption 自体ではない。

残る課題:

- full candidate graph/report DOM を作らない incremental claim adoption
- publication 前後の `publication_outcome_unknown` と post-attempt failure の扱い
- independent actual-byte projection と expected-byte projection の二重検証
- 4096 claims、512 MiB、RSS、FD、SQLite/memory parity の resource witness
- #201 logical-read receipt と結び付いた lazy-read residency

digest 一致、既存 bulk test、provider replay だけでこの issue を完了扱いにしてはならない。

### #183: NG1 provider hardening

`3adec3646db741fc39e1d4236979030cc6c38a15` は replay provenance の5つの意味 digest を `semantic-v2:sha256:<64 hex>` として検証する bounded slice である。NG1 maturity は Proposed のままで、live process port、resume/rate boundary、spill、crash/hang recovery、long-run/static/shared qualification は未完了である。

#261 の source-closure registry acceptance が先行依存である。closure receipt を resume token として使うこと、implicit NG0 downgrade、blocking execute path は禁止する。

### #185: exact-main Nightly qualification

旧 SHA `eec2a73` の Nightly は clean-full、ASan/UBSan、TSan、static-analysis、evidence ownership が成功した。しかし現在の `c99996c` には exact-main Nightly evidence がない。

timeout fixture の設計課題として、executable verification の時間と child readiness/descendant cleanup の時間が同じ固定 deadline に混ざっている。固定 timeout を伸ばすだけでは CPU contention 下の hang と遅い正常動作を区別できない。

既存コミット `6b4977f` と `1b29d99` により readiness handshake と sanitizer fixture の基礎は実装済みである。残るのは、handshake の認証・phase 分離、verification/readiness/runtime/cleanup の独立時計、descendant cleanup の独立証拠、`--repeat until-fail:20` と CPU contention 下の再現性、そしてその後の current SHA Nightly である。

### #277: Agent UX / minimal authority context

v2 packet corpus の safe-stop は確認できるが、relation の存在だけから project capability や diagnosis を推測する実装は契約違反である。`sdk-doctor` の capability boundary は Proposed/review-required。

必要なのは admitted use case ID、versioned capability graph、入力可能な authority/phase、`proved/disproved/unknown/partial/conflicting` の結果、actionable missing reason、依存順 completion plan を結ぶ schema/state machine である。`--project` や generic golden path を先に増やしてはならない。

## 4. 上流設計エージェントへの依頼

各課題について、単なる「review が必要」という回答ではなく、次を返してほしい。

1. 依存関係を壊さずに最初に実行できる smallest safe design unit。
2. その unit の exact contract ID、authority file、write scope、受理前/受理後の境界。
3. executable state machine と phase ごとの field availability。
4. P0/P1/P2 counterexample、minimal witness、bounded resource witness、crash/effect matrix。
5. positive/negative test と checker の具体的な追加箇所。
6. 必要な maintainer decision、reviewer、CI artifact、issue receipt。
7. 次の unit へ進むための判定条件。未達なら `unknown` または `blocked` の理由と completion plan。

優先順位は次の通りである。

1. #173 の authenticated cross-workflow evidence handoff 設計
2. #261 candidate の cross-task rebinding と authority digest drift の修正版
3. #201/#205 の reader zero-effect と writer lease の分離設計
4. #200 の bounded streaming publication design
5. #185 の readiness/cleanup fixture の構造的 redesign

提案段階の ADR を Accepted にすること、protocol 1.2 を live registry に追加すること、native `SQLITE_OK` を qualification とみなすこと、issue を証拠なしで close することは、この報告書から認可されない。
