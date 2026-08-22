# ADR 0072: CI supply-chain provenance

- Status: accepted
- Date: 2026-07-18
- Issue: #141

## Context

CI downloaded `llvm.sh` at execution time and ran it as root. Although external
Actions were already commit-pinned, LLVM packages, transitive Python
dependencies, the Python patch release, and the hosted runner image were not
bound to one reproducible authority. A source revision could therefore pass
against different compilers or dependency graphs without changing the tree.

## Decision

`tools/ci/llvm22-noble.lock.json` is the only CI bootstrap authority. The
repository signing key is accepted only after both its SHA-256 digest and
primary OpenPGP fingerprint match. The key is installed before an isolated APT
metadata refresh. Every requested LLVM package uses an exact epoch-qualified
version and is downloaded without root, checked against its locked SHA-256, and
only then installed. Missing packages, versions, or bytes fail; no alternate
suite, major, or first candidate is selected. CI never downloads or executes a
remote bootstrap script.

The documentation profile uses the same fail-closed order for the exact Ubuntu
24.04 Doxygen package: download without root, verify the locked SHA-256 and
Debian package/version/architecture fields, install, then assert the executable
release. The installed version is used by the documentation check only; CI does
not create a separate toolchain-provenance record.

Python 3.12 is patch-pinned by the workflow. Direct and transitive quality
dependencies are exact, binary-only, and hash-bound in
`tools/quality/requirements.lock`; CI always uses `--require-hashes` and
`--only-binary=:all:`. External Actions must equal the full commit revisions in
the supply-chain lock.

The lock is used only while installing and checking the CI dependencies. CI
does not emit a repository-side toolchain-provenance record, pair test output
with an artifact, or copy a revision/checksum into a qualification report.
GitHub's ordinary job log remains the only CI execution output.

`tools/quality/check_ci_supply_chain.py` owns the static contract. It rejects a
mutable/unknown Action, `llvm.sh`, direct network shell bootstrap, unpinned APT
requests, unhashed Python installation, generic Python minor selection, missing
bootstrap profiles, and incomplete lock/profile wiring. A cached package set may
be replayed without resolution; the lock remains the authority and a missing
cached artifact is an error, never permission to select another version.

## Consequences

- Signing-key substitution is rejected before any root filesystem effect.
- LLVM patch updates, Action updates, Python dependency updates, and runner
  changes remain explicit changes to the bootstrap lock and its tests.
- Upstream removal or mirror outage fails closed instead of silently changing
  the toolchain.
- Updating the toolchain requires an intentional lock and test update; it does
  not create a separate release-evidence record.
