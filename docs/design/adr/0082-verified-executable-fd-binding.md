# ADR 0082: verified executable FD binding

- Status: Accepted
- Date: 2026-07-18
- Owner: Issue #151

## Context

The Linux provider process adapter hashed an executable by path in the parent and later resolved the same path string again with `execve` in the child. A relative path was resolved before the child working directory was applied, and absolute paths remained vulnerable to rename, symlink, and in-place replacement between measurement and execution.

## Decision

The adapter resolves a relative executable against the requested working directory and opens the source exactly once. It copies the opened bytes to a dedicated executable memfd, applies write/grow/shrink/seal seals, hashes the exact copied image, and compares that digest with selection authority before fork.

The child retains the verified image as descriptor 3 while closing all other inherited descriptors, applies the working directory and sandbox controls, and executes descriptor 3 with `execveat(AT_EMPTY_PATH)`. It never resolves the executable path after measurement. A concurrent path replacement therefore either occurs before the single open and fails digest validation, or occurs after open and cannot change the sealed image that executes.

Sandbox evidence domain `cxxlens.provider-sandbox-evidence.v3` binds the measured executable digest in addition to resolved policy, achieved assurance, budget, and exact applied mechanisms. Runtime recomputation uses the selected manifest binary digest; mismatched evidence is not accepted as enforced.

## Consequences

- Relative paths measure and execute the target under the requested working directory.
- Rename, symlink, and in-place mutation cannot substitute code after measurement.
- The verified executable FD is an intentional child descriptor until exec and is close-on-exec.
- Script executables requiring a persistent `/proc/self/fd` interpreter path are not part of this provider executable contract.
