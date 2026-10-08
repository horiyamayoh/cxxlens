# ADR 0142: Original direct return referents and lifetime-end observations

- Status: accepted
- Contract: `clang22-original-direct-return-referent/1`
- Authority: relation registry and original Clang 22 AST/CFG
- Owner/write scope: exact Clang 22 adapter; optional `cc.address_transfer.v1` facets
- Stability: additive; no runtime object generation or lifetime candidate verdict

## Consumer and population

Monet Lifetime needs the original returned referent and compiler lifetime-end
observations for intrinsic direct automatic-object return candidates. Existing
return/address carriers preserve endpoints, while their unclassified pointer
return origin does not independently bind a returned referent or its actual CFG
lifetime ends. Runtime object versions cannot be manufactured from a declaration
ID or a reaching definition.

Reuse every original `kind=return` address-transfer carrier, its actual
`carrier_syntax` ReturnStmt, source expression, return-slot destination and
physical owner. The existing body `address_input_*` inventory retains detached original endpoints
and admission frontiers. The independent body `return_statement_profile/state/
count/ids/reason` inventory uses profile `clang22-original-written-return-statements/1`
and counts every observed original ReturnStmt in the same written physical body
before source/ID admission. Its IDs are original syntax IDs; complete membership
requires exact source/physical ownership, closed written AST enumeration and count
equal to retained IDs. A partial count may exceed retained IDs. The independent
observed lifetime requirement superset preserves unmatched calls/defaults/other
requirements.
Bodyless/global/unbound scopes remain unavailable and retain original frontiers.

## Original primitive facets

Optional `return_referent_profile`, `return_referent_state`,
`return_referent_kind`, `return_referent_declaration`,
`return_referent_expression`, `return_referent_storage_duration`,
`return_referent_reason`, `return_lifetime_end_count`,
`return_lifetime_end_ids` and `return_lifetime_end_state` extend the existing
address-transfer row. Every observed retained return receives the profile and
classification/state, including unsupported or indirect returns.

Classification uses only original AST kinds and canonical QualTypes. Documented
transparent wrappers are original ParenExpr and implicit CK_NoOp; actual builtin
address-of and CK_ArrayToPointerDecay bind their actual operands. An exact
DeclRefExpr to a non-reference VarDecl can bind `direct_variable_address`,
`direct_variable_reference` or `direct_array_decay`. The referent expression FK
identifies that original leaf, not a reconstructed source range. A reference-
typed VarDecl remains referent-unknown. An original DeclRefExpr marked by Clang as
referring to an enclosing local or capture also remains referent-unknown: its
declaration does not establish closure-member storage or captured alias identity.
Other pointer/reference results remain
`indirect`/`unsupported`; a `nonborrow` classification records only a result type
that is neither a builtin pointer nor a reference and never excludes a modeled
view or lifetime effect. No pretty type, callee name, numerical replay or alias
closure supplies classification.

Storage duration comes from the actual VarDecl enum. Lifetime-end observations
are the original CFGLifetimeEnds slots whose exact compiler VarDecl subject and
physical FunctionDecl body match the referent. Their typed IDs point to existing
`cc.cfg_element.v1` rows and preserve body/function/unit/world/declaration/source
bindings. Count each observed slot before source/ID/binding admission. Complete
end membership requires closed original CFG-element enumeration, exact original
subject/physical scope and count equal to retained IDs. Automatic referents with
no bound observed lifetime end remain partial; absent observations do not prove
absence of an end. Static/thread storage may have a complete empty observed end
set without asserting anything about global runtime lifetime.

`complete` refers only to these original bindings/observations. Dependent types,
unknown scopes, invalid declarations, unsupported casts, reference referents and
missing sources/IDs preserve partial/unavailable. Conflicting original row/FK
bindings remain conflicting. No field reports escape, dangling, path order,
receiver/resource generation or whether a captured model invalidates a use.

## Independent consumer admission and limits

The consumer independently admits every relevant potential requirement and the
original source, CFG and program-point populations. These primitives may support
an intrinsic returned automatic-object relation; general aliases, rebinding,
path-dependent referents, move/iterator policy and unobserved generated runtime
activity remain separate open axes. Saved metadata validates the exact original
FKs/shape without rerunning a lifetime solver or deriving a metric verdict.

Use existing context/population/resource/output/cancellation limits before
allocation. Genuine tests include direct local and by-value parameter addresses,
direct references and array decay; static/thread controls; reference-typed and
indirect returns; unsupported explicit casts/default/dependent scopes; exact end
subject/source/physical-body negatives, missing/subset IDs, determinism and typed
output failure. Completion requires native bindings, matching installed decoding
and independent consumer positive/fault controls.
