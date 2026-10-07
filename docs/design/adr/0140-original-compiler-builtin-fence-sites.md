# ADR 0140: Original compiler builtin fence sites

- Status: accepted
- Contract: `clang22-original-compiler-builtin-call/1` and `clang22-original-fence-sema/1`
- Authority: relation registry and original Clang 22 AST/Sema
- Owner/write scope: exact Clang 22 adapter; optional `cc.syntax_node.v1` facets
- Stability: versioned additive; stock Sema callback availability stays unavailable

## Consumer and decision

Monet Concurrency consumes original builtin fence occurrences independently from
AtomicExpr operations. Existing source-feature traversal supplies the complete
CallExpr population, but lacks compiler builtin classification and physical fence
scope. It cannot close that subset by source spelling, model names, or raw order
integers. Retain the original `CallExpr::getBuiltinCallee()` ID and classify only
compiler enum IDs for GNU, C11 and scoped thread/signal fence builtins. ID zero
means the original call is not a compiler builtin; other builtin IDs remain other
builtins. Library calls and runtime target closure remain separate unknown axes.

Retain genuine implicit fence FunctionDecls with their original source and
physical declaration identity; never substitute a canonical prototype. Bind the
actual callee declaration, order operand syntax, and existing physical body scope
profile. A scoped site can be proved a fence even when order validity is unknown.
The whole source-feature census must independently close occurrence membership;
missing syntax or classification keeps the affected population partial.

At the successful original `CheckBuiltinFunctionCall` return, retain exact call,
callee, builtin ID, original evaluation context, discarded/constant/default flags.
Clang 22 has no fence-specific order predicate or ICE requirement here. Therefore
`fence_order_validation=not_checked`, with no typed order class. Generic accepted
builtin processing is not order validation. No extraction-time constant evaluation
or numeric-to-order conversion is permitted. LLVM lowering, if later captured,
requires a separate phase contract and never establishes language validity or
runtime execution.

## Outcomes and bounds

Compiler identity complete proves or excludes this builtin subset. Exact scope
and Sema axes are independent. Missing physical callee/operand or body is partial;
stock callback absence is unavailable; duplicate contradictory callbacks are
conflicting. Unknown order predicates, default/implicit activation, aliases,
storage, runtime suspension and all atomic-library populations remain unknown.
Original unit/world/source/provenance and source-feature census are preserved.

Use existing observer work/field/storage/cancellation bounds and syntax population
bounds before growth. Positive GNU/C11/scoped sites, nonbuiltin/other builtin
exclusions, dynamic orders, global/default/unevaluated/discarded contexts, exact
lambda ownership, stock route absence and bounded failures are executable controls.
Completion proceeds from authentic identity/scope, to consumer subset binding;
order and lowering semantics require distinct original compiler observations.
