# ADR 0136: Original integer object storage

Status: accepted.

Contract ID: `clang22-original-integer-object-storage/1`. Authority is the relation
registry and original Clang ASTContext, under the integrated design. Write scope
is additive optional `cc.type.v1.integer_object_bytes`, `integer_storage_profile`
and `integer_storage_state`; semantic type identity is unchanged. Support is the
existing Clang 22 native application path. Other providers and older saved inputs
retain an unobserved storage axis.

Monet's endian consumer needs original object storage separately from integer
value width. `ASTContext::getIntWidth` excludes padding and cannot determine the
object sizeof, including `_BitInt`. The native producer reads the original
canonical nondependent complete integer or enum object through
`ASTContext::getTypeSizeInChars`. The result is in original target character
storage units. It never rounds value bits or assumes a host eight-bit byte.
The independent `cc.abi_surface.v1.char_width_bits` is original TargetInfo width.

The atomic profile/state group is independent of optional size. Complete requires
an original eligible object and positive observed size. Noninteger is
not_applicable; dependent, placeholder, incomplete enum and unsupported compiler
geometry remain unknown/partial/unsupported with no invented size. Existing
world, condition, interpretation, claim/provenance and conflict payloads remain
attached. Contradictory observed storage values conflict; no cross-world type
association, numeric predicate or endian guarantee is added.

The independent consumer can produce proved, disproved, unknown, partial and
conflicting outcomes only after its original access, source, ABI and closure
axes are admitted. Missing storage stays actionable unknown. Completion order
is schema and generated tags, native producer, actual integer/enum/Bool/_BitInt
positive controls, pointer/dependent/incomplete negative controls and bounded
failure controls, independent consumer, then ordinary main workflow. Existing
native traversal and retained population bounds apply before allocation; failure
publishes no fabricated complete geometry.
