# ADR 0087: provider budget の logical parity と process resource boundary

- Status: Accepted
- Date: 2026-07-18
- Issue: #123

## Context

public `execution_budget` は全 field を host-enforced と説明していたが、worker-side `run_worker()` は callback と同じ process 内で動く
author adapter であり、wall/CPU/memory/file/process resource を preempt できない。一方 production process port は wall、CPU、`RLIMIT_AS`、
stdout/stderr、open file、subprocess を制限していたが、rows、structured diagnostic records、logical output bytes を shared transcript validator
で合算していなかった。`rss_bytes` は実際には address-space limit、`output_bytes` は stdout protocol と stderr の transport 合計だった。
表現も計測もない `created_files` も保証として公開されていた。

## Decision

`execution_budget` を次の二群として flat value 上で明示する。

- surface-independent logical limits: `output_bytes`、`rows`、`diagnostics`
- process-isolation resource limits: `wall_ms`、`cpu_ms`、`address_space_bytes`、`transport_bytes`、`open_files`、`subprocesses`

`rss_bytes` は実装どおり `address_space_bytes` へ改名する。`transport_bytes` は stdout/stderr combined bytes とし、protocol frame credit と
logical `output_bytes` の双方から独立させる。`created_files` は count を強制する portable mechanism と output artifact surface がないため削除し、
存在しない保証を残さない。

logical `output_bytes` は handshake (`hello` / schema)、task acceptance、terminal を除く provider-to-host logical frame の control bytes と
payload bytes の和である。unknown optional frame も semantic skip 前に計上する。`rows` は validated `batch_end.row_count` の task-global 和、
`diagnostics` は decoded `unresolved_chunk` record 数の task-global 和である。frame/chunk/batch 分割で reset しない。

`protocol_writer` は同じ logical frame classification で worker-side stream を送信前に制限する。logical/process public reference と production
process runtime は一つの typed transcript validator で bytes/rows/diagnostics を検証する。limit 超過は全 surface で stable
`provider.output-limit` とし、validated success stateを付与しない。

`run_worker()` は trusted worker callback adapter であり resource isolation boundary ではない。cancellation は cooperative で、resource
preemption は adapter を `provider_process_port` の後ろで実行するときだけ保証する。process port は wall deadline と process-group kill、
`RLIMIT_CPU`、`RLIMIT_AS`、combined transport drain、`RLIMIT_NOFILE`、`RLIMIT_NPROC` を担当する。

## Consequences

- 同じ decoded logical workload と budget は logical/process validator で同じ terminal reason になる。
- wire header、stdout/stderr、frame creditをsemantic outputと混同しない。
- `address_space_bytes` をRSS/cgroup peakと誤認しない。
- resource isolationを持たないdirect callback APIをtenant sandboxとして扱わない。
- limit後のframesはdiagnostic evidenceとして保持できるが、`succeeded()` はfalseでありadoption authorityにならない。

## Verification

`tests/unit/sdk/provider_runtime_test.cpp` はlogical/process双方でlogical bytesとrowsのexact/one-under、diagnostic record超過、transport byte
limit、worker-side send-before-success rejectionを検証する。provider runtime quality checkerはfield set、shared validator counters、process
resource mapping、trusted-only Doxygen、catalog/contract traceabilityを固定する。
