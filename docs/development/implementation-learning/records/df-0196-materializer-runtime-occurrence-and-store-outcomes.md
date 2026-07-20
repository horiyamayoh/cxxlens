---
id: DF-0196
title: Bind installed materializer occurrence and Store outcomes
status: observed
kind: missing-assumption
impact: security
confidence: high
implementation_disposition: blocked
scope:
  - provider.clang22-materializer-cli-effect-root
  - provider.clang22-materializer-installation-binding
  - release.materialization-occurrence-evidence
  - store.materialization-publication-outcome-mapping
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0013-ng-sqlite-physical-store.md
  - docs/design/adr/0096-clang22-installed-materialization-boundary.md
  - schemas/cxxlens_ng_install_artifact_manifest.schema.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
  - schemas/cxxlens_ng_sqlite_store_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_contract.yaml
  - schemas/cxxlens_ng_clang22_materialization_request.schema.yaml
  - schemas/cxxlens_ng_clang22_materialization_report.schema.yaml
tracking_issue: '#196'
implementation_issues:
  - '#181'
resolution_refs: []
review:
  mode: independent
  status: pending
  author: codex-agent-runtime-authority-df
  reviewer: null
  refs: []
created: '2026-07-20'
---

# Bind installed materializer occurrence and Store outcomes

## Observation

The materialization request asserts source revision/tree, package configuration, installed tool and
worker byte digests, a prefix-manifest digest, and a relocated-prefix digest. The detailed report
mostly repeats those caller values. No accepted runtime contract identifies a co-located manifest or
defines how the running tool discovers its actual prefix, measures its own executable and the exact
sibling worker, locates the installed authority files, and compares the resulting occurrence to the
request before any worker or Store effect.

The existing install-artifact manifest is an external qualification artifact. The install test writes
it outside the relocated prefix after installation. It can authenticate the prefix to the release
harness, but the installed materializer cannot discover or validate it from its own execution
occurrence. Installing the same full-prefix manifest naively would also create a self-reference if its
own bytes participate in the file inventory it hashes.

The process effect surface has two further gaps. `publication.sqlite_path` must be relative, but the
contract does not state whether it is relative to the startup current directory, installed prefix,
project root, or another effect root. It does not bind normalization, symlink traversal, escape, or
the resolved operational occurrence in a receipt. The machine surface also says one JSON request on
stdin and one response on stdout without defining the accepted `argc`/argv shape or the outcome for
unexpected options and operands.

The publication error table is not implementable against the public SDK. It names
`store.corrupt-publication-identity`, `store.corrupt-persisted-authority`, and
`store.sqlite-failure-after-reopen`, but the Store has no such error codes. Actual failures use
`error{code, field, detail}`: publication/persistence corruption uses `store.corrupt` with a typed
field and detail, and SQLite I/O uses `store.sqlite-failure` with `field` equal to `open` or
`database` plus a diagnostic detail. A materializer cannot preserve the first SDK cause while also
reporting one of the invented codes. Parsing SQLite diagnostic prose to fabricate a phase would
violate the no-prose-control invariant.

## Working mental model

The running installed process is a physical occurrence that must be authenticated independently of
the request. A source-private occurrence manifest should be discoverable from the measured executable
location, use prefix-relative paths, avoid a digest cycle by an explicit exclusion/layering rule, and
bind revision/tree, configuration, tool/worker bytes, and installed authority bytes. The external
release manifest remains a separate witness that cross-binds the same occurrence.

`argv[0]`, PATH, and request fields are observations, not path authority. The tool should have one
closed argv contract, resolve the actual executable and sibling worker through a filesystem port, and
reject any shadowed or escaping path. The SQLite path is operational rather than semantic, but its
effect root and resolution policy must still be explicit and stable for the invocation.

Store outcome classification belongs at an authenticated API call boundary. The classifier can use
the operation name and exact SDK `error{code,field,detail}` tuple, or a tool-private typed receipt
constructed at that boundary. It must not invent public codes or infer commit state from diagnostic
text. `store.sqlite-failure` remains phase-opaque wherever the SDK cannot prove whether COMMIT became
durable; read-only recovery evidence does not retroactively prove invocation authorship.

## Mismatch or opportunity

The current request and report can be internally self-consistent while naming a different binary,
worker, authority set, prefix, or SQLite target than the actual process used. That is insufficient for
the installed and relocated production evidence required by G4/GR. The nonexistent Store error codes
also prevent an exhaustive implementation of the accepted detailed publication outcome union.

This is one coherent boundary: authenticating the installed occurrence and effect operation is what
makes the subsequent worker/Store receipt attributable to this invocation. Splitting manifest/path
and Store mapping into separate feedback would duplicate the same occurrence/effect authority and
allow an apparently typed Store result to remain unauthenticated.

## Evidence

- `schemas/cxxlens_ng_clang22_materialization_request.schema.yaml` requires the physical installation
  digest fields but contains no manifest path or effect-root field.
- `schemas/cxxlens_ng_clang22_materialization_report.schema.yaml` records installation digests and
  booleans but no measured occurrence manifest or path-resolution receipt.
- `tools/quality/install_artifact_manifest.py` constructs and verifies an external manifest from a
  caller-supplied prefix. `tests/install/run_install_test.cmake.in` stores that manifest outside the
  prefix after relocation.
