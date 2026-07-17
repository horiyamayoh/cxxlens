# ADR 0044: provider harness と process runtime で typed transcript validator を共有する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: provider-sdk/provider-runtime
- Decision issue: #101
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

## Consequences

- harness accepted は production typed state machine を通過した complete logical streamだけを意味する。
- validatorの追加修正は harness、public reference、process runtimeへ同時に適用され、判定 driftを作れない。
- fault report は decode/state/credit/cancellation failureを stable reason code と decoded prefixで保持する。

## Verification

`tests/unit/sdk/sdk_test.cpp` は valid stream、unsealed batch、unrequested descriptor、incomplete coverage、wrong direction、
missing/wrong terminal、credit/cancel/corrupt/truncate faultを検証し、同じ invalid framesをpublic reference validatorへ再投入して
reason一致を確認する。`tests/unit/sdk/provider_runtime_test.cpp` は成功process transcriptと malformed column transcriptを
`validate_process_transcript()` へ再投入し、runtime reportの acceptance/reasonと一致することを検証する。
