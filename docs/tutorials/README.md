# Tutorials

次世代 API は foundation migration 中のため、未実装 surface を動作例として偽装しません。現在利用できる
tutorial は contract authoring と検証 workflow です。

1. [設計 package](../design/README.md) の reading order を確認する。
2. [catalog index](../design/catalogs/README.md) から担当 entry と exact-contract issue を選ぶ。
3. semantics/invariant、identity、value type、schema、validator、test、service の順で変更する。
4. static/dynamic query は同じ normalized Logical IR を生成する fixture を持たせる。
5. provider は native object を worker/callback 外へ出さず、coverage と unresolved を完全会計する。
6. `cxxlens-quality` と documentation consistency gate を実行する。

Runnable recipe、typed query、dynamic query、portable provider、native provider tutorial は、それぞれの
production path が完成する #66、#67、#69、#70、#73 で追加します。
