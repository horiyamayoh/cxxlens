# Testing fixture protocol

`workspace_fixture` owns only deterministic input construction. `materialize()` returns relative
files, variants, and an `arguments`-based compilation database for the production workspace
loader. It never creates AST nodes, semantic facts, findings, or a fake query backend. M1 connects
this bundle to `workspace::open` and the real frontend/reducer/store path.

Fixtures live below `tests/fixtures/<package>/{positive,negative,ambiguous}` and use schema
`cxxlens.testing.case.v1`. Discovery is lexical, rejects symlink escape and duplicate category/name,
and is available through the `fixtures.m0-discovery` test.

Property failures print `seed=<n>;case=<n>` plus the rendered failing input. Supplying the same seed
and `replay_case` executes exactly that case. Golden normalization is intentionally narrow: only
declared workspace/build/resource roots in path fields and known runtime metadata fields change.
IDs, source ranges, variants, reason codes, evidence, coverage, and original structured errors stay
visible.
