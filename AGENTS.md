# cxxlens agent contract

## Authority and reading order

判断が衝突する場合は、次の順序を優先する。

1. `docs/design/cxxlens_integrated_design_ja.md` の原則と invariant
2. `schemas/cxxlens_public_api_contract.yaml`
3. package detail と accepted ADR
4. acceptance fixture と実装
5. `docs/design/archive/` の履歴資料

実装前に統合設計書の 0、6、7、8、37、39 章と、担当 API の catalog entry を読む。

## Required implementation rules

- C++23 を使用し、公開 namespace/type/function は設計書の lower snake case に従う。
- 通常の public header に `clang::*`、`llvm::*` または LLVM/Clang header を露出しない。
- schema-first の順序は semantics/invariants、identity、value types、schema、validator、tests、service とする。
- filesystem、process、time、hash は port 越しに扱う。
- AST pointer を保存、所有、別スレッドへ移送しない。raw owning pointer を導入しない。
- unordered container の iteration order を serialization や ID に使用しない。
- read result は empty と unresolved を区別し、evidence/coverage/guarantee を落とさない。
- mutation/generation は plan、独立 validator、dry-run、transaction の順を崩さない。
- public API を変更したら API catalog、Doxygen、acceptance test、設計 traceability を更新する。

## Forbidden shortcuts

- name や pretty type string だけによる semantic identity
- compile command や variant の silent fallback/first-wins
- macro expansion range への直接 edit
- conflict、stale digest、variant、reparse failure の無視
- unsupported surface の omission
- diagnostic prose substring による制御
- shell command の文字列連結
- test に合わせた上位 contract の縮小

## Commands and completion

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang
cmake --build --preset dev-clang --target cxxlens-quality
```

公開 API は header/signature/ownership、error/unresolved/coverage、ID/order、schema/invariant、
positive/negative test、example、catalog ID が揃うまで完成扱いにしない。
