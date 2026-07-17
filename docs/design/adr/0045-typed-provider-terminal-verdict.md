# ADR 0045: provider terminal verdict を frame kind と登録済み reason に bind する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: provider-protocol/provider-runtime
- Decision issue: #102
- Depends on: ADR 0038, ADR 0044

## Context

process runtime は `task_failed` control の先頭 token を report の terminal 文字列へコピーし、
`process_execution_report::succeeded()` はその文字列が `provider.success` かだけを確認していた。
このため failure frame が success verdict を生成でき、未登録の `provider.*` reason も execution report schema を通過した。

## Decision

共有 transcript validator の terminal result を `transcript_terminal_kind` と登録済み reason の組にする。
`task_complete` の exact task binding を通過した場合だけ kind は `complete`、reason は `provider.success` となる。
`task_failed` は error code、task ID、error field の structured control を要求し、runtime terminal registry にある
non-success provider reason だけを `failed` として受理する。`provider.success` と未登録 reason は
`provider.schema-invalid` に fail closed する。

process execution report は raw terminal text と別に runtime だけが設定できる validated success state を保持する。
`succeeded()` は validated state、`provider.success`、最終 `task_complete` frame の三者が一致した場合だけ true を返す。
transport failure report の state は常に failure である。execution report schema の terminal は正規表現ではなく、runtime
contract の stable terminal registry と同一の enum とする。

## Consequences

- provider output の文字列だけでは success report を生成できない。
- transcript frame kind、terminal reason、`succeeded()` は矛盾しない。
- terminal registry の変更は runtime contract と report schema を同時に更新しなければ quality gate を通らない。

## Verification

`tests/unit/sdk/provider_runtime_test.cpp` は `task_failed(provider.success)`、未登録 failure、登録済み failure、valid
`task_complete`、wrong/missing complete control、および raw terminal text の書き換えを検証する。
`check_ng_provider_runtime.py` は report schema の enum と runtime stable registry の完全一致、および未登録 terminal の schema
reject を検証する。
