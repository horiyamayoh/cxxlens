# ADR 0125: Actual typed call operands and function call scopes

- Status: Accepted
- Contract: `cxxlens.call-operands/1`
- Authority: NG integrated design, relation registry, ADR 0113, 0117, 0121, 0124
- Scope: additive installed relations and owned queries, private Clang collector, monet-code model binding
- Stability: versioned additive pre-release surface

monet-code binds API effects to an original call site, argument, receiver or return
expression. `cc.call_site` and `cc.call_direct_target` identify the occurrence and
syntactically selected callee, but do not witness the actual operand. Names, nearby
source text, aliases and the callee's type cannot supply that witness. The new
`cc.call_operand.v1` carries actual compiler expressions. It does not assert runtime
dispatch, aliasing, heap identity, ownership, a result destination or an API effect.
The shared input also closes an independently enumerated actual function declaration
scope, including written defaults on bodyless declarations, rather than treating
absence of returned calls as zero.

## Unified original operands

One relation uses original call identities; no separate argument/receiver/result
relations or value service is introduced. Its identity is the canonical tuple
`(call, kind, index)`. Argument indices are zero based ordinary parameter slots;
receiver/result use a separate kind and index zero. Source, original syntax node,
type, direct referenced declaration, evaluation state, origin, default/implicit
flags and actual/formal/written indices remain payload. Every join retains compile
unit, universe, variant and interpretation. Incompatible conditioned payloads are
conflicting; no first candidate or source-only match selects a witness.

Profile `clang22-actual-call-operands/1` enumerates actual parameter operand slots,
one receiver when the compiler supplies an object argument, and a result expression
for a non-void or dependent call or an explicitly written construction. The original
call independently retains the optional atomic group `argument_count`,
`operand_count`, `operand_population_state` and `operand_profile`. Counts are
observed before source/type/identity binding exits. Missing facets are unknown;
complete zero requires a completed actual traversal under this exact profile.
The optional original `expression` refers to its actual `cc.syntax_node`.

A complete slot population is independent from complete source, type, expression
or named-reference witnesses in each slot. Known literals and compound expressions
have an absent direct reference; dependent or unavailable references are unknown.
Only `DeclRefExpr` or `MemberExpr` after transparent parentheses and implicit casts
provides `referenced_entity`. A field declaration identifies that field, not a heap
object or receiver instance. A pointer receiver identifies its actual expression,
not its pointee. `&x`, `*p`, `p + 1`, conditional expressions and call results do not
become direct named-object witnesses. The result identifies the actual invocation
expression, with no assignment or initialization destination guess.

Ordinary member calls retain the implicit object argument. Member operators remove
the first actual object operand from ordinary argument numbering and retain it as
receiver. Explicit object parameters remain receiver slots with their actual formal
index. Static/free operators retain actual parameter operands; the extra AST object
expression of a static operator is neither a formal argument nor a bound receiver.
Indirect member calls still retain their actual object expression when their target
is unknown. Explicitly written constructors retain the actual constructor target and
arguments, with no fabricated receiver. Implicit default/copy construction remains
outside this written construction profile.

Optional `formal_parameter_index`, original `actual_argument_index` and
`written_argument_index` retain different compiler meanings. Variadic actuals have
no invented formal index. An injected default has an actual/formal index and no
written index. `origin` is written, default_argument, default_initializer,
implicit_receiver or result. Default wrappers detach the declaration's actual
expression source and syntax; they never borrow the enclosing call span. Such a
syntax node may have a declaring function different from the invocation caller.

Value category is actual lvalue, xvalue, prvalue, dependent or unknown. Evaluation
is potentially_evaluated, unevaluated, dependent or unknown. Bounded compiler
traversal context observes sizeof/noexcept/requires/traits/decltype/typeof, evaluated
VLA bounds and polymorphic typeid, and lambda ownership. A known syntactic evaluation
context does not assert that execution occurred. Unknown context never defaults to
evaluated. The existing finite category flags on the exact original syntax node
retain discarded non-void result versus unknown use.

## Actual target signature

Original `cc.call_direct_target` optionally retains `target_signature_state` and
`target_signature_profile` plus compiler USR/provider-key bytes, canonical type,
structural signature digest, canonical component digest/profile, language, linkage
and module domain. Profile is `clang22-original-target-signature/1`. The component
digest is the actual `cc.type.component_signature_digest`, under actual
`clang22-structural-type/1`; it is not a downstream reconstructed type digest.

