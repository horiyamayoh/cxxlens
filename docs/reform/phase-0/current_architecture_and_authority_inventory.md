# Current Architecture & Authority Inventory

この inventory は Phase 0–1 の設計入力であり、実行ごとの結果、checksum、receipt、
qualification report ではない。現行 tree の責務境界と、各 semantics の normative owner
を記録する。実装表現や checkout は semantic authority ではない。

## 責務の境界

| 層 | 現行責務 | 主な consumer |
| --- | --- | --- |
| `cxxlens::sdk` | detached value、relation/claim、query、Store、portable provider、recipe | installed SDK consumer、薄い CLI |
| `cxxlens` CLI | SDK の診断・scaffold・CLI admission の入口 | 開発者、package consumer |
| relation registry | relation descriptor、identity、reference、merge、coverage の authority | dynamic registry、生成 static tag、query/store |
| query | Logical Query IR、typed/dynamic parity、実行結果と side channel | SDK query consumer、recipe |
| Store | immutable publication、Memory/SQLite backend、migration/recovery | query、installed consumer |
| Provider runtime | provider identity、protocol、transcript、coverage、structured terminal state | portable/native provider |
| Clang 22 native | callback-scoped observation と detached materialization | 明示 opt-in の native SDK |

Compiler-native object は native/provider 境界を越えて保存・所有・移送しない。core は
detached value と意味 metadata を扱い、provider の identity、provenance、coverage、
unknown、safety receipt を結果から落とさない。

## Normative owner

| Concept | Normative owner | Projection / verification |
| --- | --- | --- |
| Relation semantics | `schemas/cxxlens_ng_relation_registry.yaml` と registry schema | `tools/sdk/relation_idl_compiler.py`、generated `include/cxxlens/relations/*.hpp`、relation contract tests |
| Public admission | `schemas/cxxlens_ng_public_api_catalog.yaml` と catalog schema | installed headers、SDK contract、compile-fail tests |
| Logical query | query contract/IR schema と accepted query ADR | `src/sdk/query*.cpp`、query direct tests、schema checks |
| Provider wire/runtime | provider protocol/runtime schema と ADR 0107/0015/0038 系列 | shared codec、provider direct tests、bounded/structured failure checks |
| Source closure | source-closure transport/manifest schema と ADR 0101 | native source-closure tests、provenance and closure checks |
| Snapshot/Store | snapshot/store/SQLite contract schema と accepted Store ADR | Memory/SQLite C++ suites、migration/recovery/fault tests |
| Trust/security | security profile/certification schemas と security ADR | OpenSSL verifier、provider identity/sandbox tests |
| Support | `schemas/cxxlens_support_matrix.yaml` | support matrix checks; no duplicated baseline artifact |
| Architecture principles | integrated design and `docs/reform/` phase designs | explanatory guidance only; exact fields remain with the owners above |

Generated headers are never hand-edited. Documentation, fixture, and package projections may
explain or verify an owner, but do not silently become a second hand-maintained authority.

## Build profiles

- `dev-core` disables Clang 22 components and the adapter, and is intended to configure without
  Git/LLVM/Clang/ICU discovery.
- `dev-clang`/CI/release enable the exact native path when the capability is requested and retain
  source identity for package/release boundaries.
- `ctest --preset fast` is a compiler-neutral local smoke lane. It is not a qualification result
  and does not replace the complete deterministic suite or installed/native tests.
