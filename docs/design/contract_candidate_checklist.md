# Contract Candidate checklist

Use this template for each package owner issue #43 through #51. A checked item requires a
machine-readable catalog/manifest field and its validator evidence; prose alone is insufficient.
The package owner may transition only to `candidate`. Issue #54 is the sole `frozen` authority.

## Identity and surface

- [ ] Every assigned API ID and atomic unit appears exactly once.
- [ ] Every callable has an exact C++23 signature, source, and signature fingerprint.
- [ ] Every coherent family lists all members and is indivisible.
- [ ] Naming, factory/constructor, overload/options, nodiscard, explicit conversion, and value traits are recorded.

## Meaning and execution

- [ ] Value/reference/pointer/view/handle ownership, nullability, backing storage, and invalidation are explicit.
- [ ] Const, noexcept, copy/move, thread-safety, thread affinity, and callback rules are explicit.
- [ ] Cancellation/deadline/time/memory/path/state/output budgets and observation points are explicit.
- [ ] Empty, unresolved, unsupported, ambiguous, partial, and failure outcomes select valid decision-table rows.
- [ ] Evidence, coverage conservation, confidence, precision, and guarantee downgrade rules are explicit.
- [ ] Total ordering, tie-breaker, duplicate policy, and equal/divergent/conflicting variant handling are explicit.

## Ownership, schema, and change

- [ ] Public type, shared component, fact/capability provider, and schema owners are unique and non-dangling.
- [ ] API/component/provider dependency edges and package integration consumer boundaries are complete.
- [ ] Canonical JSON, schema ID/version, all five version axes, unknown data, migration, and rebuild policy are explicit.
- [ ] Additive/source/semantic/schema change classification and fingerprint invalidation are explicit.
- [ ] Transition issue equals this package owner issue and state is `candidate`; implementation/readiness state is unchanged.

## Evidence

- [ ] Requirement/use-case traceability, Doxygen obligations, and a compile-checked usage example are identified.
- [ ] Concrete positive, negative, and ambiguous contract fixtures have bounded expected outcomes.
- [ ] No production algorithm, implementation conformance, or final freeze is claimed.
- [ ] Catalog, task packet, ownership, ready report, and authorization inputs are regenerated and drift checks pass.
