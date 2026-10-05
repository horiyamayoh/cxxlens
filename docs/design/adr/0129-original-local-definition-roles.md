# ADR 0129: Original local definition roles

Status: accepted

Monet's dependence and return-summary consumers must distinguish actual parameter entry definitions from initialized declarations, uninitialized declarations and later writes. A missing RHS or a legacy point number does not establish that distinction.

The existing `cc.flow_fact` carries one optional atomic `definition_role` and `definition_profile` facet, supported by `clang22-original-definition-roles/1`. It applies to actual definition rows. Original function, body, unit, world, named destination, source and expression bindings remain unchanged. Missing or unsupported role information stays unknown independently from finite fact enumeration and from reaching/liveness guarantees.

`parameter_entry` is admitted only by the actual `FunctionDecl::parameters()` entry pass. Its node is the original CFG entry node referenced by `cc.body.entry`; it has no local RHS or actual CFG element. The existing element binding remains unavailable, and no element index is invented. A later write to the same parameter has its actual assignment role. Neither role is inferred from `program_point=0`.

`initialized_declaration` and `uninitialized_declaration` come from the original local `VarDecl::hasInit()`. `assignment` and `compound_assignment` come from the actual assignment opcode, and `increment_decrement` from the actual unary predicate. Missing initializer expression binding does not turn an initialized declaration into an uninitialized declaration. Compound updates require their original operands and prior definition to calculate a value.

Consumers retain original definition/version, destination and source evidence, validate exact function/world/entry ownership and preserve absent object/type/value witnesses. The facet does not add value, alias, lifetime or purity inference. Saved rows without the optional facet remain readable and retain their earlier profile-defined initialized/uninitialized tags.

This is ordinary compiler input for semantic analysis. It adds no certificates, test-SHA ledgers or separate quality system.
