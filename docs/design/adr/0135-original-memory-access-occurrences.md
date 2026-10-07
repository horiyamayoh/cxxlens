# ADR 0135: Original memory access occurrences

Status: isolated implementation draft.

Contract ID: `clang22-original-memory-access-occurrences/1`, with per-member
observation profile `clang22-original-memory-access/1`. Authority is the accepted
relation registry, the original Clang AST and this ADR under the integrated
design. The write scope is additive optional fields on `cc.entity_detail.v1`,
`cc.body.v1` and `cc.syntax_node.v1`; existing identities and envelopes remain
unchanged. Support is the existing supported Clang 22 native application path.
Earlier inputs without the facet remain unobserved. Other providers require
their own original observation and cannot inherit this profile.

Monet's Memory3 consumer needs a complete candidate domain and exact original
access eligibility before combining an array extent or a pointwise lattice value
with an access. Existing syntax operands and local may-flow facts cannot show
that all builtin subscripts, dereferences and arrow accesses were admitted, or
distinguish a builtin address-only access and exact builtin `&*` cancellation.
Compiler observations supply inputs for proved, disproved, unknown, partial or
conflicting consumer outcomes; the producer itself supplies no bounds/null
verdict, safety guarantee, alias conclusion or Finding.

An atomic count/member/state/profile facet independently counts original builtin
`ArraySubscriptExpr`, unary `UO_Deref` and arrow `MemberExpr` candidates in each
written physical function body and constructor initializer. Unevaluated members
remain present with their actual evaluation classification. Overloaded operator
calls are outside this builtin domain. Count admission precedes source or ID
binding; complete membership requires equality between the original count and
the distinct retained syntax-ID set. Partial membership may retain a larger
count than the set. Missing source, unresolved/dependent selection, skipped or
unsupported bodies, ambiguous original syntax and unbound default/field
activations keep explicit reasons and actionable frontiers. A genuinely absent
body has a known empty body domain only after actual body-absence admission.
Function declaration and body copies agree for one original physical scope.

Members retain the original physical declaration, builtin kind, evaluation and
operand associations. A builtin address operator applies only to its original
direct operand through parentheses; it never propagates to nested subscripts.
Exact builtin `&*` cancellation retains the enclosing address expression.
An original array region requires the actual array expression, through original
parentheses and ArrayToPointerDecay only. Pointer spelling, a pointee type,
singleton may-alias and source/CFG co-location cannot create a region witness.
Original ConstantArrayType extent bytes remain on the existing structural type
component and retain their existing `cc.array-extent.v1` digest.

No event, before-point or current value version is invented by this facet. The
consumer requires the original event, its independently retained reaching_before
point and its definition IDs. It preserves source queries, claim/provenance,
coverage, closure, conflicts and world/condition identity. Missing candidate or
pointwise/value axes remain actionable unknowns. Native whole range/null domain
partial states do not become closure claims.

The completion order is schema and generated tags, original syntax/census
binding, positive and negative native controls, bounded/fault controls, then the
independent Memory3 consumer and ordinary main workflow. Existing population,
traversal and retained-byte bounds apply before allocation and throughout joins.
Cancellation/resource failure cannot publish a fabricated complete empty domain.
Tests cover direct arrays and pointers, exact address controls, unevaluated and
overloaded exclusions, distinct physical scopes/worlds, missing/ambiguous bindings,
default activation frontiers and healthy known-zero sibling functions. Ordinary
Git, build logs and tests remain the development record.

The same original TargetInfo data-model profile additionally retains optional
`cc.abi_surface.v1.char_width_bits` from `TargetInfo::getCharWidth()`. This is the
original target character storage unit, not the process host byte size. Monet's
endian consumer can compare original integer value width against this unit;
value width still excludes padding and is not an integer object-size claim.
Earlier inputs without character width leave that axis unobserved. The field
preserves ABI context, unit, world and the existing target data-model state.
