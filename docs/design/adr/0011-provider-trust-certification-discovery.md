# ADR 0011: Provider trust、certification、namespace、discovery

- Status: Accepted
- Date: 2026-07-15
- Owner: Issue #65
- Depends on: ADR 0003, ADR 0005, ADR 0010

## Context

provider manifest は publisher、signature、relation offer、interpretation domain を記述できるが、自己申告だけで
standard relation や canonical interpretation の authority を得てはならない。また、PATH 探索、同一 ID の別
binary、失効済み certificate、sandbox の silent degradation は、再現性だけでなく snapshot の意味を破壊する。

## Decision

次の authority を独立して固定する。

- `cxxlens.security-profile.v1`: trust chain、qualification、discovery、sandbox、validation boundary
- `cxxlens.namespace-registry.v1`: relation/provider/interpretation namespace の owner と grant rule
- `cxxlens.provider-certification-registry.v1`: trusted issuer、subject tuple、relation/domain qualification、revocation
- `cxxlens.provider-support-matrix.v1`: provider/relation/toolchain/platform tuple の support state

manifest の certification/trust flag は request でしかなく、authority ではない。standard namespace または
standard interpretation を publish するには、namespace owner と trusted certification registry の双方が認可した
`canonical-semantic-qualified` certificate が必要である。`schema-conformant` は shape validation だけを表し、
canonical semantic authority を含まない。

署名対象は provider ID/version、package identity、publisher、manifest digest、binary digest、semantic contract
digest の exact tuple である。cryptographic verification は trusted verifier port が行い、verifier ID、key
fingerprint、signed subject digest、verdict を evidence として返す。manifest 内の `verified: true` のような
自己申告は入力として受理しない。

discovery precedence は explicit path、installation manifest、project config、system registry の順である。PATH-only
discovery は禁止する。同一 provider ID/version に異なる binary/package が現れた場合は shadowing として全候補を
reject する。上位 source の exact candidate が trust/security validation に失敗した場合、下位候補への fallback は
downgrade なので禁止する。adjacent version/provider fallback も ADR 0010 と同様に caller の明示 policy なしには行わない。

sandbox assurance は `none < best_effort < enforced < certified` の全順序とし、runtime achieved assurance が manifest、
request、security profile の最大 required minimum を満たさなければ provider unavailable とする。product
compiler/plugin/spec execution は opt-in、exact digest、process-tree/audit evidence を必須とする。

## Consequences

- unsigned/unqualified provider は provider-owned namespace では利用できるが、standard authority を獲得しない。
- certification registry の更新と revocation は manifest 配布とは別の trusted update path を必要とする。
- discovery は「見つかった順」ではなく、全候補の検証結果と selection/rejection explanation を返す。
- production support は support matrix の tuple と qualification evidence が揃った場合だけ宣言できる。

## Cryptography and key operations

production signature algorithm は Ed25519、subject digest は domain-separated SHA-256 とする。秘密鍵、production
trust anchor、certificate 発行・rotation 手順は release infrastructure の責務であり repository に埋め込まない。
本 issue の registry は conformance trust anchor のみを持ち、production certificate を一件も宣言しない。

## Verification

`tools/quality/check_ng_security_contract.py` は namespace authority、certificate subject/issuer/revocation/validity、
signature evidence binding、discovery precedence/shadowing/downgrade、sandbox、product execution audit、untrusted input
validation、support tuple を executable vector として検証する。
