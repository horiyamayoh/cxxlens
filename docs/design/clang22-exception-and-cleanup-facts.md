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

These facts establish static compiler correspondences. They do not establish a
runtime object version, alias set, operation CFG point, or complete resource
cleanup population. Consumer proofs must require their remaining independent
inputs. The next binding step is the compiler's actual invocation unwind route
to registered cleanup emissions, followed by the original acquire/release
operation and object-definition correspondence. LLVM ordinals and source
overlap cannot substitute for those inputs.
