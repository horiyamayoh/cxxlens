---
cxxlens_document_status: archived
cxxlens_authority: non-normative
cxxlens_replacement: docs/design/cxxlens_next_generation_integrated_design_ja.md
cxxlens_removal_issue: "#72"
---
# Workspace catalog contract

`workspace::open` reads every entry in a JSON Compilation Database without executing a shell. An
`arguments` array is authoritative; a legacy `command` string is tokenized only for compatibility.
Response files are expanded through the filesystem port, and plugin/load flags are rejected.

The compilation database parser accepts RFC 8259 strings (including BMP escapes and valid UTF-16
surrogate pairs), validates raw UTF-8, and accepts number, boolean, and null values in unknown
producer extension fields. Unknown fields do not affect required `directory`, `file`, `arguments`,
`command`, or `output` semantics. Malformed UTF-8, Unicode escapes, surrogate pairs, numbers,
duplicate keys, and non-JSON whitespace fail with `workspace.compile-database-invalid` plus a stable
`reason` and byte `offset`.

Parsing uses an explicitly bounded in-memory policy. `workspace_options` exposes these nonzero
budgets:

| Budget | Default | Meaning |
|---|---:|---|
| `compilation_database_byte_budget` | 256 MiB | File bytes, checked with the filesystem port before read and again before parse |
| `compilation_database_entry_budget` | 1,000,000 | Elements or members in any JSON container |
| `compilation_database_string_byte_budget` | 64 MiB | Decoded UTF-8 bytes in one JSON string |
| `compilation_database_nesting_budget` | 64 | JSON container depth |

The 256 MiB default removes the former 16 MiB compatibility ceiling while retaining a predictable
allocation bound for untrusted databases. Callers handling a larger generated database can raise the
byte/string budget explicitly; v1 does not silently switch to an unbounded or alternate streaming
parser. A limit produces `core.budget-exhausted` with stable `reason`, `limit`, `observed`, and parser
`offset` when applicable. Zero is rejected as `core.invalid-argument`, never interpreted as unlimited.

Each distinct normalized invocation becomes a build variant. Compile-unit and variant IDs exclude
the checkout root and enumeration order. `command_for` deliberately returns zero, one, or many
units; optional header inference reports its `stem-match` evidence in `explain_build_context`.

The workspace snapshot captures compilation database, source, configuration, schema/semantics,
tool, and LLVM identity inputs. `explain_build_context` rechecks mutable inputs and reports stale
rows rather than treating an old snapshot as compatible.

Run the contract tests with:

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --test-dir build/dev-clang -R workspace --output-on-failure
```
