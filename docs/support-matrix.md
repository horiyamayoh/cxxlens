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
| Clang 22 native SDK/worker | implemented; exact adapter requires exact Clang 22 |
| Recipe foundation | implemented |
| Flagship `calls_to_function` end-to-end recipe | implemented with typed/dynamic/provider/store parity |
| GCC/IR/object/binary providers | future provider entries |
| Stable third-party C++ binary plugin ABI | unsupported |
| Source mutation apply | outside NG0 scope |

Support は provider ID/version/binary digest/relation/interpretation/toolchain/platform tuple と conformance evidence
で決まり、library version 一つから推測しません。machine-readable authority は
[provider support matrix](../schemas/cxxlens_ng_provider_support_matrix.yaml) です。
