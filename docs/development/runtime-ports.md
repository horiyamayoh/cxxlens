# Runtime ports

Kernel/provider runtime code obtains filesystem, process, time, and digest services through explicit
ports. These interfaces are Clang-free; OS handles and adapter types never enter stable semantic headers.

Each operation receives a `request_context` with a stable operation name, caller-assigned call index,
cancellation token, optional steady deadline, and output limit. A fault plan matches the operation and
call index, so injected behavior does not depend on which worker happens to complete first.

Process requests contain an argv vector. The production adapter passes each element directly to
`execvp`; it has no command-string path and rejects `shell_allowed=true`. Environment changes are
explicit name/value pairs. Filesystem enumeration is deliberately not canonical: consumers call
`canonical_path_order` before using rows in a semantic projection. Root checks compare normalized path
components rather than string prefixes.

Hash requests always name the algorithm, algorithm version, and domain. The current migration adapter uses
`fnv1a64` version 1 and still requires a non-empty domain; this does not decide the vNext identity algorithm.
Time is a separate service and must not be
added to a semantic hash request unless a higher-level contract explicitly defines time as semantic
input. The fixed clock exists for deterministic operational tests.

Code must not add convenience calls to `std::filesystem`, process shells, ambient clocks, or `std::hash`.
Extend the owning port contract through the responsible NG issue when an operation is missing.
Production and fake adapters must continue to share cancellation, deadline, output-limit, failure
mapping, path, and argv conformance tests.

Compiler frontend、third-party native provider、high-RSS solver は process isolation required です。worker
crash、timeout、malformed/oversized output は prior published snapshot を破壊せず、coverage/unresolved と
sandbox assurance へ反映します。
