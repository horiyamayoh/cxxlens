---
cxxlens_document_status: archived
cxxlens_authority: non-normative
cxxlens_replacement: docs/design/cxxlens_next_generation_integrated_design_ja.md
cxxlens_removal_issue: "#72"
---
# Failure contract

Use `error` when an operation cannot be established because an API argument, configuration, I/O
operation, deadline, cancellation, capability check, or invariant failed. Use `unresolved` when the
operation succeeded but one or more requested semantic targets could not be determined. An empty
successful collection means that the requested scope was covered and no values matched; it must not
stand in for unresolved work. Compiler/process observations are diagnostics, while findings describe
user code and are not tool failures.

Control flow uses lower-case dotted stable codes and enums. Message and summary prose are display
projections and do not participate in canonical semantics. Locations use normalized source spans;
causes remain value-owned trees; actions, missing inputs, capabilities, precision, and attributes stay
machine-actionable. Cancellation, deadline expiry, budget exhaustion, and retryable I/O have distinct
codes.

Package authors register owned namespaced codes with `stable_code_registry`. Invalid and duplicate
codes are hard errors. A standard `unresolved_kind` must use its prescribed code. A custom unresolved
item must use a package-namespaced code and retain at least one missing input. Capability references
must exist in the supplied capability registry.

Expected failures return `result<T>` or `result<void>`. Propagation copies the original error value and
does not flatten or rebuild its cause chain. Exceptions are reserved for allocation/programming
boundaries; callbacks and interop adapters catch foreign exceptions and translate them to a structured
boundary error. Destructors do not throw.
