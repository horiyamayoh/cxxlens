# ADR 0141: Original observed lifetime requirement members

- Status: accepted
- Contract: `clang22-original-observed-lifetime-requirements/1`
- Authority: relation registry and original Clang 22 AST/CFG
- Owner/write scope: exact Clang 22 adapter; optional `cc.body.v1` facets
- Stability: versioned additive; existing nonempty lifetime frontiers remain open

## Consumer and population

Monet Lifetime consumes an independent inventory of potential lifetime requirement
members for the observed first-party static population. Existing address-transfer
and lifetime-end subsets cannot establish that this population is empty. The
inventory belongs to the exact written physical FunctionDecl body, using its
original declaration, function, compile unit, source and world. It covers original
observed body syntax, parameters/local value declarations, written initializers and
actually observed implicit/default CFG members. It makes no assertion about
unobserved generated runtime activity, bodyless declarations or global scopes.

The normative conservative requirement superset selects:

- Every original call, construction, allocation, deletion, throw, coroutine
  suspension, atomic expression, default argument/member activation, cleanup
  expression and inline assembly occurrence. Calls are selected independently
  of names and of whether a captured model later assigns lifetime effects.
- Every observed expression or value declaration whose original canonical type
  is a pointer (including member/block pointers), reference, record, array or
  nullptr type. A DeclRefExpr also tests its actual referred ValueDecl type, so
  an lvalue expression does not hide a reference declaration.
- Builtin address-of, original xvalues, explicit cast types as written and
  returns from functions with a selected return type. Expr types alone do not
  retain reference cast targets, so original value category and spelled QualType
  are independent compiler observations.
- Actually observed lambda call-operator captures by reference, of this, or with
  a selected captured-variable type. The capture itself is counted as an unbound
  requirement member; an outer declaration ID is not its body activation ID.
  Lambda expressions also retain their original record-type requirement member.
- Actual implicit CFG destructor, cleanup, allocator and initializer elements;
  actual scope/lifetime elements when their original subject has a selected type.

A null/dependent or unsupported object type is selected with partial admission.
Primitive arithmetic/enumeration types, function types and void are explicit
exclusions unless one of the preceding occurrence predicates selects them. Thus
scalar-only CFGLifetimeEnds and scalar return/address-transfer observations remain
in their independent existing populations without becoming requirement members.
This contract supplies a conservative potential-requirement inventory; iterator,
move invalidation, view and effect eligibility remain authored model policy.

## Facets and outcomes

`lifetime_requirements_profile`, `lifetime_requirements_state`,
`lifetime_requirement_count`, `lifetime_requirement_syntax_ids`,
`lifetime_requirement_declaration_ids`, `lifetime_requirement_cfg_element_ids`
and optional `lifetime_requirements_reason` are additive body columns. The three
sets refer to existing original rows with their compiler kinds and exact existing
source/operand/subject/CFG bindings; no mixed-ID union or synthetic candidate row
is introduced. A distinct original AST object is counted once in each physical
body and each original CFG slot is counted once. AST and CFG views are explicitly
different tagged members and never substituted for each other.

Count candidates before source, ID and binding admission. Relevant original lambda captures
remain counted without substituted IDs, keeping their physical body partial. Complete membership
requires count equal to the sum of the three distinct retained sets, exact
written physical ownership, closed original AST and CFG enumeration, available
original sources/bindings, nondependent selection and no parse error. Default
activations, unknown types, unsupported inline assembly, missing/ambiguous IDs or
sources, and unsupported physical scopes keep partial/unavailable membership;
partial count may exceed retained IDs. Bound defaults retain only their original
wrapper ID, never borrow activated declaration syntax as caller occurrences.
Bodyless/global scopes have no facet and stay unknown.

Complete zero proves only an empty observed potential-requirement population.
The consumer may then bind empty relevant event/version/alias/policy member
domains, while separately checking existing original written CFG element, point,
edge and first-party source populations. It never proves absence of objects,
lifetime ends, unobserved generated activity or universal runtime alias closure.
Nonempty members retain unavailable subject/referent/version/policy/placement axes.
No compiler type spelling, saved numerical replay or selected-move policy guess
is admitted. Conflicting original row bindings remain conflicting/partial through
the existing claim and original identity contracts.

## Bounds and executable controls

Use existing 32M context work, original population storage, fact/output and
cancellation limits before growth; no new product quotas. Test genuine scalar
prvalue arithmetic zero despite nonempty CFG/lifetime/address observations;
retain C++23 implicit scalar-return xvalues as nonempty requirement controls; explicit call,
pointer, reference, record and array members; default activation and dependent
selection frontiers; unsupported assembly and subset/unbound controls; repeated
query decoding and typed output-bound failure. Completion proceeds through native
inventory, a matching public descriptor/decoder and the independent Lifetime
consumer. Actual referent/version/alias/model transitions for nonempty members
remain separate future compiler/model inputs.
