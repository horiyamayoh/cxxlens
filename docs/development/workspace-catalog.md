# Workspace catalog contract

`workspace::open` reads every entry in a JSON Compilation Database without executing a shell. An
`arguments` array is authoritative; a legacy `command` string is tokenized only for compatibility.
Response files are expanded through the filesystem port, and plugin/load flags are rejected.

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
