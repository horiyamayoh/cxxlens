# M0 acceptance gate

Install Python dependencies from `tools/quality/requirements.txt`, `markdownlint-cli2`, Doxygen
1.9.8 or newer, Ninja, and a C++23 compiler. From a clean checkout, run:

```sh
CXX=clang++ cmake --preset m0-acceptance
cmake --build --preset m0-acceptance --target cxxlens-m0-acceptance
```

The target builds every M0 test binary, runs all unit/property/schema/golden/public-header/install
tests, Doxygen contracts and examples, static quality checks, then executes the 72-case
jobs/seed/order/root/repeat semantic matrix. It writes
`build/m0-acceptance/m0-acceptance-report.json` using the checked-in report schema.

On a semantic divergence, output includes the completion vector, catalog API IDs, fixture,
reproduction seed, and expected/actual SHA-256 digest. The completion manifest at
`schemas/cxxlens_m0_completion.yaml` maps every conformant M0 API and QR-001 through QR-010 to its
owned tests. Only the run metadata named there may be excluded from semantic comparison.

PR CI runs this target for static and shared libraries with Clang 22. A separate GCC job compiles
every ordinary public header without linking LLVM/Clang.
