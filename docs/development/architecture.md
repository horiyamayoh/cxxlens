# Development architecture

## Target DAG

```text
cxxlens::base
    ↓
cxxlens::kernel
    ↓
cxxlens::query
    ↓
cxxlens::cpp

cxxlens::query → cxxlens::recipes
cxxlens::cxxlens = base + kernel + query + cpp
cxxlens::cxxlens + cxxlens::recipes → cxxlens::provider_sdk
cxxlens::provider_sdk + exact Clang 22 → cxxlens::clang22_provider_sdk
```

`cxxlens::provider_sdk` は relation、snapshot、query、provider、testing、recipe を束ねる高水準 author SDK です。
`base/kernel/query/cpp/recipes/provider_sdk` は compiler-native type を public surface に露出しません。
native target だけが Clang major に opt-in します。

## Semantic flow

```text
project catalog
  → provider observation
  → descriptor/schema validation
  → assertion / canonical claim / derived claim
  → partition validation
  → immutable snapshot publication
  → logical query result
  → recipe or analysis service
```

query result は row だけでなく coverage、closure、unresolved、conflict、guarantee を保持します。
closure certificate のない absence は unknown であり、empty と unresolved を混同しません。
NG1 の `anti_join` は right coverage と applicable closure が揃った場合だけ absence row を返します。
incremental materialization は source/dependency/invocation/toolchain/provider/descriptor/model/precision を含む exact input
fingerprint で partition 単位に再利用を決め、全件 exact reuse の場合だけ warm-zero を宣言します。bounded closure は
budget 超過時も positive row/evidence を保持しますが closure を認定しません。

## Invariants

- central `fact_kind`、opaque custom payload、use-case profile、旧 selector/reducer authority を導入しない。
- semantic identity は versioned canonical tuple から作り、name、pretty string、pointer、時刻、arrival order を使わない。
- unordered iteration order を serialization、ID、acceptance comparison に使わない。
- provider は detached value だけを process/kernel 境界へ渡す。
- AST/TU は callback-scoped borrowed object とし、保存・所有・thread 移送しない。
- provider selection、toolchain、variant、fallback は明示し、first-wins や silent fallback を行わない。
- mutation/generation を追加する場合は plan、独立 validator、dry-run、transaction の順を守る。

詳細は [次世代統合設計](../design/cxxlens_next_generation_integrated_design_ja.md) と
[Public API catalog](../../schemas/cxxlens_ng_public_api_catalog.yaml) を参照してください。新しい relation、API、
recipe、provider を追加する具体的な順序は [Extending the platform](extending-platform.md) にあります。
