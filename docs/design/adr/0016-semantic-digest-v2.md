# ADR 0016: semantic digest の domain/payload framing を v2 typed tuple にする

- Status: Accepted
- Date: 2026-07-17
- Decision owner: identity-kernel
- Decision issue: #120
- Depends on: ADR 0009, ADR 0012

## Context

公開 `semantic_digest(domain, bytes)` は legacy で `domain + NUL + bytes` を SHA-256 へ入力していた。
両引数は長さ付き `string_view` で embedded NUL を保持できるため、異なる pair が同じ入力 byte 列になった。
さらに返却値が content digest と同じ `sha256:` namespace であり、identity contract を識別できなかった。

## Decision

`semantic_digest` v2 は `cxxlens-canonical-tuple-v1` の ordered tuple として次を hash する。

```text
[
  utf8("cxxlens-semantic-digest-v2"),
  utf8(domain),
  bytes(payload)
]
```

domain は `^[a-z][a-z0-9_.-]*$` とし、違反は `sdk.semantic-domain-invalid` の `result` error とする。
payload は任意 byte 列を lossless に保持する。serialized digest は
`semantic-v2:sha256:<64 lowercase hex>` とし、legacy `sha256:` と区別する。

legacy digest を v2 として読み替えない。canonical source tuple を保持する object だけが明示的に再計算できる。
source tuple のない digest-only object に migration はなく、identity major mismatch として拒否する。

## Consequences

- domain と payload の境界は type tag と 64-bit length により prefix-free になる。
- arbitrary binary payload は semantic string coercion を受けない。
- source signature は `result<string>` へ変わり、expected failure を空文字や例外で表さない。
- identity-contract axis は 1.0.0 と 2.0.0 の major mismatch になる。runtime migration はなく、
  canonical source から新 object identity を明示的に構築する場合だけ再計算できる。

## Verification

`tests/unit/sdk/sdk_test.cpp` は collision pair、invalid domain、binary payload、独立 reference vectorを検査する。
public API catalog、acceptance G0 evidence、release-bundle migration が同じ v2 contract を参照する。
