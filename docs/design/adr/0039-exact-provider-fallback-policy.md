# ADR 0039: provider fallback を明示 exact tuple policy に限定する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: provider-runtime/security
- Decision issue: #96
- Depends on: ADR 0011, ADR 0015, ADR 0038

## Context

`provider_selection_request::allow_adjacent_fallback` は一つの bool であり、true の場合は同じ provider ID の任意 version、
binary digest、semantic contract digest を fallback 候補にした。許可された identity、version direction、qualification、
certificate、複数候補の precedence を表現できず、candidate 集合の辞書順が実行 authority になっていた。

## Decision

bool fallback flag を廃止し、optional `provider_fallback_policy` に置き換える。policy は namespaced policy ID と一件以上の
`provider_fallback_tuple` を持ち、各 tuple は unique non-zero priority、provider ID、semantic version、binary digest、
semantic contract digest、requested version との direction、certification requirement、certification authority が付与した
required qualification set を exact に bind する。

tuple の direction は requested version との比較結果へ一致しなければならない。manifest の requested/self-asserted
qualification は fallback authority に使用せず、discovery/certification verdict の `certified_qualifications` だけを照合する。
policy にない tuple は provider ID が同じでも `provider.fallback-policy-mismatch` とする。

複数の listed tuple は unique priority の昇順、その後 discovery source precedence で選ぶ。最上位 tuple/source が invalid な場合は
lower policy precedence へ silent downgrade しない。exact requested identity は常に fallback より優先し、invalid exact identity が
存在する場合も従来どおり lower fallback を禁止する。

policy は canonical priority/identity order で serialize し、`cxxlens.provider-fallback-policy.v1` semantic digest を持つ。
`provider_selection::canonical_form()` は fallback を使用したかにかかわらず request policy digest を保持する。

## Consequences

- flag 一つで provider identity の範囲を拡張できない。
- different major、same-version rebuild、semantic contract change は exact tuple と direction が明記された場合だけ候補になる。
- cross-version/support qualification は manifest self-claim で代替できない。
- candidate input orderではなく policy priority と discovery authority が selection を決める。

## Verification

`tests/unit/sdk/provider_runtime_test.cpp` は listed exact fallback、unlisted major/binary/semantic contract の拒否、listed
same-version rebuild、certified semantic change、manifest self-claim の拒否、複数 tuple の explicit priority、selection canonical form の
policy digest binding を検証する。
