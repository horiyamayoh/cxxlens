# Runtime ports

Domain code obtains filesystem, process, time, and digest services through the interfaces in
`src/runtime`. These interfaces are internal and Clang-free; OS handles and adapter types never enter
the public headers.

Each operation receives a `request_context` with a stable operation name, caller-assigned call index,
cancellation token, optional steady deadline, and output limit. A fault plan matches the operation and
call index, so injected behavior does not depend on which worker happens to complete first.

Process requests contain an argv vector. The production adapter passes each element directly to
`execvp`; it has no command-string path and rejects `shell_allowed=true`. Environment changes are
explicit name/value pairs. Filesystem enumeration is deliberately not canonical: consumers call
`canonical_path_order` before using rows in a semantic projection. Root checks compare normalized path
components rather than string prefixes.

Hash requests always name the algorithm, algorithm version, and domain. The provided constructor uses
`fnv1a64` version 1 and still requires a non-empty domain. Time is a separate service and must not be
added to a semantic hash request unless a higher-level contract explicitly defines time as semantic
input. The fixed clock exists for deterministic operational tests.

Downstream code must not add convenience calls to `std::filesystem`, process shells, ambient clocks, or
`std::hash`. Extend the owning port contract through a dependency request when an operation is missing.
Production and fake adapters must continue to share cancellation, deadline, output-limit, failure
mapping, path, and argv conformance tests.
