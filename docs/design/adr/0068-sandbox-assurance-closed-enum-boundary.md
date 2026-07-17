# ADR 0068: sandbox assurance は全 security boundary で closed enum として検証する

- Status: Accepted
- Date: 2026-07-18
- Decision owner: provider-runtime
- Decision issue: #137
- Depends on: ADR 0011, ADR 0042, ADR 0046, ADR 0057

## Context

`sandbox_assurance` の security order は `none < best_effort < enforced < certified` である。一部の入口は closed-enum
membership を検証していたが、selection と process runtime の比較は underlying `uint8_t` の大小を直接使っていた。さらに evidence
digest API が範囲外 achieved value を文字列化できたため、future/invalid ordinal が `certified` より強い値として比較されながら、別の
canonical text へ bind される余地があった。custom process port、FFI、decoder、public aggregate の直接構築は同じ trust boundary を通る。

## Decision

assurance の正当な値は four-value closed set だけとする。requirement、report、selection candidate、effective minimum の全 operand、process
output、evidence digest の各入口で membership を検証し、検証済み operand のみを比較する。未知値を既知値へ map せず、ordinal rank を
与えない。

invalid required は `provider.sandbox-requirement-invalid`、invalid achieved は `provider.sandbox-report-invalid` とする。selection と
runtime はこれらを `provider.not-found` や `provider.runtime-unavailable` に畳まず、そのまま fail-close する。
`sandbox_evidence_digest` は `result<string>` を返し、invalid achieved から digest を生成しない。canonical name は未知値を `none` へ
fold しない。selection token の runtime replay は original request と selected candidate を再検証するため、invalid assurance を含む
authority を再利用できない。

## Verification

required/achieved に raw ordinal 4 と 255 を与え、requirement、report、selection、evidence digest、custom process port の各 surface で
stable error を検証する。valid four levels は 4 x 4 comparison matrix を固定する。security conformance vectors も invalid required と
invalid achieved の 4 case を持ち、unknown future ordinal を既存最強値として扱わないことを schema/profile authority に含める。
