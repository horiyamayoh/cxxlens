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
