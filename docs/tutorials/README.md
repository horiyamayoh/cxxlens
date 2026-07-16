# Author SDK tutorials

Issue #66 で、recipe 利用者から native provider 作者まで同じ relation descriptor と Logical Query IR を使う
5経路が runnable example になりました。repository root で次を実行します。

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang -R '^(unit\.sdk|public-api\.(sdk-header|provider-clang22-header)|sdk\.)'
```

## Generated typed query

[typed_query.cpp](../../examples/sdk/typed_query.cpp) は generated relation/column tag を使い、型付き literal、filter、
project を同じ Logical Query IR へ lower します。tag は accepted registry から生成できます。

```sh
python3 tools/sdk/relation_idl_compiler.py \
  --relation cc.call_site --output /tmp/cc_call_site.hpp
```

存在しない generated column は [generated_unknown_column.cpp](../../examples/sdk/negative/generated_unknown_column.cpp)
のように compile fail します。

## Runtime dynamic query

[dynamic_query.cpp](../../examples/sdk/dynamic_query.cpp) は relation name と explicit semantic major から descriptor を
取得し、runtime column lookup 後に typed path と同じ `query::builder` を使います。文字列から数値列への暗黙 coercion
は [dynamic_literal_type_mismatch.cpp](../../examples/sdk/negative/dynamic_literal_type_mismatch.cpp) の structured error
になります。`logical_query_ir::canonical_form()` は独自形式ではなく、accepted
`cxxlens.logical-query-ir.v1` の canonical JSON を返します。

## Portable provider

[portable_provider.cpp](../../examples/sdk/portable_provider.cpp) は `portable_provider::run` 内で `relation_sink` と
coverage/evidence builder を使います。provider 作者は frame header、SHA-256、sequence、credit を構成しません。
`provider_harness` は production codec と同じ経路に no-credit、cancel、checksum、truncate fault を注入できます。
`provider::manifest` と scaffold が出力する manifest は、同じ `cxxlens.provider-manifest.v1` schema に適合します。

project scaffold は次で生成します。既存の非空 directory は上書きしません。

```sh
./build/dev-clang/cxxlens-provider-scaffold \
  /tmp/lock-provider company.example.lock-provider portable company.lock.acquire
```

## Clang 22 native provider

[clang22_native_provider.cpp](../../examples/sdk/clang22_native_provider.cpp) は明示 native package の callback/lifetime と
detachment boundary を示します。pointer は `detach_scalar` の constraint を満たさず、
[native_pointer_escape.cpp](../../examples/sdk/negative/native_pointer_escape.cpp) は compile fail します。AST/TU は
callback 外へ保存、所有、thread 移送せず、source range は `normalize_source` を通して detached source value にします。

## High-level recipe foundation

[recipe.cpp](../../examples/sdk/recipe.cpp) は versioned recipe を exact Logical Query IR へ lower し、必要 relation と
partiality を `explain()` で保持します。flagship `calls_to_function` と end-to-end engine execution は Issue #73 が
所有し、generic foundation の成功を flagship completion と解釈しません。

SDK 契約全体は `cxxlens-ng-sdk-contract-check`、公開 install consumer は `install.consumer` test で検証されます。
