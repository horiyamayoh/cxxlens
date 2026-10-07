# ADR 0139: Original syntax physical body scope

Status: accepted.

Contract ID: `clang22-original-syntax-body-scope/1`. Authority is the relation
registry and the original parser-job RecursiveASTVisitor body admission. The
write scope is five additive optional `cc.syntax_node.v1` fields:
`syntax_scope_profile`, `syntax_scope_state`, `syntax_scope_declaration`,
`syntax_scope_body` and `syntax_scope_reason`. Syntax identity and claim
envelopes remain unchanged; every observation participates in conflict.

Monet Concur14 and Source2 need a physical definition owner for an original
AtomicExpr or CoroutineSuspendExpr source-feature occurrence. The current
carrier is emitted for those original statement classes and, under
[ADR 0140](0140-original-compiler-builtin-fence-sites.md), actual compiler-enum
classified fence CallExpr occurrences. Other syntax remains unobserved. A canonical function entity, nearest local variable,
source range or object-specific binding does not identify that definition. This
carrier borrows the actual FunctionDecl while traversal is inside that
declaration's admitted body, then binds its original physical declaration and
finalized body. Complete bindings agree with body.function_exit_declaration,
function entity, compile unit and original claim world. It adds no census;
independent source-feature traversal supplies occurrence membership.

The profile/state pair is atomic. Complete requires both original physical
references. Syntax outside an admitted body, including global initialization,
written default arguments, field initializers and omitted implicit/default
activation, is partial with an actionable reason and no borrowed canonical
owner. Missing declaration/body admission is likewise partial. No source-valid,
runtime-evaluated, object/storage, CFG placement, coroutine suspension or
activation guarantee follows from body membership. Unevaluated and discarded
syntax can have a complete physical body binding independently of evaluation.

Consumers preserve proved, disproved, unknown, partial and conflicting results,
coverage, closure, unresolved reasons and original provenance. Earlier inputs
without this profile remain unobserved. Completion proceeds through schema,
generated descriptors, bounded original admission, genuine written/local,
unevaluated/discarded, default/global and nested-owner controls, then main CI.
Parser-job pointers are never persisted or transferred. Existing population and
output bounds cover retained bindings; cancellation and dropped body admission
cannot produce a complete scope.
