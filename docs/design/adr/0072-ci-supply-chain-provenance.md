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
release. Doxygen is recorded in tool and package provenance when installed.

Python 3.12 is patch-pinned by the workflow. Direct and transitive quality
dependencies are exact, binary-only, and hash-bound in
`tools/quality/requirements.lock`; CI always uses `--require-hashes` and
`--only-binary=:all:`. External Actions must equal the full commit revisions in
the supply-chain lock.

Every evidence-producing job emits `cxxlens.toolchain-provenance.v1` containing
the source revision/tree, runner image identity, OS/kernel, action revisions,
tool binary digests, exact installed LLVM packages, installed Python
distribution RECORD digests, and both lock digests. The foundation completion
report validates and summarizes the same-SHA records. Release-layout artifacts
remain paired with their provenance/evidence artifacts, so source and toolchain
identity cannot be substituted independently.

`tools/quality/check_ci_supply_chain.py` owns the static contract. It rejects a
mutable/unknown Action, `llvm.sh`, direct network shell bootstrap, unpinned APT
requests, unhashed Python installation, generic Python minor selection, missing
bootstrap profiles, and incomplete provenance wiring. A cached artifact set may
be replayed without resolution; the lock remains the authority and a missing
cached artifact is an error, never permission to select another version.

## Consequences

- Signing-key substitution is rejected before any root filesystem effect.
- LLVM patch updates, Action updates, Python dependency updates, and runner
  changes become explicit provenance changes.
- Upstream removal or mirror outage fails closed instead of silently changing
  the toolchain.
- Updating the toolchain requires an intentional lock, test, contract, and
  provenance update.
