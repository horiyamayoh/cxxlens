# ADR 0017: relation descriptor identity を authority と runtime projection の双方へ bind する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: relation-kernel
- Decision issue: #74
- Depends on: ADR 0003, ADR 0006, ADR 0016

## Context

generated descriptor は accepted relation document の canonical JSON とその content digest を保持する一方、
runtime が使用する columns、key、references、merge、conflict columns、semantics を別の mutable value として
保持していた。validator は authority JSON の digest だけを検査したため、runtime fieldを変更しても trusted
descriptor/registry digestを流用できた。

## Decision

`relation_descriptor` は accepted authority JSON の `contract_digest` を独立して保持する。
`descriptor_digest` は ADR 0016 の semantic digest v2 で次を bind する。

```text
domain = cxxlens.relation-descriptor-binding.v2
payload = contract_digest + LF + runtime canonical_form
```

`contract_canonical` がある descriptor は、その content digest と `contract_digest` の一致を必須とする。
全 descriptor は runtime canonical projectionから descriptor digestを再計算し、exact一致しなければ
`sdk.descriptor-digest-mismatch` で拒否する。generated tag compiler は authority digestを literalとして出力し、
全 runtime field設定後に bound descriptor digestを構築する。

dynamic descriptor は authority JSONを持たないため empty contract digestを明示的に bindする。registry は
dynamic descriptor の未設定 digestだけを補完し、authority-bearing descriptor の未設定/stale digestは補完せず
拒否する。registry digest は canonical descriptor ID と bound descriptor digestから構成する。

## Consequences

- generated runtime fieldを変更して trusted digestを保持する impersonation は失敗する。
- authority document digestは独立して監査可能なまま残る。
- runtime semanticsが異なる registryは同じ registry digestを持たない。
- descriptor binding v1/v2 の silent interpretation は行わない。

## Verification

`tests/unit/sdk/sdk_test.cpp` は merge、key、column type、reference、semantics、conflict columns の各 mutation、
unmodified generated descriptor、異なる dynamic registry digestを検査する。IDL compiler reproducibility と
authority digest literalは `tools/quality/check_ng_sdk_contract.py` が検査する。
