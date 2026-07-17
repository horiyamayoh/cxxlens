# ADR 0050: relation domain identity を descriptor runtime projection にする

- Status: Accepted
- Date: 2026-07-17
- Issue: #107

## Context

accepted relation registry は result column と ordered projection を宣言するが、runtime `relation_descriptor` は
その metadata を保持していなかった。Clang worker はそのため `cc.entity` と `cc.call_site` の ID を hidden
variant や provider-local semantic key を直接混ぜた独自 digest から生成し、row payload と ID が一致しなかった。

## Decision

`relation_descriptor` は `domain_identity_descriptor` を保持し、IDL compiler は accepted `result_column`、ordered
`projection`、`canonical-binary-tuple-v1` contract を generated descriptor へ写す。
`derive_domain_identity(descriptor,row)` は projection 順に typed cell を canonical tuple 化し、result column の
`typed_id<X_id>` から `_id` を除いて `_` を `-` にした domain tag で full SHA-256 ID を導出する。optional absent
は canonical null、unknown、欠落 required field、表現不能 unsigned integer は structured error で fail closed とする。

`validate_domain_identity` は row の result cell と独立再計算値の exact 一致を要求する。Clang worker は entity と
call-site row の payload を先に構築し、この共通 helper で result ID を設定してから再検証する。batch variant や
display field は descriptor projection にない限り ID へ入れない。provider-local semantic key は宣言済み
`provider_local_key` cell を通じてのみ entity identity に参加する。

## Consequences

static generated API、dynamic descriptor consumer、provider は同じ ordered typed projection と domain separator を共有する。
projection field の変更は ID を変更し、non-projection field と hidden batch metadata の変更は ID を変更しない。
result ID を持たない relation は derive 対象外であり、独自 ID を捏造しない。

## Verification

Clang normalizer test は entity/call ID の descriptor parity、全 projection field mutation、display/variant independence、
independent canonical encoder parity、source.span helper parity、mismatch rejection を検証する。SDK quality gate は
generated descriptor metadata の再現性と worker の共通 helper 利用を固定する。
