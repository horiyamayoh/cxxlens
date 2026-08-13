# ADR 0044: provider harness と process runtime で typed transcript validator を共有する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: provider-sdk/provider-runtime
- Decision issue: #101
- Clarification feedback: DF-0195 / #195, DF-0197 / #197
- Depends on: ADR 0010, ADR 0038, ADR 0043

## Context

public `provider_harness::run()` は frame decode と sequence continuity だけで conformance を accepted にしていた。
batch seal、output descriptor whitelist、coverage、terminal task binding、message direction/order は検証せず、production
process runtime の typed state machine と別実装だった。そのため checksum が正しくても production では reject される
provider が SDK conformance を通過できた。

## Decision

provider-to-host transcript の production validator を `provider_validation_internal.hpp` の一つの implementation にする。
validator request は task/provider identity、optional manifest、exact output descriptors、byte/frame credit、handshake requirement を
bind する。process runtime は full hello/schema handshake mode、in-memory harness は logical stream modeで同じ functionを呼ぶ。
両 mode は task acceptance、direction/order、columnar batch、coverage/unresolved/progress、terminal、credit、sequenceを同じ
state transition と stable reason code で判定する。

public testing API は decoded frames を同じ validatorへ再投入する `validate_logical_transcript()` と
`validate_process_transcript()` を提供する。harness fault は worker実行後の encoded bytes/frame set または validation creditを
変更してから production validatorへ渡す。checksum corruption、truncationは共通 frame decoder、wrong direction、missing/wrong
terminal、credit exhaustion は共通 typed validatorが分類する。

`run_worker()` は callback errorだけでなく coverage/unresolved/evidence finalization errorでも task-bound `task_failed` を送って
logical streamをterminalにし、failure cleanup と reasonを transcriptに保持する。open batchを残した成功、未要求 descriptor、
不完全 coverage は conformance accepted にしない。

DF-0197 / Issue #197 の Provider Protocol 1.1 host input は、host encoder、worker decoder、process runtime、conformance validator が
同じ incremental transition/digest/length/budget core を使用する。minor 0 の既存 public vector encoder/validator signature は変更せず、
exact 5-frame stream をこの core に渡す bounded compatibility wrapper とする。minor 1 の `input_descriptor` / `input_chunk` は
欠落、重複、順序・offset・length・digest 不一致を task acceptance 前に拒否し、raw frame または input spool から semantic task や
adoption value を再構成する別 validator を作らない。

DF-0195 / Issue #195 により、この shared validator は specialization-blind のまま successful transcript ごとに exact
`{kind: task, id: <provider task ID>, state: covered, reason: ""}` transport record を一件要求する。それ以外の未知または追加 semantic
coverage record は exact decoded record と wire order を losslessly retain し、generic 層では捨てず、specialization の意味へ解釈または
再分類しない。duplicate `(kind, id)` と missing/duplicate/renamed/wrong-task/non-covered/nonempty-reason transport record は拒否する。

process runtime は raw provider stdout の exact bytes を decode/move 前に count/SHA-256 し、同じ shared validation pass から
decoded frame projection の `cxxlens.provider-frame-transcript.v2` digest と immutable sealed value の
`cxxlens.provider-sealed-transcript.v1` digest を生成する。runtime-private receipt はこの三 source と count だけを closed field set で保持し、
report が supplied digest 同士の self-consistency だけで作ることを禁止する。public
`process_execution_report::semantic_digest()` は diagnostic report identity であり、raw/frame/sealed receipt の alias または adoption
authority ではない。

同じ pass は provider stdout から期待値を学習しない。process launch 前に independently validated selection/manifest/session authority から
closed `{provider_id, provider_version, provider_binary_digest, provider_semantic_contract_digest, protocol_major, protocol_minor,
required_features, sandbox_policy_digest, offered_relations}` projection を受け取り、raw `hello` occurrence と exact compare する。
`task_accepted` の provider ID/version はその外部 projection と task ID の双方へ cross-bind する。hello と task acceptance が互いに
self-consistent でも、selected binary/semantic contract/version と異なれば seal または receipt を生成しない。

## Consequences

- harness accepted は production typed state machine を通過した complete logical streamだけを意味する。
- validatorの追加修正は harness、public reference、process runtimeへ同時に適用され、判定 driftを作れない。
- host input の fragmentation/read 境界が異なっても同じ sealed logical input verdict となる。
- specialization-specific coverage を generic validator に hard-code せず、完全な retained record set を後段の独立 specialization seal へ渡せる。
- raw occurrence、decoded frame transcript、immutable seal の三 digest authority が分離され、public diagnostic report digest と混同されない。
- provider identity は raw transcript と独立した selected-manifest/session/launcher authority に束縛され、自己申告の置換を受理しない。
- fault report は decode/state/credit/cancellation failureを stable reason code と decoded prefixで保持する。

## Verification

`tests/unit/sdk/sdk_test.cpp` は valid stream、unsealed batch、unrequested descriptor、incomplete coverage、wrong direction、
missing/wrong terminal、credit/cancel/corrupt/truncate faultを検証し、同じ invalid framesをpublic reference validatorへ再投入して
reason一致を確認する。`tests/unit/sdk/provider_runtime_test.cpp` は成功process transcriptと malformed column transcriptを
`validate_process_transcript()` へ再投入し、runtime reportの acceptance/reasonと一致することを検証する。
contract checker は task transport record の mutation を拒否し、未知 semantic coverage の byte-preserving retention を検証する。
さらに raw stdout、decoded frame projection、sealed value の各 mutation、same-pass marker drift、receipt extra field、public digest alias、
report self-consistency、provider ID/version/binary/semantic contract の自己整合した置換を拒否する。
