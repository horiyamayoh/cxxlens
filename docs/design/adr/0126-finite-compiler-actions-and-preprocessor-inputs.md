# ADR 0126: Finite compiler actions and preprocessing inputs

- Status: Accepted
- Contract: `cxxlens.finite-compiler-inputs/1`
- Authority: NG integrated design, relation registry, ADR 0124 and 0125
- Scope: installed semantic inputs, Clang 22 extraction, owned SDK queries, monet-code
- Stability: additive versioned pre-release surface

monet-code's modeled occurrence counts and local protocol rules need actual new,
delete, construction, destruction and state/type observations beyond CallExpr.
Its preprocessing metrics need raw inactive directives and actual evaluated macro
and conditional observations with independent finite populations. Returned rows,
display strings and source-position similarity cannot establish those domains.

`cc.operation` is one original compiler occurrence relation. AST, declaration and
CFG occurrences retain distinct original identities and an independently bound
static site identity under `clang22-function-compiler-actions/1`. Exact compiler
expression/declaration/lifetime correspondence joins views of one site; source
position equality never supplies that join. A new expression and its CFG allocator
view share one allocation site. Conditional initialization-failure cleanup remains
a distinct static site with a conditional outcome. Several CFG exit views of one
actual object destruction share its declared object/lifetime site; counts do not
assert path executions. Missing site grouping remains unknown.

Original invocation rows reuse `cc.call_site`, target signatures and unified
`cc.call_operand` observations. Non-call operations retain actual compiler target,
type and one explicit object/declaration/expression/source witness when present.
A pointer operand does not identify its pointee. A nominal type role does not
identify an implicit lifetime transition. No separate value or ownership service
is added. Evaluation, outcome, finite membership, source, target, object and type
classification remain independent axes.

The same relation retains actual declarative type-use subjects: distinct original
parameters, return slots, local declarations, captures and materialized temporaries
in their exact function/declaration scope. Repeated casts or parentheses do not
invent new type subjects. A constructor invocation and a type-use subject are
different declared categories; an exact captured catalog may classify both.
Type-use population is required when model catalogs permit type subjects. A call
census alone cannot close such a catalog's full occurrence domain.

An optional atomic operation count/member/state/profile facet on actual entity
details and definition bodies counts admissions before source/identity helpers.
Actual return/parameter/capture indices and the original function capture count
retain type-subject membership independently. Actual target kind can survive a
missing normalized implicit constructor/destructor entity. Original syntax/declaration classifications and actual implicit CFG node action
counts independently retain admission. Existing direct CallExpr and emitted-call
profiles keep their semantics. Dependent/unobserved bodies, unknown compiler
operations and missing scope/source/CFG/target/object joins retain actionable
frontiers. Observation does not force instantiation or certify dynamic dispatch.
CFG element positions are not substituted for another declared flow-point policy.

Preprocessing extends original `source.preprocessor_event` with typed raw/evaluated,
macro, replacement, parameter, argument, conditional region, symbol, expansion and
effect facets. One `source.preprocessor_inventory` enumerates each actual
unit/file/snapshot/world under separate `clang22-frozen-raw-directives/1` and
`clang22-evaluated-preprocessor/1` profiles. Raw compiler Lexer observations include
inactive and unopened frozen files. Evaluated completion requires actual entered
file traversal; unopened is unknown, not parsed empty. Raw directive-start tokens
and actual PP callback admissions independently bound membership.

Conditional parent/branch/closing IDs, physical intervals and depths are typed.
Frozen physical line starts preserve LF, CRLF and CR. Activity comes from actual
compiler observations and remains independent of raw structure. Macro parameters,
variadics and replacement #/## use actual compiler/raw-token grammar, never display
prose. Performed expansions and unexpanded candidate uses are different domains.
Parent/depth must retain real argument pre-expansion correspondence; missing compiler
context stays unknown. Argument side effects require exact original slot/substitution
and AST evaluation witnesses, not text parsing or name guesses.

Selected-unit parse health extends the original build domain through
`build.compile_unit_analysis` and `build.analysis_inventory`. Prepared project,
unit, source and variant identities are reused even when parsing fails. Parser
success/recovery/failure is observed independently from semantic output. Fatal
errors remain failed even with a partial AST; completed nonfatal recovery is distinct.
Unobserved parsing is unavailable. Actual selected unit and variant counts/member
IDs are admitted before parser callbacks; preparation gaps preserve selection
unknown. Diagnostics are ordinary product input, not development certification.

Owned SDK projections preserve original annotated rows, world/condition identity,
finite counts, known-empty versus missing, conflict, coverage, query ownership and
frontiers. Existing bounds and caller cancellation apply before retained growth.
Unknown future kinds/profiles remain unavailable to unsupported consumers. Native
and SDK examples cover actual operations and PP structures plus missing/conflicting
joins and resource/cancellation failures. Git and ordinary build/test logs remain
the development workflow; no tested-SHA ledger, certificate or additional quality
system is introduced.

The existing `cc.flow_fact` also carries optional original `expression` and
`value_expression` syntax IDs under `clang22-flow-original-expressions/1`. A
written variable definition uses its actual initializer; an assignment uses the
original assignment expression and RHS, without joining source intervals or
inferring storage for a call result. `clang22-flow-cfg-elements/1` separately
retains actual CFG element index/kind, with a terminator at index equal to the
original block size. Entry definitions and boundary set facts have no invented
CFG element. Expression and CFG binding states are independent of the finite
flow population, reaching fixed-point and alias frontiers.

Default argument and in-class initializer activation retains actual
`expression_context` and the original `context_declaration` (parameter or field)
on the existing operation. The original written expression/call owner differs
from the execution function/body/CFG owner. A bounded compiler-pointer walk
through `CXXDefaultArgExpr` and `CXXDefaultInitExpr` supplies the witness; source
containment never supplies it. An absent or ambiguous original owner stays unknown.
The CFG enables Clang's actual in-class initializer elements. A default argument
activation whose original action has no retained execution occurrence leaves the
execution scope partial and retains its written declaration trace. This profile
does not invent an activation identity or CFG element index; explicit activation
views require a later compiler-bound identity extension.

Macro `substitution_count` is the direct original replacement-token parameter
membership. `evaluating_substitution_count` is the independently observed
expanded AST multiplicity of the original argument in potentially evaluated
contexts. Nested forwarding can have direct count one and actual evaluation
count two; a `sizeof` use has evaluation count zero. SourceManager
`MacroArgExpansion` spelling correspondence supplies the original argument
join. Stringified/unused arguments retain explicit zero evaluation multiplicity
and unavailable AST side-effect truth. No ordering comparison between the two
count axes is assumed.

Parameter type-use subjects are admitted before optional named-entity or
original declaration binding. Their actual parameter index, type and source
remain available for unnamed slots. `parameter_has_default_argument` retains
only the actual `ParmVarDecl::hasDefaultArg()` result on that original slot;
absence is unknown. Named object binding is an independent axis and cannot
erase the finite parameter population or provide a guessed default.
