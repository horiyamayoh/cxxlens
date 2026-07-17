# ADR 0069: provider control metadata は self-delimiting typed CBOR に統一する

- Status: Accepted
- Date: 2026-07-18
- Decision owner: provider-protocol/provider-runtime
- Decision issue: #138
- Depends on: ADR 0010, ADR 0041, ADR 0043, ADR 0044, ADR 0064

## Context

column chunk と batch end は deterministic typed CBOR へ移行済みだったが、task accepted、batch begin、coverage、unresolved、
evidence、task terminal は CBOR text 内で `|` と改行を連結していた。authoring builder は delimiter を正当な text として受理する一方、
host validator は固定 field count で split するため、SDK が生成した transcript を同じ SDK が拒否できた。また evidence builder は
summary を canonical record の一部として扱うが、wire は kind、subject、producer だけを送り、summary のみ異なる evidence が同一
transcript identity へ collapse していた。

## Decision

host-to-provider の schema negotiate、open task、credit、close と provider-to-host の task accepted、batch begin、coverage、
unresolved、evidence、task complete、task failed control を RFC 8949
deterministic closed-subset CBOR map にする。single record は schema discriminator と exact named text fields を持つ。record set は schema、
unsigned record count、`<index>.<field>` の exact named text fields を持ち、全 field は CBOR type と byte length で self-delimiting にする。
optional prose は field omission ではなく empty text で明示する。

portable worker、process fixture、Clang worker、testing harness、process/shared transcript validator は同じ public typed codec を
使用する。decoder は exact schema、record count、field set/type、canonical key order、shortest length、duplicate key、trailing bytesを検証する。
semantic ID の grammar は decode 後に検証するが、reason/detail/summary など schema が許す text は `|`、LF、CR、U+0000、multi-byte
UTF-8 を byte-preserving で保持する。

evidence wire record は kind、subject、producer、summary の4 fieldを必須とする。transcript projection は control digest を含むため、summary
の差は harness semantic transcript と process execution report transcript digest の双方へ lossless に bind される。

## Verification

全 metadata codec で delimiter、LF、CR、NUL、multi-byte UTF-8 の round trip を検証する。delimiter を含む dependency/atomic/batch ID、
coverage reason、unresolved detail を portable worker から shared validator まで通す。summary だけ異なる2 evidence set は異なる
semantic transcript と execution report canonical identity を生成する。malformed map field count、duplicate key、record count、trailing bytesを
reject し、logical harness と process runtime が同じ typed decoder/state machine を通ることを固定する。
