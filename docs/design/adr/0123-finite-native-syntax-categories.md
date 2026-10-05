# ADR 0123: finite native syntax categories

Status: accepted

Monet needs counts of written syntax categories, including catch forms, pointer/integer
conversions, suspension expressions, and unused nonvoid call results. The native producer
adds `finite_categories_v1` to every admitted `cc.syntax_node.v1` row. Within this profile,
an absent category flag is known false; a producer without this marker cannot establish
the absence of a category. Existing source, callable, unit, condition, and finite body
enumeration identities remain authoritative. Unmapped syntax still prevents a complete
body population.

The profile exports these independent flags:

- `catch_all`: an actual catch declaration has no exception variable.
- `catch_empty`: an actual compound handler contains only null statements, or no statements.
- `catch_nontrivial_by_value`: its actual canonical exception type is nonreference and
  nontrivial. Dependent or incomplete types use `catch_type_unknown`.
- `cast_pointer_to_integer` and `cast_integer_to_pointer`: the actual Clang cast kind.
- `coroutine_suspend_site`: an actual `CoawaitExpr` or `CoyieldExpr`, including initial and
  final suspension when they are admitted with an actual source identity. Missing source
  coverage is retained; the profile does not fabricate implicit suspension spans.
- `discarded_nonvoid_result`: an evaluated, nondependent nonvoid call whose result is
  discarded by an expression statement, explicit void conversion, builtin comma left
  operand, or `for` initializer/increment. Transparent expression wrappers, builtin comma
  right operands, and conditional branches inherit the surrounding result use. Arithmetic,
  argument, condition, return, and declaration initializer uses consume the value. The
  rule observes actual value use and does not depend on warning attributes such as
  `nodiscard` or `pure`. Unevaluated `sizeof`, `noexcept`, requirements, `decltype`, and
  unevaluated `typeid` contexts do not count. Dependent or missing usage uses
  `discarded_result_unknown`.

Category inspection uses the bounded existing traversal stack, without rebuilding a
translation-unit parent map or instantiating dependent declarations. A separate 32 million
operation bound stops category ancestry inspection. Flags remain detached, owned public
facts; no compiler pointer crosses the native callback scope.

The real compiler fixture checks three discarded calls alongside consumed calls, comma
operands, and unevaluated calls; independent catch and conversion fixtures verify positive
and negative classifications. These are ordinary behavior tests, not a producer certificate
or execution admission requirement.