The facet witnesses original observed axes at the frozen call source, including
external targets whose declarations are outside the frozen project. It creates no
external declaration span, admission or body closure. Complete observed axes do not
interpret unsupported or missing type structure: actual `cc.type` and ordered
`cc.type_component` evidence remain required for an exact semantic model selector.
Target resolution is independent from syntactic call/operand enumeration. Indirect
and dependent target diagnostics remain on the target relation; they do not turn a
known syntactic call population into missing input.

## Independent actual function declaration scope

One optional atomic facet extends `cc.entity_detail` and the actual definition's
`cc.body`: `call_site_count`, `call_site_ids`, `call_site_state`, `call_site_profile`.
Recognized profile is `clang22-function-emitted-call-sites/1`, state complete or
partial. It counts actual expressions admitted to the original emitter before
source/identity/normalization exits, and lists original `cc.call_site` IDs. The count
is not inferred from returned rows. Per-kind counts use original site kinds without
redundant columns.

Scope belongs to each actual FunctionDecl/source/unit/world, not a merged canonical
entity. It contains original CallExpr of every emitted kind, including indirect and
dependent calls, written declaration defaults, written constructor-initializer calls,
the written body and separately admitted written construction expressions. A
bodyless declaration can have a known nonzero default-call count. A definition body
repeats only its own declaration facet; another redeclaration never replaces a
nonempty scope with zero. Inherited defaults are visited only at their actual written
declaration, and are not copied into redeclaration scopes or syntax populations.

Lambda capture initialization stays in the enclosing owner; the lambda operator's
written body/defaults have their own actual function owner. Unevaluated syntactic
calls remain original sites with an independent evaluation axis. Implicit template
instantiation copies are outside the written declaration profile; observation does
not force instantiation. Global/default-member initialization outside a function
keeps its own original event and does not close this function scope.

Actual function declaration flags `finite_function_body_v1` and exactly one of
`body_written`/`body_absent`, when classified, use
`doesThisDeclarationHaveABody()` plus actual getBody(). Deleted/defaulted/copied
bodies are excluded; skipped or unavailable written bodies preserve unknown
eligibility. These flags supply clone/body candidate membership without substituting
`is_definition`. Missing original mappings, owner/source identities or unsupported
contexts retain frontiers. Complete requires independently admitted count equal to
unique original IDs with actual same-owner rows. New/delete and synthesized lifetime,
coroutine or dispatch operations acquire no call truth from a source position.

Original syntax flags `finite_call_admission_v1` and `admitted_call_site` retain
membership of this exact emitter profile independently from returned call rows.
Every observed syntax node carries the classification marker; a call is admitted
only by the actual project-source CallExpr or written construction predicate.
The SDK matches admitted syntax identities to original `call_site.expression` in
the actual declaration scope. Removing call rows and setting their inventory to
zero cannot erase surviving admitted syntax. Missing syntax classifications or
scan completeness preserve unknown population; incompatible mappings conflict.
The optional physical definition body mirrors its declaration facet, and an
actual contradictory mirror cannot provide complete closure.

## Owned public projection and bounds

`project_call_operands` and `project_function_call_scopes` accept explicit annotated
row spans or `application_query_results` and return an owned `call_operand_projection`.
Raw completeness assertions default false. The scope-only query skips operand/type
scans. The result preserves original relation-labelled evidence, query ownership,
conditions, coverage, closure, unresolved/conflict, contributors and guarantees.
Default/temporary/literal expression effects may bind original operand/syntax/type
without a named object; named-object effects remain unknown without that witness.

Compiler AST pointers exist only during one borrowed TU callback. The observer binds
the actual expression to its exact original canonical observation; the normalizer
returns that observation's original call ID. The collector binds original syntax by
the actual expression, never just matching text, source range or pointer order.
Additive call fields replace the exact original row before publication. Pointers are
not serialized or retained in returned observations. Metadata is precharged and
bounded; rows use existing output limits; at most 4096 operand slots per call and
bounded traversal/condition/evidence/source-plan budgets apply. Cancellation and
resource failures retain existing typed terminals. The existing finite limits also
accept an optional caller cancellation callback, polled alongside the stop token
during projection work; it creates no worker or separate execution domain.

The consumer dependency order is schema/descriptor validation, original native
observations, owned typed SDK projection, then monet-code effect binding. Independent
native and portable fixtures cover direct/default/member/static/member/free/static
operators, explicit objects, constructors, indirect/dependent targets, macros,
bodyless/redecl/default scopes, lambda ownership and unevaluated contexts. Negative
fixtures remove witnesses, contradict counts/indices, duplicate/conflict slots, mix
worlds and test bounds/cancellation. Ordinary build/run checks validate semantic
inputs; no certificate, tested-SHA ledger or additional quality system is introduced.
