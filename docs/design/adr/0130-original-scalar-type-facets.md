# ADR 0130: Original scalar type facets

Status: accepted

Monet's boolean result-condition binder and integer overflow/narrowing providers need compiler-observed scalar facts. A type spelling, opaque structural digest or object-size width does not prove the original boolean kind or an integer value range.

The existing `cc.type` relation adds optional `builtin_kind`, `builtin_profile` and `builtin_state` under `clang22-original-builtin-type/1`. The kind is the exact canonical Clang22 `BuiltinType::Kind` discriminator, including `Bool`. Qualified and aliased builtin types retain that canonical kind; a pointer or record does not inherit it. Dependent classification is unknown, a compiler placeholder is partial, and an unsupported discriminator stays unsupported. Missing older facets remain unknown.

An independent `clang22-original-integer-representation/1` group retains `integer_bit_width`, `integer_signed`, `integer_profile`, `integer_state` and the optional original enum `integer_underlying_type`. Width comes from `ASTContext::getIntWidth`, excluding padding: `_BitInt(9)` has nine value bits even if its object occupies two bytes. Complete representation requires both width and signedness. Noninteger types are not applicable; dependent or missing enum underlying type remains unknown. An enum's underlying representation does not close its enumerator/value domain. Actual `Bool` has the value domain 0..1 independently of its compiler representation width.

Consumers use the existing original `cc.syntax_node.canonical_type` result/destination and `operand_type` cast-source references. No second type grammar, duplicated expression geometry, overflow absence, runtime purity, storage stability, alias or ownership inference is introduced. Supported profile and facet state must be checked independently from original contributor guarantees and from finite subject population closure. Contradictory optional payloads stay conflicting; incomplete geometry cannot be treated as a range.

The facets are authoritative optional payload and merge-conflict columns. Type identity projection and structural encoding are unchanged. The actual enum underlying type is a soft original association, preserving missing binding as unresolved. Saved earlier plans and sparse rows remain readable through the existing optional-column projection contract; absent facts are not filled in.

The public generated type tag remains compiler-neutral. A source-private native helper reads the original compiler enums and integer representation; ordinary native positive/negative fixtures cover canonical aliases, pointers, records, dependent and placeholder types, `_BitInt`, enums, arithmetic result types and actual inserted narrowing casts. Portable typed-row checks preserve old optional absence, reject foreign/type-invalid payloads and verify unchanged semantic type identity. This is product semantic input and adds no quality certificate or test-SHA ledger.
