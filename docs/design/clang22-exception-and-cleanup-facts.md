# Original exception specifications and cleanup bindings

The consumers are the function exception metrics and the NOEXCEPT_ESCAPE,
CLEANUP_FAILURE, ROLLBACK_FAILURE, and resource-release rules. The existing
exceptional-exit population identifies original lowered instructions and
physical ABI variants. It does not identify their automatic object or resolve a
lazy function exception specification.

`clang22-function-exception-specification/1` observes the stored
`FunctionProtoType::getExceptionSpecType()` of an already parsed function. The
optional `cc.entity_detail` fields retain its kind, state, known nonthrowing
truth, and profile. Complete classifications retain true or false. Dependent,
unevaluated, uninstantiated, and unparsed specifications remain partial with no
truth value. A missing prototype is unavailable. The observer does not invoke
`canThrow`, `isNothrow`, Sema, or an evaluator. This facet is independent from
function-body and exceptional-exit admission.

`clang22-destroy-object-cleanup-emission/1` observes the actual automatic
`VarDecl` in `emitAutoVarTypeCleanup` and the actual `DestroyObject::Emit`
normal/EH flags. The compiler cleanup object carries a copied registration
token. Registration and emission ordinals start at one and belong to the
original physical function and ABI lowering variant. `cleanup_declaration`
retains the exact original declaration FK. An unregistered temporary cleanup
shadows an enclosing automatic cleanup; it never inherits that object's
identity. NRVO and other cleanup classes do not acquire invented bindings.

`clang22-destructor-emission-target/1` separately observes the actual
destructor declaration and `CXXDtorType` passed to the destructor emitter.
Both the ABI-delegating declaration overload and the `GlobalDecl` overload
retain the compiler's original operands. It binds only the generic emitted
`CallBase`. The
optional target entity, raw USR bytes, and original `CXXDtorType` remain
independent from the generic `EmitCall` abstract target. Runtime and sanitizer
helpers do not inherit this target. More than one generic invocation within an
emitter view leaves its target correspondence unknown.

All facts retain the existing source query rows, evidence, claim condition,
unit, world, physical scope, and lowering variant. Missing declaration or target
bindings remain unknown; contradictory original rows retain conflicts. Existing
exceptional count membership and eligibility do not depend on these new facets.
The callbacks charge bounded work/storage and latch cancellation or failures.
Compiler pointers expire with the existing analysis job.

Original compiler functions without a `GlobalDecl`, including coroutine
await-suspend wrappers, retain matching start/end frames without a written
owner. They do not inherit an enclosing function's identity. Unsupported
coroutine activation keeps that written scope partial while other independent
source and declaration populations remain available.

The LLVM-free `project_exception_cleanup_facets` SDK projection retains the
original callable detail and exit rows. Stored specification truth, callable
identity, source, cleanup emission, registration, declaration binding and
destructor attribution each have an independent state. A registered automatic
object and an unregistered temporary can both have a known emission. A missing
normalized destructor entity does not erase the actual emitter's raw USR and
ABI destructor enum. The raw and original-query overloads validate every
supplied row and report bounded measured work and conservative retained
storage. Older saved rows with absent optional columns keep those facets
unknown.

`sdk.project-function-exception-specifications/1`, catalog entry
`public.function-exception-specifications`, is the supported NG1 purpose-specific
projection for consumers that need stored specifications without cleanup
correspondences. Monet's original noexcept declaration and static path metadata
admission are its consumers. The full cleanup projection remains unchanged.
The narrow projection admits the same nine supplied relation groups, retains
specification states, original evidence and evidence indexes, all source scan
coverage flags, missing-scan reasons, and the original query side channels. It
also checks supplied detached declaration membership framing and bounds. Its
result has no cleanup population; it makes no cleanup absence or closure claim.
Known stored true and false remain complete; lazy or missing truth remains
partial or unknown, and contradictory observations remain conflicting. It never
resolves or evaluates a specification. Missing inputs are completed by acquiring
the independently required original scans, rather than by inferring truth.
Work, text, byte, member, retained-storage and cancellation limits apply before
retention, and usage remains zero on failure. This additive API does not change
existing full projection types, symbols or results.

These facts establish static compiler correspondences. They do not establish a
runtime object version, alias set, operation CFG point, or complete resource
cleanup population. Consumer proofs must require their remaining independent
inputs. The next binding step is the compiler's actual invocation unwind route
to registered cleanup emissions, followed by the original acquire/release
operation and object-definition correspondence. LLVM ordinals and source
overlap cannot substitute for those inputs.

The existing lowering callback route discriminators are 1 for the original
`EmitCXXThrowExpr` insertion block, 2 for `getTerminateLandingPad`, 3 for
`getTerminateHandler`, 4 for `getTerminateFunclet`, and 5 for `getEHResumeBlock`.
The emitted `compiler_route` is an original block-role witness. It does not
establish entry reachability, an invoke's unwind successor, or a path from a
potentially throwing invocation to termination.
