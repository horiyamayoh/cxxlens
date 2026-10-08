# ADR 0143: Original field bit-field facets

- Status: accepted
- Contract: `clang22-original-field-bitfield/1`
- Authority: relation registry and original Clang 22 `FieldDecl`
- Owner/write scope: exact adapter; optional `cc.entity_detail.v1` columns
- Stability: additive, original primitive observation

Monet's LP64/LLP64 function/class comparison consumes actual TargetInfo integer
storage and record/ABI layouts. Aggregate layout and opaque ABI bytes cannot
establish whether a field has a written bit-field width or close the field-value
input domain. The independent consumer needs this fact for every original field,
including non-bitfields, zero-width unnamed fields and dependent template fields.

Five optional columns preserve the original classification:
`field_bitfield_profile`, `field_bitfield_state`, `field_is_bitfield`,
`field_bit_width`, and `field_bitfield_reason`. The first three are all-or-none.
The exact `FieldDecl::isBitField()` result is retained. A valid non-bitfield has
complete classification and no width. A valid nondependent bitfield has the
actual Sema-evaluated `getBitWidthValue()`, including zero; this width is not a
host integer type width, record layout offset or inferred value range. Invalid
fields, missing width expressions and dependent/unexpanded widths retain partial
state and a reason. Observation must not instantiate a template or evaluate a
new user expression. Non-field declarations have no facets; legacy sparse rows
remain unclassified.

Existing original `cc.record_surface` field count and member IDs enumerate all
original physical fields before source/ID admission; the query's independent
record inventory closes the record population. A consumer must bind the exact
field entity, compile unit, source and world to its original detail and record
membership. Missing, foreign, ambiguous and conflicting members preserve
unknown/partial/conflicting. No count, layout-change predicate, alias closure,
protocol outcome or metric availability verdict is emitted by the adapter.

The generated relation row view exposes optional typed fields without changing
any existing SDK projection DTO layout. Coverage, source provenance, closure and
frontiers use existing claim/query channels and resource/cancellation limits.
ABI and record-surface projections do not consume these five facets, and accept
their absence in older saved `cc.entity_detail.v1` rows. They preserve the
original row without adding cells or interpreting missing classification as
false. Present cells retain descriptor type/value validation; all other existing
column checks remain unchanged. Consumers of bit-field classification still
require the original complete field facts and return unknown for sparse rows.
Tests cover actual signed/unsigned/non-bitfield/zero/dependent fields, independent
full record census, missing and foreign memberships, sparse legacy and malformed
typed fields, repeat determinism and ordinary bounded analyzer output failure.
