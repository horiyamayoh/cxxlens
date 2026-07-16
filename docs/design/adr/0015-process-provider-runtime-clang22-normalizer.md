# ADR 0015: process provider runtime、Clang 22 worker、canonical normalizer

- Status: Accepted
- Date: 2026-07-16
- Decision owner: provider-runtime
- Decision issue: #70
- Depends on: ADR 0009, ADR 0010, ADR 0011, ADR 0012

## Context

provider wire、trust/discovery、author SDK は確定したが、host が executable を安全に起動して framed stream を
検証する production runtime は存在しなかった。また、Clang observation を standard `cc.*` relation へ変換する
責務と、GCC 固有・無視された semantic option を exact と誤認しない規則が実装境界として固定されていなかった。

## Decision

process effect は LLVM-free provider SDK の `provider_process_port` 越しに扱う。runtime は exact manifest selection、
effective sandbox minimum、provider/binary/semantic contract、task/invocation/toolchain/environment digest を検証して
から argv-only で worker を起動する。Linux NG0 transport は anonymous read-only input、explicit environment、
process group cleanup、resource limits、no-new-privileges、network syscall deny を evidence 付きで報告する。達成度が
manifest/request/security profile の最大 minimum 未満なら executable を起動せず
`security.sandbox-insufficient` とする。

host input と provider output は ADR 0010 の frame を連結した一回の bounded transcript とする。sequence は 0 から
連続し、provider identity を含む `hello`、唯一の terminal、terminal 後の出力禁止を runtime が検証する。
timeout、cancel、output limit、signal crash、malformed/checksum/truncated stream、schema/coverage failure は別の
stable terminal とする。失敗 transcript は unpublished であり、既存 snapshot series head を変更しない。

official executable は `cxxlens-clang-worker-22` とする。worker input は provider-owned detached task value とし、
外部境界は provider protocol だけである。Clang native object は job/callback/thread/process 内に閉じ、
provider output は detached `frontend.clang22.*_observation` と canonicalized `cc.entity`、`cc.call_site`、
`cc.call_direct_target` row に限定する。

normalizer identity は structural observation、normalized source、compile unit、variant、toolchain から作る。
pretty name、USR 単独、host absolute path、native address、arrival order を authority にしない。fatal/error、
ignored semantic option、GCC-specific option、source normalization failure がある場合は exact equivalence を主張せず、
provider-local/partial guarantee と coverage/unresolved を出す。canonicalize できない observation は保持し、
standard claim を捏造しない。

discovery は explicit path、installation manifest、project config、system registry の順で全候補を説明する。
PATH-only、shadowing、上位 invalid candidate からの downgrade、無許可の adjacent provider/version fallback を拒否する。

## Consequences

- query/store/kernel の SDK link closure は LLVM/Clang-free のまま process provider を利用できる。
- exact Clang major と native SDK は独立 package/executable に閉じる。
- real-time duplex credit、durable resume、multi-worker warm pool は NG1 hardening とし、NG0 transcript と同じ
  semantic validator を共有する。

## Verification

`tests/unit/sdk/provider_runtime_test.cpp` が exact selection、shadowing、sandbox fail-closed、frame/terminal、
timeout/cancel/crash/malformed/output-limit、prior snapshot preservation を検証する。exact Clang 22 job は
`tests/adapter/clang22/provider_normalizer_test.cpp` と
`tests/adapter/clang22/provider_worker_protocol_test.cpp` で observation と standard row、semantic loss、
provider protocol、binary identity を検証する。installed consumer は worker executable、portable/native SDK
package、runtime contract/report schema を確認する。
