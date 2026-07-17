# ADR 0036: claim stage constructor は入力 claim を独立再検証する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: claim-kernel
- Decision issue: #93
- Depends on: ADR 0007, ADR 0019

## Context

`make_canonical_claim()` は assertion stage と transform/descriptor だけを確認し、入力 claim の row、envelope、identity を
`validate_claim()` で再検証しなかった。canonical output 自体が valid なら、改変された入力 assertion を provenance basis として
受理できた。一方、`make_derived_claim()` は全入力 claim を再検証しており、stage constructor 間で入力 authority が異なっていた。

## Decision

入力 claim を受け取るすべての stage constructor は、stage 固有の判定や出力 encoding より前に、共通
`validate_claim_inputs()` policy で各入力へ `validate_claim()` を適用する。

- canonical constructor は単一入力 assertion の row、descriptor、condition、interpretation、producer、basis、guarantee と
  semantic key/assertion/content identity を独立再検証する。
- derived constructor は同じ policy を全入力へ適用する。
- 入力 validation error は stage constructor がそのまま返し、valid assertion に対してのみ canonical stage 条件を検証する。
- ADR 0058 の schema-equivalent text validation error も変換せず、builder/aggregate/stage constructorで同じ categoryを返す。
- canonical constructor の assertion-only 規則は維持し、canonical/derived claim の再 canonicalization は拒否する。

## Consequences

- canonical claim は独立 validation を通過した assertion だけを入力 authority とする。
- 出力 identity が valid でも入力改変を隠せない。
- canonical/derived constructor は同じ入力 claim に対して同じ validation error を返す。

## Verification

`tests/unit/sdk/sdk_test.cpp` は row、semantic key、assertion/content ID、condition、interpretation、producer contract、guarantee、
basis の改変を canonical/derived の双方が同じ error で拒否すること、non-assertion stage の拒否、valid assertion の成功を検証する。
ADR 0058 の UTF-8/control/max-length field mutation も同じ revalidation matrixへ含める。