- `CMakeLists.txt` currently installs `cxxlens-clang-worker-22`; it has no
  `cxxlens-clang22-materialize` executable, occurrence manifest, or materializer-relative lookup
  target yet.
- The request schema and `src/llvm/clang22/materialization_request.cpp` accept a relative SQLite path
  while leaving its base to a later Store open call.
- `open_sqlite_snapshot_store()` passes the received string to SQLite. A relative path therefore uses
  ambient process path resolution unless the materializer establishes a fixed effect-root policy.
- `schemas/cxxlens_ng_clang22_materialization_contract.yaml::sdk_publish_mapping` names SDK codes that
  do not occur in `src/sdk/store.cpp`.
- `src/sdk/store.cpp::validate_publication_identity()` returns
  `store.corrupt / publication / identity`; persisted authority validation returns other
  `store.corrupt` field/detail tuples; SQLite open/execute/query return
  `store.sqlite-failure / open|database / <diagnostic>`.
- The snapshot and SQLite Store contracts reserve `store.corrupt` for persisted identity/topology
  corruption, `store.counter-overflow` for checked counters, and
  `store.publication-conflict` for SQLite parent CAS. They do not define the invented specialized
  codes.
- Issue #181 requires relocated actual-source execution, exact installed tool/worker/revision/tree
  binding, file-backed reopened SQLite, phase-authentic Store outcomes, and external execution
  receipts.

## Alternatives and trade-offs

1. Introduce a co-located, non-circular occurrence manifest and exact executable-relative discovery;
   close argv; define one startup effect root and safe relative SQLite resolution; and replace the
   invented error table with an exhaustive actual-SDK operation/outcome mapping. This makes the
   installed run independently attributable without adding public C++ API.
2. Trust request values inside the tool and rely solely on the external release harness. The external
   evidence can detect some drift after execution, but it does not authenticate the worker and Store
   effects at their boundary.
3. Resolve the worker through PATH, trust `argv[0]`, and let SQLite use ambient current-directory
   behavior. This is vulnerable to shadowing or invocation-context drift and conflicts with the
   accepted no-fallback installation rules.
4. Add new public Store error codes solely to match the materialization table. This broadens public
   semantics and still does not identify the API operation or prove commit attribution. A private
   typed mapping/receipt is preferable unless Store itself needs a general outcome contract.

## Recommendation

Amend the installed materialization authority with one exact occurrence/effect contract. The current
recommended hypothesis is:

- install a versioned occurrence manifest at one fixed prefix-relative path;
- define a two-layer or explicit-exclusion digest algorithm so the occurrence manifest does not hash
  itself, while the external install-artifact manifest still covers the complete delivered prefix;
- derive the prefix from the actual executable object/path through a source-private filesystem port,
  require the exact materializer relative path, and locate the worker and authority files only by
  manifest-declared relative paths beneath that prefix;
- measure and compare revision/tree, configuration, manifest/prefix, tool, worker, and authority
  digests before worker launch; report the measured occurrence separately from request assertions;
- accept no command-line operands or options, treat `argv[0]` as non-authoritative, and specify an
  exact no-response/exit receipt for invalid process invocation before a JSON request is authenticated;
- capture one startup effect-root handle for SQLite, require a canonical relative path with no empty,
  dot, dot-dot, absolute, NUL, or escape components, resolve beneath that handle without ambient
  fallback, and retain the operational resolution result outside snapshot semantic identity;
- classify each Store call using `{authenticated operation, exact SDK code, exact field, exact stable
  detail when authority declares it}`; retain human SQLite detail only as diagnostic evidence;
- replace the nonexistent error names with actual SDK tuples, and keep any publish-time
  `store.sqlite-failure` outcome unknown unless an SDK-authentic commit receipt proves otherwise.

The normative resolution should update the integrated design, ADR 0096, the installation artifact or
new occurrence-manifest schema, materialization request/report/contract schemas, install/relocation
tests, and materialization checker negatives. Snapshot/SQLite Store authority should change only if
review finds that the existing SDK tuple cannot provide the required phase-authentic outcome; no
public Store signature change is assumed.

Independent review should reject the resolution unless it proves all of the following:

- request-only spoofing of revision/tree/configuration/tool/worker/prefix/authority digests fails
  before worker or Store effects;
- relocation preserves prefix-relative discovery and build-tree/PATH shadowing is rejected;
- the occurrence manifest's digest graph is acyclic and the external full-prefix witness cross-binds
  it exactly;
- unexpected argv cannot be confused with a schema-valid materialization response;
- relative SQLite paths have one captured base and cannot escape through lexical or symlink aliases;
- all actual SDK publish/open/current errors map exhaustively without diagnostic prose parsing;
- `store.sqlite-failure` is never downgraded to a zero-commit claim solely from candidate absence;
- successful SDK calls followed by projection mismatch retain expected/actual digests and do not
  fabricate a `store.*` failure;
- installation/path occurrence fields remain outside semantic snapshot and backend parity identity.

## Disposition

2026-07-20: Opened as a blocking security/attribution assumption discovered during Issue #181
installed runtime planning. `resolution_refs` and canonical review refs are intentionally empty until
the normative occurrence/effect contract is amended and an independent reviewer posts a concrete
review on Issue #196. Pure codec, claim, and Store-mechanics work that does not claim installed
occurrence or publication outcome authority may continue.
