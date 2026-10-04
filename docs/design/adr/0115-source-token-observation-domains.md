# ADR 0115: Compiler token observations and finite source domains

- Status: Accepted
- Contract-ID: `cxxlens.source-token-observation-domains.v1`
- Authority: ADR 0113, relation registry, frozen source/span identity
- Stability: versioned LLVM-free public relations; Clang 22 private producer
- Write scope: registry/tags, private compiler observer, both consumer adapters and tests

Monet needs physical/preprocessor/expanded token measurements, lexical clone inputs,
readability and template facts. Monster needs preprocessor/template mass and source
explanations. Neither application should build a separate Clang token observer.
Existing syntax nodes and preprocessor events cannot count the token stream or prove
that an absent local token is zero. The compiler adapter must detach token values and
explicit finite source inventories while the compiler job owns its objects.

`source.token.v1` identifies a token by compile unit, content-bound source span,
phase, ordinal and profile. It retains compiler kind, spelling, macro expansion state,
optional active-state, optional template-state and optional directive-start state.
The phases are `raw` and `expanded`. A directive start is the raw hash token at
the start of a logical preprocessing line; hashes inside macro replacements do
not start another directive. Expanded tokens have false directive-start state.
Raw lexical tokens exclude comments, EOF and whitespace; their byte range is the
spelling range in the frozen input. Expanded tokens come from the compiler's final
preprocessor token stream and retain the expansion range in project input, including
many distinct token identities at a shared macro invocation range. Missing source
locations remain explicit unresolved items and prevent complete expanded enumeration.
Spelling belongs to source content and is subject to each consumer's privacy policy.

`source.token_inventory.v1` identifies one compile-unit/source-snapshot/profile/phase
and retains token count, directive count where applicable and enumeration state.
The finite inventory is complete only when the whole declared stream was retained
and mapped to the frozen file. A consumer validates distinct token identity, contiguous
ordinal/cardinality and source bounds before using local absence or a count as exact.
The domain is the observed file stream under that unit's language options, not every
C++ runtime behavior or every project configuration. An unopened source has a raw
inventory but no fabricated expanded complete domain. Raw observations from separate
TUs may merge only when profile, content, language interpretation and values agree;
expanded observations and disagreeing variants remain distinct.

Active raw tokens require complete preprocessor skipped-range/directive observations.
Template state requires complete source-range classification of actual template
syntax. An unavailable classification remains an absent/unknown field, not false.
Macro and template ranges use half-open byte intervals and explicit overlap checks.
Malformed/partial preprocessing, unsupported source mapping, conflicting inventories,
resource truncation and incomplete domains retain specific gap reasons and witnesses.
The original query coverage, closure, guarantees, provenance and conflicts survive;
finite token completeness never promotes the entire query to a closed world.

Limits bound detached token counts, spelling bytes and retained row bytes before
retention. Failures follow the compiler observer's typed resource/error path, with no
partially published complete inventory. Parsing source is read-only and has no
irreversible effects. Independent scan/query exchange preserves these new rows using
the installed public registry and transfer decoder without a provider admission gate.

The LLVM-free SDK exposes `query::source_token_input`, `source_token_limits`,
`source_token_state`, `source_token`, `source_token_stream`,
`source_token_projection` and both `project_source_tokens` overloads. The pure
overload borrows already validated independent-scan rows for the call; the query
overload retains all original scans and their plans and result side channels.
Both produce owned, deterministic evidence and streams partitioned by unit,
snapshot, phase, profile, universe, variant and interpretation. Equal observations
merge supporting evidence. Overlapping payloads remain conflicting. Missing scans,
source bounds, ordinals or cardinality prevent complete local enumeration. Activity
and template completeness are independent classifications; missing fields prevent
exact classification counts without hiding an otherwise complete lexical stream.
Declared raw directive counts must match distinct directive-start tokens, whose
preprocessor flag and hash kind must also agree. Classification gaps retain the
stream and witnesses rather than manufacturing false flags.

All configured limits must be positive. Defaults are two million input rows, four
million condition expansions, 256 MiB canonical evidence, 512 MiB expanded rows,
sixteen million evidence references, 100,000 streams, 250,000 tokens per stream,
4,096 original queries and 64 MiB owned query plans. Bounds are checked before
retention; cancellation is checked throughout joins and validation. Invalid inputs,
limits, budgets, cancellation and allocation failures return typed `sdk.token-*`
errors. These are value projections without effects or shared mutable state.

Completion follows registry/identity and positive/negative/bounded tests, the actual
compiler observer, shared SDK finite-token projection, native Monet file metrics and
Monster token mass, then archive/CXMIR reimport and report/scene tests. Clone/template
consumers then use this same stream with their own versioned algorithms and policies.
