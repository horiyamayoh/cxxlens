# ADR 0040: provider frame version と flags を session semantics に bind する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: provider-protocol/provider-runtime
- Decision issue: #97
- Depends on: ADR 0010, ADR 0038

## Context

104 byte frame header は `protocol_major`、`protocol_minor`、`flags` を持つが、public `frame` と decoder は
major 以外を保持せず、minor と flags を上位 session validator から消していた。unsupported minor、unknown required
extension、codec 未交渉の compressed payload、reserved bit、不正な end-of-stream を通常 frame として解釈できた。
一方、accepted protocol の unknown optional message は skip-and-account であるのに、public enum の範囲外 message は
flags に関係なく reject されていた。transcript identity も header semantics を区別しなかった。

## Decision

public `frame` は major、minor、flags を value として保持し、`message_type` は wire の uint16 domain を損失なく保持する。
`protocol_limits` は session の protocol major、compatible minor range、supported flags を明示する。runtime は manifest と
request の compatible minor intersection から最高 minor を一度だけ選択し、以後の全 host/provider frame をその exact
major/minor へ bind する。範囲外 minor は `provider.protocol-minor-mismatch` とする。

reserved bit、同時 required/optional、base message への optional flag は `provider.invalid-frame-flags` とする。
unknown required flag/message は `provider.unknown-required-extension`、codec を交渉していない compressed payload は
`provider.unsupported-compression` として reject する。

unknown optional message は header、declared length、checksum を検証した decoded frame として保持する。runtime は encoded
byte/frame credit と contiguous sequence を先に account し、その後 typed state transition からだけ skip する。したがって
optional extension は state、terminal ordering、budget を迂回できず、execution report に accounting evidence が残る。

`end_of_stream` は最終 `task_complete` または `task_failed` frame だけに許可する。flags=0 の v1.0 terminal も互換のため
引き続き受理する。semantic transcript projection は major、minor、flags、message type、stream、sequence、control/payload
digest を含み、header semantics だけ異なる transcript を同一視しない。

## Consequences

- negotiation と wire decoder の accept/reject 集合が一致する。
- optional extension は安全に前方互換となるが、resource accounting や audit evidence から消えない。
- compression は codec と展開後 size bound の contract が追加されるまで fail closed となる。
- minor または flags の変更は semantic transcript identity を変更する。

## Verification

`tests/unit/sdk/sdk_test.cpp` は exact minor、unsupported minor、reserved/required/optional/compressed flags と current v1.0
round trip を検証する。`tests/unit/sdk/provider_runtime_test.cpp` は negotiated minor、optional extension の credit accounting、
invalid EOS、valid terminal EOS、header-only semantic digest 差を検証する。`tests/quality/test_ng_provider_protocol.py` は独立
backend authority で同じ分類を検証する。
