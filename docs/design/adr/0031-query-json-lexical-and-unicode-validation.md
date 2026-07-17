# ADR 0031: query argument decoder は RFC 8259 lexical/Unicode rules を fail closed で適用する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: logical-query
- Decision issue: #88
- Depends on: ADR 0027, ADR 0030

## Context

query argument の bounded JSON parser は `00` / `01` / `-01` を integer として受理し、raw string byte の UTF-8 validity を
検査しなかった。一方 `\uD83D\uDE00` のような valid UTF-16 surrogate pair は各 surrogate を単独 error にして拒否していた。
validated Logical IR の受理集合が RFC 8259 serializer/schema validator と一致せず、invalid JSON が semantic identity に入り、
標準 serializer の valid non-BMP string が相互運用できなかった。

## Decision

bounded parser は JSON lexical grammar を fail closed で適用する。

- integer token は `0` または nonzero digit で開始する digit sequence とし、minus はその直前だけに許可する。
- leading zero、plus prefix、欠落 digit、typed field が許可しない fraction/exponent を拒否する。
- JSON whitespace は space、tab、LF、CR の4 byteだけとし、vertical tab/form feed は whitespace とみなさない。
- unescaped string は shared strict UTF-8 validator を通す。
- BMP non-surrogate `\uXXXX` は対応 code point にする。
- high surrogate は直後の `\u` low surrogate とだけ組み合わせ、U+10000〜U+10FFFF の UTF-8 にする。
- isolated high/low surrogate、invalid pair、invalid hex を拒否する。

decode 成功値は ADR 0027 の typed canonical encoder へ渡し、raw UTF-8 と equivalent Unicode escape を同じ canonical bytes に
する。size/depth、duplicate key、exact operator shape の既存 bound は維持する。

## Consequences

- invalid JSON/UTF-8 は validated Logical IR、digest、executor に到達しない。
- Python/JavaScript 等が surrogate pair escape で生成した non-BMP string と相互運用できる。
- raw UTF-8、BMP escape、surrogate pair の representation 差は semantic identity に影響しない。
- decoder と shared canonical JSON/UTF-8 policy の受理集合が一致する。

## Verification

`tests/unit/sdk/query_runtime_test.cpp` は valid `0`/`10`、invalid `00`/`01`/`-01`、BMP escape、valid surrogate pair、isolated
surrogate/invalid pair、overlong/lone continuation/truncated/surrogate/out-of-range raw UTF-8、vertical tab/form feed を検証する。
direct UTF-8 と escaped non-BMP/BMP query の validation、canonical form、digest は一致する。
