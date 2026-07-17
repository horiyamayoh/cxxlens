# ADR 0052: Clang call kind と direct target の意味を一致させる

- Status: Accepted
- Date: 2026-07-17
- Issue: #109

## Context

Clang worker はすべての `CallExpr` に `direct_function` を設定してから `getDirectCallee()` を調べていた。そのため
function pointer、member pointer、dependent call も direct call と主張する一方で `cc.call_direct_target` を出せず、
`cc.call_site.kind` と target availability が矛盾していた。virtual member call では static callee declaration と
dynamic dispatch の可能性も区別されていなかった。

## Decision

worker は AST shape を先に分類する。free function は `direct_function`、非 virtual member または qualified virtual call は
`direct_member`、unqualified virtual member call は `virtual_member`、overloaded operator は `operator` とする。
`getDirectCallee()` がなく type/value dependent なら `dependent`、`CXXMemberCallExpr` なら
`indirect_member_pointer`、それ以外は `indirect_function` とする。これらは open `cc.call-kind/1` symbol として保持する。

direct callee のある direct/virtual call だけが `cc.call_direct_target` を生成する。virtual call の target は static target であり、
`virtual_member` kind が dynamic dispatch の可能性を表す。indirect/dependent call は target を捏造せず、points-to または
template resolution が未実装であることを stable unresolved code/reason に残す。既知の direct kind に target がない場合、または
既知の non-direct kind に direct callee がある場合は `provider.call-kind-target-inconsistent` として non-exact にする。

## Consequences

call-kind query は indirect call を direct call と誤認しない。direct-target relation の有無は kind と整合し、virtual dispatch の
static target は失われない。将来 points-to provider が indirect target を追加する際も、現時点の unsupported surface は
unresolved evidence として残る。

## Verification

Clang normalizer test は free function、function pointer、member pointer、dependent call、virtual member の static target、
および kind/target の双方向不整合を検証する。native AST branch は Exact Clang 22 build に加え、利用可能な Clang header に対する
構文コンパイルで分類 API を確認する。
