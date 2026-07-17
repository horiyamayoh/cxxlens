# ADR 0030: SDK serialization は shared canonical JSON encoder と strict UTF-8 validation を使う

- Status: Accepted
- Date: 2026-07-17
- Decision owner: query-runtime / relation-foundation
- Decision issue: #87
- Depends on: ADR 0007, ADR 0014, ADR 0029

## Context

query execution result の手書き JSON escape は `\b`、`\f`、`\n`、`\r`、`\t`、quote、backslash だけを処理し、
その他の U+0000〜U+001F byte を raw のまま出力していた。`detached_cell` は string scalar の UTF-8 validity を検査して
いなかったため、valid SDK data から parse不能な canonical result を生成できた。serializer ごとに escape 実装も重複して
いた。

## Decision

SDK internal serializer は `src/sdk/json_internal.hpp` の単一 canonical JSON string encoder を使用する。relation、claim、logical
query、query execution result、provider manifest/runtime report はこの encoder を共有する。

encoder は quote/backslash と U+0000〜U+001F を必ず JSON escape する。backspace、form feed、newline、carriage return、tab は
short escape、それ以外の control byte は lower-case `\u00xx` とする。valid non-ASCII UTF-8 sequence は byte-preserving で出力し、
Unicode normalization は行わない。bytes scalar は従来どおり hexadecimal representation だけを使用する。

`detached_cell::validate()` は present string scalar と unknown reason に strict UTF-8 (no overlong、surrogate、truncated sequence、
U+10FFFF超過) を要求し、違反を `sdk.cell-invalid` / `invalid-utf8` で拒否する。defensive encoder は未検証 invalid byte を raw
出力せず `\u00xx` に退避するが、この fallback は semantic data の受理を認可しない。

## Consequences

- validation 成功した SDK value の canonical JSON は control byte を含まない parseable UTF-8 になる。
- provider data による JSON lines/log record injection を防ぐ。
- memory/SQLite は同じ semantic row string を byte-identical に serialize する。
- serializer 間の escape policy divergence を除去する。

## Verification

`tests/unit/sdk/query_runtime_test.cpp` は 0x00〜0x1f、quote、backslash、非ASCII UTF-8 を含む valid row を memory/SQLite snapshot
から query し、raw control byte がなく全 required escape が存在し、row projection が backend 間 byte-identical であることを
検証する。overlong UTF-8 value と unknown reason は `invalid-utf8` で拒否される。
