# ADR 0137: Original atomic Sema orders and context

Status: accepted.

Contract ID: `clang22-original-atomic-sema/1`. Authority is the accepted relation
registry, the original Clang 22 `Sema::BuildAtomicExpr` phase and this ADR under
the integrated design. The write scope is additive optional fields on
`cc.syntax_node.v1`; existing syntax identity, original operands, raw atomic
profile and claim envelopes remain unchanged. Support requires the actual
instrumented SemaChecking translation unit. An unavailable route or missing
original callback is an explicit unknown, including with a complete independent
source-feature traversal. Earlier snapshots remain unobserved.

Monet's Concur14 consumer needs typed original order and eligibility inputs for
the builtin AtomicExpr subset. The existing raw atomic expression profile can
retain successful EvaluateAsInt values but does not establish compiler order
legality. The original source-feature inventory supplies independent occurrence
membership and denominator admission; this facet does not add a second census.
Fences and library calls are separate populations and prevent any claim that
AtomicExpr membership closes all atomic operations. Object/storage, mixed
access, alias, root reachability, wait and coroutine outcome axes remain open.

The callback runs only on the original successful AtomicExpr return path, after
the original argument/type checks and original order observations. It retains
the exact original operation form, success/failure operand associations, the
original ICE results and the outcomes of the actual operation-specific success
order predicate and actual failure-order predicate. Later extraction does not
re-evaluate an operand, translate a saved raw integer, repeat a legality check or
classify an API name. Typed order classes use the compiler's AtomicOrderingCABI
enumeration after the original predicate has established validity. Invalid
orders remain invalid and retain the existing raw fields. A nonconstant order
is unknown; initialization and absent failure operands are not_applicable.

Clang 22 does not validate compare-exchange failure strength against success
strength in this original phase. The independent pair validity is therefore
unknown for compare-exchange, with an actionable reason, and not_applicable for
other forms. Neither two individually valid orders nor a parsed compiler enum
can promote that missing axis. Scoped operation validity and any local Sema
error outcomes likewise remain independent of order-class observations.

Evaluation records the original Sema ExpressionEvaluationContext and the
original discarded, always-constant and default-argument/initializer context
observations. It is independent of source validity, a generic statement class
and the source-feature origin/evaluation fields. PotentiallyEvaluatedIfUsed,
default/field activation and missing physical scope do not imply an executed
caller occurrence. Constant/immediate contexts do not imply runtime execution.
Source-feature evaluation remains unknown wherever no original binding exists.
An exact feature.syntax -> syntax.syntax_scope_body ->
body.function_exit_declaration chain under ADR 0139 can identify a physical
function; a nearest VarDecl context cannot replace it.

The optional fields are `atomic_sema_profile`, `atomic_sema_state`,
`atomic_operation_kind`, `atomic_success_order_class`,
`atomic_success_order_validation`, `atomic_failure_order_class`,
`atomic_failure_order_validation`, `atomic_pair_validation`,
`atomic_evaluation_context`, `atomic_sema_discarded`,
`atomic_sema_constant_evaluated`, `atomic_sema_default_context` and
`atomic_sema_reason`. Profile/state are atomic, independently of the optional
typed observations. Open state/class/context symbols preserve future values.
All fields participate in conflicts; no new semantic identity or numeric order
formula is introduced. Exact original syntax/operand/source/unit/world bindings
and source-feature census provenance remain mandatory consumer inputs.

Consumers may report proved, disproved, unknown, partial or conflicting results
from these original primitives. The producer supplies no concurrency verdict,
lock model, atomic storage equality or runtime guarantee. Coverage, closure,
unresolved reasons, conflict and provenance survive projection and archives.
The bounded parser-job recorder freezes before extraction, owns no compiler
pointer outside that job and charges existing work/member/retained-byte limits
before growth. Cancellation, unsupported hooks, dropped members and ambiguous
callbacks cannot fabricate a complete empty domain.

Completion order is schema/generated tags, original callback and bounded
recorder, exact syntax extraction, native positive/negative/unknown controls,
stock-hook and resource/fault controls, the independent Concur14 consumer, then
ordinary main CI. Controls include all six compiler order classes, operation
restrictions, invalid and nonconstant orders, initialization, compare-exchange
pair frontiers, unevaluated/default/global contexts, missing physical bindings,
deterministic repetition and bounded output failure. Ordinary Git, build logs
and tests remain the development record.
