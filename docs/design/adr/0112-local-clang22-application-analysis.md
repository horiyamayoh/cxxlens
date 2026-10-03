# ADR 0112: Local Clang 22 application analysis

Status: Accepted

CXXMONSTER needs functions, direct calls, build context and source locations from
one actual compilation variant. An installed library command accepts a project
root, a compilation database and an optional explicitly selected source file.
It rejects ambiguous variants and unsupported compiler inputs with actionable
errors. The initial route requires Clang 22 on Linux x86_64, with the compiler's
version matching the Clang frontend used to build the analyzer.

The command uses the existing Clang AST observer and canonical row normalizer,
then the public SDK claim, immutable Store and query APIs. Compiler-native
objects stay inside the callback. Project inputs are captured in an immutable
source closure; toolchain metadata comes from ordinary compiler probes. Unknown
references and partial extraction remain visible. Observed functions and calls
can be rendered without claiming that an incomplete graph proves absence.

The process output is `cxxlens.application-query-results.v1` (ADR 0111), with
independent scans of one immutable snapshot and all query side channels retained.
The local route uses the explicitly selected compiler and needs no provider
quality certificate, trust registry, tested Git SHA, or release attestation.
Normal input bounds, compiler compatibility, parse errors, and schema/reference
validation remain part of execution. This command runs the local compiler inside
its process; it does not claim a provider sandbox.

Verification covers a real compiler-to-query-to-scene path, ambiguous variants,
malformed or unsupported inputs, parse failure, and partial calls. Normal logs,
Git and regression tests are sufficient; no additional evidence ledger is made.
