---
cxxlens_document_status: archived
cxxlens_authority: non-normative
cxxlens_replacement: docs/design/cxxlens_next_generation_integrated_design_ja.md
cxxlens_removal_issue: "#72"
---
# Clang 22 frontend adapter contract

The frontend keeps compiler-native state inside one synchronous job. `src/llvm/common` owns
Clang-free invocation, diagnostic, coverage, and batch values; `src/llvm/clang22` is the only
production directory allowed to include Clang or LLVM headers. The explicit public exception is
`<cxxlens/interop/clang.hpp>`.

`CXXLENS_CLANG_ADAPTER` has three modes:

- `AUTO` links only an exact LLVM/Clang 22 installation and otherwise reports the capability as
  unavailable.
- `ON` requires exact LLVM/Clang 22 at configure time.
- `OFF` deliberately builds the unavailable adapter.

The component map lives only in `cmake/CxxlensClangTargets.cmake`. It names every required library;
the monolithic `clang-cpp` target and accidental transitive link closure are rejected by quality
checks.

Every parse constructs a new `ClangTool`, overlay VFS, frontend action, compiler instance, and AST
context. A `borrowed_clang_tu` is non-copyable and checks a thread-affine lifetime token. The token is
retired before callback return; retaining native references, moving them to another thread, or
suspending a coroutine is outside the contract. Detached diagnostics, coverage, and observations are
validated before return, and the semantic batch excludes the debug context identity.

Run both capability paths locally:

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang -R 'frontend|interop'

CXX=clang++-22 cmake -S . -B build/clang22 -G Ninja \
  -DCXXLENS_CLANG_ADAPTER=ON -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build/clang22
ctest --test-dir build/clang22 -R 'frontend|interop|clang22'
```
