# Development architecture

`cxxlens` の安定核は use-case API 群ではなく、versioned relation、semantic claim、immutable snapshot、
logical query、provider contract です。

```text
Applications / Recipes
        ↓
Semantic Services / Analysis Modules
        ↓
Static DSL + Dynamic DSL
        ↓
Versioned Logical Query IR
        ↓
Semantic Relation Kernel
        ↓
Provider Protocol + Runtime Ports
        ↓
Process-isolated native providers
```

## Dependency rules

- kernel は Clang AST、GCC tree、LLVM IR、lint rule、特定 relation の列内容を知らない。
- provider implementation は SDK/protocol に依存し、kernel は provider implementation を link しない。
- 新 relation/provider/recipe は central enum、switch、registry source list の変更を要求しない。
- stable public semantic header は compiler-native type layout に依存しない。
- compiler-native object は provider job/callback/thread の外へ出さない。
- filesystem、process、time、digest は runtime port を通す。

## Product boundary and public targets

language-neutral な `relation-kernel` / `logical-query`、C/C++ project semantics、portable provider SDK、
major-specific native provider、recipes を別 component として扱います。kernel/query から C/C++ semantics
や native provider へ依存する辺は禁止です。これにより typed query や provider 下位層の開発者も、
Clang AST lifetime や provider implementation の link dependency を負わずに契約を利用できます。

1.0 の予定 public target は `cxxlens::base`、`cxxlens::kernel`、`cxxlens::query`、`cxxlens::cpp`、
`cxxlens::provider_sdk`、`cxxlens::recipes` です。Issue #66 で `cxxlens::provider_sdk` は標準ライブラリだけに依存する
独立 package として先行実装され、Clang opt-in は `cxxlens::clang22_provider_sdk` に分離されました。残る
`base/kernel/query/cpp/recipes` と aggregate の target DAG は Issue #67 の migration 対象であり、現在の legacy
`cxxlens::cxxlens` target が完成形という意味ではありません。

author SDK の5経路、exact signature、lifetime/threading は
[Public C++ API Catalog](../../schemas/cxxlens_ng_public_api_catalog.yaml) を参照してください。

正確な boundary、target DAG、stability tier は
[release bundle](../../schemas/cxxlens_ng_release_bundle.yaml) を参照してください。

## Semantic data flow

```text
Project Catalog + Source Snapshot + Condition Universe
  -> provider observation
  -> schema validation
  -> assertion
  -> canonical claim / derived claim
  -> partition validation
  -> immutable snapshot publication
  -> logical query result
```

query result は rows だけでなく input coverage、closure、unresolved、conflict、summary guarantee を保持します。
成功は complete/closed を意味せず、closure certificate のない absence は unknown です。

## Identity and ordering

semantic key、assertion、content digest を分離し、versioned length-prefixed canonical tuple から生成します。
absolute root、pointer、timestamp、PID、task/provider arrival order、hash iteration、display prose は semantic
identity に含めません。relation と query result は unordered が既定で、`order_by`、canonical export、
digest construction、acceptance comparison だけが順序を保証します。

## Migration boundary

現在の `src/facts`、旧 selector/query plan、linked Clang adapter、M0/M1/M2 gate は移行 baseline です。
[asset migration ledger](../../schemas/cxxlens_asset_migration_ledger.json) の disposition に従い、Issue #72 で
production path から除去します。新規機能を旧 package/fact surface に追加してはいけません。

正確な invariant は [次世代統合設計書](../design/cxxlens_next_generation_integrated_design_ja.md)、各 contract の
状態は [catalog index](../design/catalogs/README.md) を参照してください。
