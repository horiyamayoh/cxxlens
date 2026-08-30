# Real-project Corpus Specification

この corpus は通常の test で network access や秘密情報を要求しない。固定した fixture の
source、toolchain、build command を入力とし、実行結果を運用 artifact として保存しない。

## Phase 0–1 の corpus

| Corpus | 観測する能力 | 現行 fixture |
| --- | --- | --- |
| Small CMake project | package discovery、header/link closure、relocation | `tests/install/consumer` |
| Multi-TU project | static library と executable の境界、Memory/SQLite parity | `tests/install/real_project_consumer` (`main.cpp`, `model.cpp`, `model.hpp`) |
| Macro/template/virtual provider paths | provider callback、virtual sink、bounded process fixture、template helper | `examples/sdk/portable_provider.cpp`、`tests/fixtures/provider_process_fixture.cpp`、provider unit tests |
| Native materialization project | exact Clang 22 source closure、detached observation | `tests/install/clang22_sdk_consumer` と materializer install tests |

Phase 0–1 ではこの repository 内の deterministic fixture を使う。GCC-built corpus、MSVC
solution、外部 real-project collection は Phase 3 の別 gateで追加し、Phase 1 の native
support を暗黙に拡張しない。

## 固定する入力

- fixture revision と license は repository の履歴で管理する。
- toolchain family/version、target、linkage、build command は test/consumer contract に
  明示する。
- credential、個人情報、社内 absolute path を fixture に含めない。
- macro/template/virtual の有無を source inspection だけで推測せず、対応する compile/run
  test で確認する。
