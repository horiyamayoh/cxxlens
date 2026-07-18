# ADR 0083: cross-TU semantic entity identity から occurrence anchor を分離する

- Status: Accepted
- Date: 2026-07-18
- Issue: #152

## Context

`cc.entity.v1` の domain identity は declaration/definition の source anchor を含んでいた。一方、Clang の
call-only batch は header declaration について anchor を常に観測できず、同じ semantic key に対して caller TU の
declaration anchor、anchor 欠落、definition TU の definition anchor の三通りを生成した。その結果、
`cc.call_direct_target.target` は後から publication された実体 `cc.entity.entity` へ join できなかった。

source anchor は semantic entity ではなく declaration occurrence の属性である。definition の選択規則を deterministic
にしても、caller TU が project-wide の definition occurrence を知るとは限らないため、cross-TU identity authority には
できない。

## Decision

`cc.entity.v1` の ordered domain projection から `cc.entity.v1.anchor` を除外する。identity は canonicalization、kind、
semantic owner、structural signature digest、toolchain、provider-local semantic key の完全な共有 projectionから導出する。
Clang USR または versioned structural fallback が provider-local semantic key を構成し、synthetic direct target と実体
entity は同じ `entity_row()` helper を同じ semantic inputs で呼ぶ。

`cc.entity.v1.anchor` は occurrence-only display metadata とし、functional assertion の conflict columns から除外する。
Clang normalizer は standard entity rowへ anchor を投影せず、各 declaration/definition の source span を
`frontend.clang22.entity_observation.v1.source` に保持する。新しい標準 occurrence publication は `cc.declaration.source`
を使用する。

これは未releaseの NG0 `cc.entity.v1` にある cross-TU invariant 違反を正す corrective registry amendment である。
registry document version 1.3.0 が exact authority であり、旧 descriptor digest は同じ engine generation に混在できない。

## Consequences

- header declaration、caller-local forward declaration、別 TU definition は同じ entity ID を得る。
- declaration/definition の source 位置変更は exact USR entity ID を変えない。
- overload、template specialization、member、operator の区別は semantic key と structural signature に残る。
- occurrence source は失われず provider-owned observation または `cc.declaration` で照会できる。
- call-only batch の target は entity publication 後に soft reference から実体 rowへ一意に joinできる。

## Verification

Clang normalizer test は header/forward/relocated definition、overload/template/member/operatorを別 batchで
canonicalizeし target/entity ID equality を検証する。R2 vertical slice は anchor を共有しない call batch と entity batch を
同じ store/query flowへ publishし、direct-target join を検証する。relation quality checker は anchor の identity projection、
authoritative role、merge conflict への再導入を拒否する。
