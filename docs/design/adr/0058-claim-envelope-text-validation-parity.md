# ADR 0058: claim envelope の text primitive を全 construction surface で共有する

- Status: Accepted
- Date: 2026-07-18
- Decision owner: common/claim-kernel
- Decision issue: #130
- Depends on: ADR 0016, ADR 0030, ADR 0036, ADR 0057

## Context

`cxxlens.claim-envelope.v2` schema は identity field を strict UTF-8、control-free、最大 512 Unicode scalar とする一方、
public C++ aggregate と claim constructor は多くの field を nonempty だけで受理した。invalid UTF-8 を JSON escape で別 Unicode textへ
救済する path もあり、binary identity、JSON view、schema decoder の受理集合が一致しなかった。

## Decision

- `validate_utf8_text()`、`validate_strong_id()`、`validate_registered_symbol()` を common public primitive とする。
- strong ID は nonempty、strict UTF-8、U+0000..U+001F と U+007F を禁止し、最大 512 Unicode scalar とする。
- registered symbol は schema の `[a-z][a-z0-9_.-]+` と exact 一致する。
- claim condition、interpretation、producer、provenance、guarantee、direct/derived basis の全 text は identity encoding 前に同じ primitiveを通す。
- `canonical_utf8_string()` と `canonical_json_text()` は invalid UTF-8 を別 text へ変換せず `sdk.text-invalid` で拒否する。
  unchecked `canonical_value` aggregate も `canonical_binary()` の recursive validation を通過しなければ identity bytesを生成しない。
- source-span identity も同じ strong ID primitive を使用する。field-specific API は common rejection detail を保持して安定 categoryへ写像できる。

## Consequences

- public builder、aggregate revalidation、stage constructor、store adoption は `validate_claim()` を通じて同じ text acceptance setを持つ。
- valid UTF-8 の byte sequence は canonical binary/JSON の間で意味を変えず、invalid sequence はどちらにも入らない。
- delimiter は framing で扱い、schema が許可する printable character を場当たり的に禁止しない。

## Verification

`tests/unit/sdk/sdk_test.cpp` は overlong、surrogate、範囲外、truncated、invalid continuation の UTF-8 corpus、control/DEL、
512/513 scalar 境界、registered symbol grammar、checked binary/JSON parity、source-span rejectionを検証する。同 test は condition、producer、
interpretation、provenance、guarantee、basis の個別 mutation を builder、`validate_claim()`、canonical/derived constructorへ通し、
同じ `sdk.text-invalid` categoryで拒否することを検証する。
