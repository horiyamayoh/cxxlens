# Author SDK tutorials

Issue #66 で、recipe 利用者から native provider 作者まで同じ relation descriptor と Logical Query IR を使う
5経路が runnable example になりました。repository root で次を実行します。

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang -R '^(unit\.sdk|public-api\.(sdk-header|provider-clang22-header)|sdk\.)'
```

## Generated typed query

[typed_query.cpp](../../examples/sdk/typed_query.cpp) は generated `cc.call_site` と外部
`company.lock.acquire` の tag を使い、型付き join/order/project を同じ Logical Query IR へ lower します。
tag は accepted registry から生成できます。

```sh
python3 tools/sdk/relation_idl_compiler.py \
  --relation cc.call_site --output /tmp/cc_call_site.hpp
```

存在しない generated column は [generated_unknown_column.cpp](../../examples/sdk/negative/generated_unknown_column.cpp)
のように compile fail します。

## Runtime dynamic query

[dynamic_query.cpp](../../examples/sdk/dynamic_query.cpp) は relation name と explicit semantic major から standard/
custom descriptor を取得し、runtime column lookup 後に typed path と同じ `query::builder` を使います。文字列から数値列への暗黙 coercion
は [dynamic_literal_type_mismatch.cpp](../../examples/sdk/negative/dynamic_literal_type_mismatch.cpp) の structured error
になります。`logical_query_ir::canonical_form()` は独自形式ではなく、accepted
`cxxlens.logical-query-ir.v1` の canonical JSON を返します。

## Portable provider

[portable_provider.cpp](../../examples/sdk/portable_provider.cpp) は core enum/switch を変更せず generated
`company.lock.acquire` を追加し、`portable_provider::run` 内で `relation_sink` と
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

## Flagship call-search recipe

[recipe.cpp](../../examples/sdk/recipe.cpp) は `recipes::calls_to_function` を exact Logical Query IR へ lower し、
`cc.entity`、`cc.call_direct_target`、`cc.call_site` の exact requirements を事前取得します。`run()` が返す
`call_search_report` は plan と query result を所有し、match、empty-complete、empty-incomplete、ambiguous を区別
します。coverage、closure、unresolved、conflict、differential disagreement、producer contract、guarantee、
provenance、logical/physical explain は underlying query result から失われません。

production E2E は `integration.r2-vertical-slice`、SDK 契約全体は `cxxlens-ng-sdk-contract-check`、repository 内の
recipe/typed/dynamic/portable/native example を実際の install tree から再 build/run する gate は
`install.consumer` です。

## Relation/claim kernel

[relation_claim_batch.cpp](../../examples/sdk/relation_claim_batch.cpp) は Issue #67 の walking skeleton です。
external generated relation の typed builder と runtime descriptor を同じ immutable engine へ登録し、fake provider
observation を assertion に変換して atomic batch へ投入します。hard reference は同じ batch staging space の既存
claim で解決し、未 materialize の soft reference は row を捨てず `unresolved_reference` として返します。
`make_canonical_claim` と `make_derived_claim` を使う場合も同じ identity encoder と独立 validator を通ります。

## Immutable snapshot / store

[snapshot_store.cpp](../../examples/sdk/snapshot_store.cpp) は同じ validated partition を in-memory と SQLite に
publish し、backend に依存しない snapshot ID を確認します。`snapshot_series_selector` は catalog、channel、engine
generation、condition universe、registry、interpretation policy、trust policy の全 field が必須です。writer は
`stage` → `validate` → `publish` の順で使用し、staged claim は reader から見えません。`row_view` は cursor の次の
`next()` までだけ有効で、長寿命化には `copy()` を使います。

SQLite の path、publication sequence、physical generation は semantic ID に含まれません。current head の破損や
stale parent は structured error となり、別 series や prior publication への暗黙 fallback は行いません。

## Deterministic query execution

[query_execution.cpp](../../examples/sdk/query_execution.cpp) は typed Logical Query IR を immutable snapshot に bind
し、memory/SQLite の両 backend で同じ annotated semantic rows を得る reference runtime の最小例です。query
result は execution success と input completeness/closure を分離し、coverage、unresolved、conflict、
differential disagreement、exact producer semantic contract、guarantee、logical/physical explain を保持します。
