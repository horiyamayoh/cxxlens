# Configuration resolver contract

`configuration` resolves each dotted key independently with this fixed precedence:
API option, CLI overlay, named profile, config default, then safe built-in default. Inputs are
immutable; `overlay()` returns a new value and retains the losing origins for `explain()`.

The loader accepts a deliberately restricted YAML mapping: duplicate or unknown keys, aliases,
custom tags, malformed indentation, non-string list items, documents over 1 MiB, lines over 16
KiB, more than 4096 nodes, or nesting beyond 32 levels are rejected. `${NAME}` interpolation is
allowed only for schema-declared path or secret fields. It cannot change analysis precision,
macro/template policy, transformation safety, or other semantic keys.

`load_nearest()` canonicalizes paths, stops at `.cxxlens-root` or `.git`, and verifies that a found
configuration remains within that boundary after symlink resolution. Filesystem and environment
access are behind injectable ports.

`resolved_json()` and `explain()` use the common canonical JSON writer. Secret values are emitted
as `[redacted]`; absolute roots, timestamps, process IDs, thread IDs, and cache state are excluded.
The input, resolved, and explanation contracts are registered as schema version 1.0.0.
