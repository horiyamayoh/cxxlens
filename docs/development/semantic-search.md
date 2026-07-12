# Semantic search and explanations

The M2 public search surface is available from `<cxxlens/search.hpp>`. It returns an immutable
`search_report<T>` whose rows are authoritative: JSON and Markdown are projections of the same
matches, evidence, coverage, unresolved state, guarantee, precision, and predicate accounting.

```cpp
#include <iostream>

#include <cxxlens/search.hpp>

int run_search(const cxxlens::workspace& workspace)
{
    auto report = cxxlens::search::calls_to_method(workspace, "Base", "step");
    if (!report) {
        std::cerr << report.error().explain() << '\n';
        return 2;
    }
    std::cout << report.value().to_markdown();
    return report.value().coverage().complete() ? 0 : 3;
}
```

`calls_to_method` resolves the receiver and method through semantic symbol/type facts. It includes
derived receivers and virtual override candidates, while keeping the static target separate from
the possible target set. A virtual open-world result is therefore normally `best_effort` with a
stable unresolved code; complete parse coverage alone does not make the program world closed.

Use `search_options::strict_coverage` when partial parse/fact coverage must fail the operation. The
default returns a valid partial report so callers can distinguish usable matches from missing
coverage. `result_limit` similarly adds `search.result-limit-exhausted` instead of silently claiming
completeness. Setting `include_unresolved` to false only compacts Markdown details; canonical JSON
and the `unresolved_items()` accessor remain authoritative and complete.

For rejection analysis, `<cxxlens/explain.hpp>` provides `why_not_matched`. Its structured result
keeps matched, predicate-rejected, and unresolved candidates separate and includes bounded examples
plus stable reason codes. Never branch on its prose; use the structured properties and codes.

The canonical report, explanation, and task-card schemas are installed as
`cxxlens_search_report.schema.yaml`, `cxxlens_explanation.schema.yaml`, and
`cxxlens_agent_task_card.schema.yaml`.

For a complete installed-package example and clean-checkout acceptance command, see
[`m2-acceptance.md`](m2-acceptance.md) and `examples/m2-flagship`.
