# ADR 0070: Clang 22 USR fallback は構造・toolchain・canonical source anchor を bind する

- Status: Accepted
- Date: 2026-07-18
- Decision owner: clang22-provider
- Decision issue: #139
- Depends on: ADR 0048, ADR 0050, ADR 0051, ADR 0052, ADR 0054

## Context

Clang USR 生成失敗時の旧 fallback は qualified name と declaration kind だけを digest していた。そのため function、method、
constructor、operator、conversion の overload、template specialization、constrained overload が同じ semantic key へ collapse し、
canonical entity と direct call target が任意の1 entityへ誤って結合され得た。

## Decision

USR 成功時の `clang-usr:` identity は変更しない。USR が得られない場合は versioned shared encoder
`clang22.declaration-fallback.v2` を使用し、toolchain digest、declaration kind、qualified name、canonical signature、template
identity、constraint identity、declaration context、canonical source anchor を length-prefixed named field として bind する。
canonical source anchor は task の source snapshot/file、spelling buffer の content digest と offset から作り、filesystem path や observation order を authority に
しない。同じ canonical declaration chain の declaration/definition は同じ入力を使う。

identity result は semantic key と `exact-usr` / `structural-fallback` confidence を別 field で返す。entity observation と direct
callee projection は同じ result を保持する。fallback identity を使った batch は provider-local non-exact とするが、異なる key の
entity と direct target は lossless に保持する。

一意な canonical source anchor を構成できない場合は opaque key を捏造しない。
`provider.declaration-identity-unresolved` として frontend evidence に残し、direct call は explicit unresolved edge にする。

## Verification

`schemas/cxxlens_ng_clang22_fallback_identity.yaml` を versioned conformance authority とする。USR failure seam は overload、special
member/operator/conversion、template primary/specialization、constraint、same-signature redeclaration、definition preference、toolchain、
unanchored caseを検証する。synthetic entity/call batch は overload ごとに異なる entity/direct target を生成し、observation order の
permutationで同一 canonical outputになることを検証する。normal USR prefix の既存 identity は不変とする。
