# Contract Preservation Map

Phase 1 は contract の意味、identity、wire、persisted format、trust semantics を変更しない。
この map は変更 class と保全試験の対応を示す設計入力であり、qualification report ではない。

| Boundary | Preserved contract | Current authority | Required regression | Change class |
| --- | --- | --- | --- | --- |
| SDK/public headers | detached value、lower snake case、LLVM-free core header、stable result state | public API catalog、installed headers | public header/link closure、compile-fail、installed consumer | `CH-0` |
| Relation | descriptor identity、references、merge/conflict、coverage/closure | relation registry/schema | relation contract、IDL `--check`、static/dynamic examples | `CH-0` |
| Query | typed/dynamic parity、budget、ordering、unknown/partial result | query IR/runtime contracts | query direct tests、negative/fault vectors、Golden | `CH-0` |
| Provider protocol | bounded decode、frame/session binding、structured terminal failure | provider protocol schema、ADR 0107 | C++ protocol adapter/process tests、schema boundary | `CH-0` |
| Provider trust | identity、signature/certification、revocation、sandbox classification | security/provider schemas and ADRs | verifier、runtime、hardened provider tests | `CH-0` |
| Source closure | detached source identity、coverage、closure、read-only safety receipt | source-closure schemas、ADR 0101 | native source-closure/transport tests | `CH-0` |
| Store | immutable publication、failed isolation、digest、migration、prior snapshot | snapshot/SQLite schemas and Store ADRs | Memory/SQLite C++ suites、fault/recovery tests | `CH-0` |
| Build profile | core configure without native dependencies; native unavailable is structured | CMake option/presets and support matrix | `configure.dev-core-no-dependencies`、core/native CTest | `CH-0` |
| Known downstream | old provider SDK consumers are not silently broken or waived | read-only census and explicit Phase gate | migration only after separate permission | outside Phase 1 scope |

Product runtime provenance、coverage、unknown、materialization report、SQLite/source-closure
safety receipt は development/release の運用証跡削除の対象ではない。
