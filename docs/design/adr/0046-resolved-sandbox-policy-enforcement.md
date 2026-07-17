# ADR 0046: sandbox policy digest を resolved enforcement plan に bind する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: security/provider-runtime
- Decision issue: #103
- Depends on: ADR 0015, ADR 0042

## Context

Linux process port は任意の well-formed `sandbox_requirement.policy_digest` に同じ固定 controls を適用し、request の digest を
execution report へコピーして `achieved=enforced` としていた。digest から policy を resolve せず、report evidence も request
文字列と固定 mechanism 名だけに依存したため、policy identity と kernel enforcement の対応を証明できなかった。

## Decision

`cxxlens.provider-sandbox-policy.v1` の built-in registry を public value API と security profile に定義する。baseline policy は
resource limits、sealed input、explicit environment、process group、no-new-privileges、network syscall deny を bind する。strict
policy はさらに core dump と locked memory の limit を bind する。policy digest は canonical policy projection の
`cxxlens.provider-sandbox-policy.v1` semantic digest とする。

selection と system process port は digest を process effect 前に `resolve_sandbox_policy()` で解決する。未知 digest と canonical
policy の 1 byte mutation は `security.sandbox-policy-mismatch` で reject する。adapter は resolved controls から child setup を
構成し、request digest を report へ echo しない。policy digest は applied policy から再計算する。

evidence digest v2 は resolved policy canonical form、recomputed policy digest、achieved assurance、invocation budget limits、exact
applied mechanisms を bind する。runtime は report adoption 前に evidence を独立再計算し、enforced/certified report の mechanism
set が policy と exact に一致することを検証する。required mechanism installation が失敗した child は exit 126 で fail closed し、
`achieved=none` と `security.sandbox-insufficient` を返す。

## Consequences

- 同じ enforcement plan に異なる arbitrary policy digest を付けられない。
- distinct built-in policy は distinct digest、mechanism set、kernel limit plan を持つ。
- selection authority、process invocation、execution report は同じ resolved policy token に bind される。
- cancellation など process effect 前の terminal は applied mechanisms を主張しない。

## Verification

`tests/unit/sdk/provider_runtime_test.cpp` は known baseline/strict、unknown digest、canonical byte mutation、distinct plan、evidence
recomputation、selection/execution identity、limit installation failure を検証する。Clang worker protocol test も同じ built-in policy
registry を使用する。security/runtime quality checker は public resolver、adapter resolution、evidence recomputation、contract
registry を trace する。
