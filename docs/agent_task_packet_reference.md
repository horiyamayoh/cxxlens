# Agent task packet reference

The task-packet corpus is the deterministic agent-facing projection of
`schemas/cxxlens_public_api_contract.yaml`. It does not make planned declarations exact, infer
ownership, or treat contract maturity as implementation readiness.

## Generate and verify

From the repository root:

```sh
python3 tools/agent/task_packet_generator.py generate --root .
python3 tools/agent/task_packet_generator.py check --root .
```

`generate` rewrites the canonical corpus and validation report. `check` independently regenerates
them, validates both JSON schemas and semantic digests, verifies frozen declaration source bytes,
and rejects stale checked-in output. `cxxlens-task-packet-check` runs this check and its negative
fixture suite. It is a dependency of `cxxlens-api-contract-check` and therefore of
`cxxlens-quality`.

## Corpus fields

Each API record contains the authoritative API and atomic-unit IDs, package/header, symbol, kind,
phase, contract maturity, implementation state, exact or unresolved declaration, signature/source
fingerprints, and a combined contract fingerprint. A coherent method, builder, factory, or free
function family records its entire surface under `family_contract`; the surface is indivisible.

`dependencies` keeps facts, capabilities, expressions and their concrete expansions, minimum
precision, API IDs, and derived atomic-unit IDs as separate typed fields. `traceability`, `behavior`,
and `quality_obligations` retain requirements, errors, guarantees, side effects, evidence, coverage,
unresolved, and schema obligations without manufacturing missing catalog facts.

Every record exposes positive, negative, and ambiguous fixture requirements. Existing test/example
paths are evidence candidates, not an inferred claim about which category they cover. Acceptance
commands are argument arrays plus explicit environment maps; consumers must not concatenate them
into shell command strings.

`generation.state` is copied from catalog readiness and is independent of contract maturity. An
unresolved declaration stays visible as `blocked` with a stable reason. Existing conformant entries
are `complete`; a later exact, unimplemented and dependency-ready entry may be `ready` only when the
catalog says so.

## Atomic units and coordination

The corpus-level `atomic_units` array gives exactly-once API membership and the complete family
surface. Issue #28 owns the separate file-ownership and shared-steward contract; packets reference
that version and issue but do not guess path owners. Multiple shared-steward references are rejected
as ambiguous.

Packet, corpus, and validation-report digests exclude timestamps and absolute checkout roots.
Package/API input order, checkout root, and process count therefore cannot alter semantic output.
Catalog, declaration source, signature, dependency, family, or schema drift invalidates the
checked-in corpus and requires regeneration.
