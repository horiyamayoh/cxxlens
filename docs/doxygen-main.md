# cxxlens Semantic Relation Platform

`cxxlens` は C/C++ build context から versioned semantic claims を収集し、condition、provenance、
partiality とともに immutable snapshot へ公開する C++23 library です。

現在の installed header は次世代 public surface です。public API の状態は
`schemas/cxxlens_ng_public_api_catalog.yaml`、設計 invariant は
`docs/design/cxxlens_next_generation_integrated_design_ja.md` を参照してください。

## Author SDK

通常の author SDK は `<cxxlens/sdk.hpp>` と CMake target `cxxlens::provider_sdk` から利用でき、LLVM/Clang の
header と link dependency を持ちません。generated typed query、runtime dynamic query、portable provider、
conformance harness、recipe lowering は同じ `cxxlens::sdk::relation_descriptor` と
`cxxlens::sdk::query::logical_query_ir` を使用します。

Clang 22 native author は `<cxxlens/provider/clang22.hpp>` と `cxxlens::clang22_provider_sdk` へ明示 opt-in
します。translation unit と AST object は callback-scoped borrowed value であり、結果は callback 内で detached
row/source value に正規化します。

`cxxlens::provider::clang22::translation_unit_input::validate()` は、単一ソースの logical path を effects より前に
fail-closed 検証します。空文字列、`/` または `\\` で始まる root-relative path、NUL を含む path、`/` または `\\` で
区切られた正確な `..` parent segment は `native.input-invalid`（field=`logical_path`）になります。一方、
`project://src/foo..bar.hpp` のように component の一部として二つの dot を含む通常の名前は受理されます。この
preflight は #261 の source-closure identity や compiler VFS を定義せず、ambient filesystem fallback も追加しません。
公開 validator の positive/negative acceptance は
`tests/public_headers/provider_clang22_translation_unit_input_test.cpp`、family の invariant と evidence は
`schemas/cxxlens_ng_public_api_catalog.yaml` が authority です。

API family の stable error、lifetime、threading、versioning、invariant、実装/正例/負例は
`schemas/cxxlens_ng_public_api_catalog.yaml`、一 callable 一 row の exact signature と declaration correspondence は
`schemas/cxxlens_ng_public_callable_inventory.yaml` が authority です。利用例は `docs/tutorials/README.md` と
`examples/sdk/` にあります。
