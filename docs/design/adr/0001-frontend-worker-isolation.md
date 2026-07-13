# ADR 0001: Clang frontend worker process isolation

- Status: Accepted
- Date: 2026-07-13
- Decision owner: frontend/runtime

## Context

`ClangTool::run()` is synchronous and cannot safely be force-stopped from another host thread. A crash in the linked Clang frontend also terminates the library host. Pre/post cooperative checks therefore cannot satisfy bounded workspace execution or failure-as-data.

## Decision

Workspace scheduler parse/extraction runs in the installed `cxxlens-frontend-worker` subprocess. The parent sends a validated `cxxlens.frontend-worker-ipc.v1` request through anonymous memfd stdin and receives a detached batch/error through bounded stdout. The request binds virtual files, normalized compile command identity, fact profile, snapshot key, input fingerprint, and exact toolchain version.

The existing Linux/glibc `posix_spawnp()` process port owns timeout/cancellation, process-group termination, pipe limits, and leader reap. Signal/abnormal exit becomes `parse.crashed`; deadline becomes `parse.timeout`; cancellation becomes `core.cancelled`. Scheduler accounting remains per compile unit and preserves successful siblings.

No AST or Clang pointer crosses the IPC boundary. Malformed, oversized, wrong-version, or trailing IPC data fails closed. No temporary path is created.

## Consequences

`cxxlens-frontend-worker` must be installed in the configured binary directory and resolvable through `PATH`. The production isolation baseline is Linux + glibc. Other platforms return an explicit unavailable/platform failure until they provide an equivalent spawn adapter.

The borrowed `interop::with_translation_unit()` callback remains in-process because arbitrary C++ callbacks cannot be serialized. Its cancellation/deadline guarantee is cooperative; workspace fact/search execution uses the isolated boundary for bounded behavior.

## Verification

Production-process regression tests cover a 30-second blocking worker with a short deadline, cancellation during execution, a real SIGSEGV/SIGABRT worker, deterministic sibling continuation, balanced coverage, process disappearance, IPC validation, and ordinary production frontend bridging.
