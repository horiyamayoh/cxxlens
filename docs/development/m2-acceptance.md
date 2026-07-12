# M2 flagship acceptance

The M2 gate proves that the installed package can execute the polymorphism-aware method-call
flagship through the production Clang 22 path. From a clean checkout, run:

```sh
CXX=clang++-22 cmake --preset m2-acceptance
cmake --build --preset m2-acceptance --target cxxlens-m2-acceptance
```

The target first runs the ordinary tests and quality checks. It then installs cxxlens to an isolated
prefix, builds `examples/m2-flagship` only against that installation, and executes the complete
declared perturbation matrix. The matrix covers jobs 1/2/8, scheduler seeds, forward/reverse file and
compilation-database order, relocated roots, repeated processes, memory/SQLite stores, and cold/warm
cache states.

The installed example uses only `<cxxlens/cxxlens.hpp>`. Its fixture has two translation units, two
variants, direct and macro-spelled calls to `Base::step`, the `Derived::step` override, and an
unrelated `Other::step`. A separate invalid translation unit verifies that known matches survive
partial coverage without an exact guarantee.

The authoritative manifest is `schemas/cxxlens_m2_completion.yaml`. It maps every exact M2 API once,
records all unresolved M2 catalog entries as explicit deferrals, and links #23 through #26 to their
requirements, invariants, tests, and fixtures. Together with the M0 and M1 manifests it covers every
foundation issue from #2 through #26.

The generated `m2-acceptance-report.json` conforms to
`schemas/cxxlens_m2_acceptance_report.schema.yaml`. It records artifact digests and bounded
performance counters. The cold fixture may schedule at most one parse task per compile unit; the
repeated warm query must schedule zero tasks, scan only the four flagship call facts, and request no
targeted refinement.
