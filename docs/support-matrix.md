# Support matrix

| Surface | State |
|---|---|
| Language | C++23 |
| Host | Linux primary |
| Core target DAG | implemented and installed |
| Relation/claim kernel | implemented |
| In-memory and SQLite immutable snapshots | implemented with semantic parity |
| Typed/dynamic Logical Query runtime | implemented reference engine |
| Portable provider SDK/runtime | implemented |
| Clang 22 native SDK/worker | production-supported only for exact tuples in a passed GR report |
| Recipe foundation | implemented |
| Flagship `calls_to_function` end-to-end recipe | implemented with typed/dynamic/provider/store parity |
| GCC/IR/object/binary providers | future provider entries |
| Stable third-party C++ binary plugin ABI | unsupported |
| Source mutation apply | outside NG0 scope |

Support は provider ID/version/binary digest/relation/interpretation/toolchain/platform tuple と conformance evidence
で決まり、library version 一つから推測しません。machine-readable authority は
[provider support matrix](../schemas/cxxlens_ng_provider_support_matrix.yaml) です。

Distribution 1.0 の production claim は
`cxxlens.ng-release-qualification-report.v1` に列挙された tuple だけが authority です。source-tree matrix の
`conformance-only` entry は認定候補を表し、`binary_digest: pending`、広い platform 名、wildcard から production support を
推測しません。report の各 tuple は exact installed worker digest、Clang toolchain identity、Linux architecture と static/shared
configuration、relation、`cc.clang22-canonical-1` interpretation、capability、guarantee、security profile digest、evidence digest を
保持します。report にない provider、relation、platform、binary rebuild は unsupported です。
