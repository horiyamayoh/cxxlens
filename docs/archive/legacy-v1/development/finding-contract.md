---
cxxlens_document_status: archived
cxxlens_authority: non-normative
cxxlens_replacement: docs/design/cxxlens_next_generation_integrated_design_ja.md
cxxlens_removal_issue: "#72"
---
# Diagnostic and finding contract

A diagnostic is an observation from a compiler, formatter, process, or other tool. A finding is a
semantic result about user code. They are separate value types and collections; no diagnostic is
implicitly promoted to a finding, and diagnostic/message prose never controls identity or branching.

Finding identity uses the versioned `cxxlens.finding-id.v1` canonical domain and includes the
rule/recipe ID, full semantic subject ID, primary semantic file/range, variant signature, and explicit
identity parameters. Message, severity, absolute checkout root, jobs, hash seed, and diagnostic prose
are excluded. Distinct canonical identity payloads that collide remain a hard error through the
shared identity collision registry.

Finding sets use a total order of rule, primary file/range, subject, variant, and full finding ID.
Equivalent duplicates occur exactly once. Reusing an ID with a different material payload is an
invariant failure. Severity/confidence filters are pure copies of whole rows, preserving IDs,
evidence, unresolved items, and coverage. JSON, Markdown, SARIF, and prose rendering remain owned by
the serializer issue; this contract exposes renderer-neutral explanation inputs.
