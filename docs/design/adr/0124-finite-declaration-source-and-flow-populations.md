# ADR 0124: Finite declaration, source and local-flow populations

- Status: Accepted
- Contracts: `cxxlens.declaration-populations/1`, `cxxlens.source-comments/1`,
  `cxxlens.source-include-populations/1`, `cxxlens.body-flow-populations/1`
- Authority: NG integrated design, relation registry and public API catalogue
- Scope: detached native observations, installed public projections, monet-code
  census/readability/dataflow and cxxmonster include/structural inputs
- Stability: additive versioned pre-release surface

A successful query returning no rows cannot prove that a declaration, comment,
include or flow population is empty. These products need independently declared
finite populations in each compile unit, universe, variant and interpretation.
The native producer therefore records admission before identity/source failures,
and publishes cardinality, original member IDs and an enumeration state.
Traversal failure or producer resource exhaustion fails the partition. Missing
observations remain unknown; retained subsets are partial; incompatible counts,
sets, source bindings or candidates are conflicting.

`cc.declaration_inventory.v1` uses profile
`clang22-explicit-admitted-named-declarations/1`. It enumerates written named
declaration occurrences admitted by the frozen source closure. Template wrappers
which share their templated declaration's identity are excluded. Written lambda
call operators are included, although Clang marks those operators implicit.
Ordinary compiler-created declarations and implicit template instantiations are
excluded. Written explicit specializations/instantiation declarations/definitions
are separate categories. Every retained declaration binds to its actual canonical
entity, detached declaration source and same-unit entity detail. Repeated header
observations retain all units and sources without increasing an occurrence count.
Actual constexpr/consteval and evaluated exception specification facts carry a
category profile marker; unresolved exception specifications remain unknown.
Observation never requests instantiation or constant evaluation to fill a gap.
Actual preprocessor-entered files and their frozen snapshots are independently
listed as `parsed_files` and `parsed_source_snapshots`, with `file_state`.
Only these parsed source domains may prove declaration absence. Lexing a frozen
but unopened file does not make its declaration population complete.

`source.comment.v1` and `source.comment_inventory.v1` use
`clang22-frozen-raw-comments/1`. Clang's raw lexer runs with comment retention over
every frozen file, including inactive preprocessor regions and files not opened
in that unit. Each original comment has its actual source bounds, line/block kind,
byte count and ordinal. String and raw-string contents are never comments. Comment
observations do not alter the existing comment-excluding raw token population.
The inventory additionally freezes physical line starts using
`source.physical-lines.crlf-lf-cr/1`: CRLF is one terminator; LF and bare CR are
terminators; an empty file has zero lines; a final terminator adds no phantom line.
Starts are canonical unsigned 64-bit little-endian offsets, strictly increasing,
beginning at zero for nonempty files and less than the actual source byte length.
This permits source line/SLOC/clone mapping without platform newline assumptions.
Comments are observations, not inferred documentation/test coverage or semantics.

`source.include_inventory.v1` uses
`clang22-preprocessed-inclusion-directives/1`. It describes the actual finite
preprocessing inclusion-directive population for each opened frozen file in the
unit, including unresolved/external targets and header-guard-skipped inclusions.
Inactive directives are outside this explicitly selected profile. A frozen file
not preprocessed in the unit is unavailable, never complete empty. Enumeration
and target resolution are separate states. Admission is counted before source
identity exits, including missing include targets. All original `source.include`
IDs, source snapshots and actual target/resolution observations are retained.
Raw lexical directive counts cannot substitute for an include population.

`cc.flow_inventory.v1` uses `clang22-local-may-flow-facts/1`. It binds one actual
body/CFG traversal and declares the emitted local fact IDs and finite enumeration
state independently of alias, range, null and summary guarantees. Missing subject,
source or CFG-position bindings prevent a complete population. Explicit frontier
facts remain members and prevent the corresponding semantic facet from becoming
complete merely because enumeration succeeded. Actual local variable/parameter
and referenced member subjects form separate typed pointer/integer candidate
sets; their finite candidate census never certifies a points-to/null/range result.
Bodyless and dependent functions remain unavailable with actionable reasons.
Events follow the compiler's potentially evaluated expression contexts.
Unevaluated sizeof/alignof, noexcept, type traits, requirements, decltype and
non-variably-modified typeof operands do not become uses, writes or calls through
an enclosing CFG node. Compiler evaluated VLA and polymorphic typeid operands
remain eligible. Discarded constexpr branches and nested callable bodies are
excluded from the enclosing flow; lambda capture initializers belong to that
enclosing flow, and the actual call operator has its own body population.

The installed `project_declarations`, `project_source_comments`,
`project_source_includes` and `project_body_flow` projections own detached members,
conditioned joined entity/detail/source evidence and independently selected units.
They validate counts and references, same-unit/world bindings, snapshot bounds,
category profiles, equal and contradictory candidates, and extra observed members
within the declared population. All original plans, coverage, unresolved/conflict,
guarantee and provenance channels remain in the source query bundle. Local finite
enumeration never establishes relation-wide or uncompiled-project closure. Raw
callers must explicitly supply scan completeness; its default is false.

Implementation proceeds through schema and producer, portable projections, both
consumer calculations, then ordinary report/scene paths. Limits precharge rows,
condition expansion, retained bytes, set decoding, evidence references, work and
cancellation before retention. Tests cover empty/nonempty/dependent/implicit and
explicit templates, repeated headers, wrong counts/members/world/unit/snapshots,
missing bindings, CRLF/LF/CR/comments/raw strings, inactive and unopened sources,
unresolved includes, local flow frontiers, reordering, budgets and cancellation.
No compiler pointer escapes. These are semantic product inputs; no certificate,
test-run ledger, release admission or Git revision enters these contracts.
