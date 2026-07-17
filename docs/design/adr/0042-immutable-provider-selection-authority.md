# ADR 0042: process provider selection を immutable validated authority token にする

- Status: Accepted
- Date: 2026-07-17
- Decision owner: provider-runtime/security
- Decision issue: #99
- Depends on: ADR 0011, ADR 0015, ADR 0039

## Context

`process_task_request` は public mutable `provider_selection` を受け取り、runtime は `select_provider()` 由来かを確認しなかった。
caller は selected candidate の authoritative path、trust/certification verdict、validation error、decision transcript、fallback
policy binding、sandbox requirement を構築後に変更できた。binary content digest の launch-time 検証だけでは discovery と trust
authority を再現できず、manifest minimum より弱い request sandbox を process invocation に渡せた。

## Decision

`provider_selection` を private construction の class とし、有効 token は `select_provider()` だけが生成する。selected candidate、
original `provider_selection_request`、全 candidate decisions、fallback used、fallback policy semantic digest は private value-owned state
とし、const accessor だけを公開する。default token は invalid であり、candidate や decision を caller が書き換える API は持たない。

`provider_selection::validate()` は launch authority boundary で次を再検証する。

- token が `select_provider()` によって seal されている
- selected decision が exact に一件で、source/provider ID/version/binary digest と selected candidate に一致する
- selected reason と fallback used が一致する
- stored authority request の fallback policy digest と token binding が一致する
- stored request と immutable candidate を `select_provider()` へ replay し、authoritative path、manifest、validation error、trust、
  certification、certified qualifications、sandbox report、exact/fallback policy を再度通過する

`process_provider_runtime::execute()` は task validation や process launch より先に token validation を必須とする。process request の
sandbox policy digest は selection authority request と exact に一致させ、minimum は selection request、process request、manifest
minimum の assurance 最大値とする。この effective requirement を process port への invocation と returned sandbox report の両方へ
適用し、caller による downgrade を許さない。

## Consequences

- untrusted/path-only/uncertified candidate から execution token を構築できない。
- selection 後の candidate、decision、fallback policy 改変は public C++ source API 上不可能になる。
- default/forged token は `provider.selection-invalid` で process effect 前に reject される。
- request が弱い sandbox minimum を提示しても manifest/security selection authority の minimum は保持される。

## Verification

`tests/unit/sdk/provider_runtime_test.cpp` は path/trust/certification/validation-error candidate が token を生成しないこと、token の
selected decision replay、public accessor の constness、default token の runtime rejection、sandbox policy mismatch rejection、
selection/request/manifest maximum の enforced execution、validated exact/fallback token の成功を検証する。
