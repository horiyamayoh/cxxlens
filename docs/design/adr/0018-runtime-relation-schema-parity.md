# ADR 0018: runtime relation descriptor を IDL schema projection と一致させる

- Status: Accepted
- Date: 2026-07-17
- Decision owner: relation-kernel
- Decision issue: #75
- Depends on: ADR 0003, ADR 0006, ADR 0017

## Context

relation IDL document は JSON Schema と relation contract validator の双方で検証されるが、public C++
`relation_descriptor::validate()` は緩い namespaced string、非 empty list、参照先の存在だけを検査していた。
この差により schema が拒否する名前、semantic major、column、重複 key/reference、incomplete functional mergeを
dynamic registryへ登録できた。

## Decision

runtime descriptorが保持する IDL projectionに次の同一制約を適用する。

- relation nameは2 segment以上の lowercase ASCII identifier、column nameは単一 lowercase ASCII identifier
- descriptor/column/reference ID と owner namespaceは schemaの stable ID、semanticsは versioned semantics pattern
- `semantic_major` は1以上で descriptor ID、version majorと一致
- key、reference source/target、conflict columnsはそれぞれuniqueで、keyは全 `claim_key` roleとexact一致
- functional assertionのconflict projectionは全 authoritative payloadとexact一致し、非 functional modeでは空
- reference source/targetは非空同数で、sourceはlocal column、target relation/columnはschema patternに一致

collection semantics は次で固定する。`key_columns`、`domain_identity.projection`、各 reference 内の
`source_columns` / `target_columns` は位置対応を持つ sequence であり、順序を保持する。`columns`、`references`、
`conflict_columns` は投入順に意味を持たない collection である。canonical form は columns を column ID、
conflict columns を stable ID、references を serialized field 全体
（source columns、strength、target relation、target columns）の strict total order で整列する。reference comparator が
equivalent を返すのは serialized record が同一の場合だけとし、source→target の位置対応は並べ替えない。

runtime `reference_strength::hard` は IDL の `strength: hard` と `on_missing: reject_batch` の組、
`soft_semantic` は `strength: soft_semantic` と `on_missing: unresolved` の組を表す。したがって required
`on_missing` を欠落させたり strengthと矛盾するpolicyを注入するsurfaceはない。

上位設計9.8が定義する `multiset` と `keyed_union` はruntime kernel modeとして維持する。IDLのmultivalued
`set` と同様にnon-functionalであり、conflict projectionを空にする。accepted registryから生成するstatic
descriptorは、IDL compiler入力時のfull schema validationと、生成後の同じruntime projection validationの
両方を通る。

## Failure contract

relation、column、referenceのshape違反はそれぞれ `sdk.relation-invalid`、`sdk.column-invalid`、
`sdk.reference-invalid` と field/detailを返す。digest mismatchはauthority-bearing descriptorで先に検査し、
trusted digestを保持した改変を ADR 0017どおり `sdk.descriptor-digest-mismatch` に保つ。dynamic descriptorは
registry adoption前にschema projectionを検査し、invalid descriptorをengineへ公開しない。

## Verification

`tests/unit/sdk/sdk_test.cpp` はvalid generated/dynamic descriptorに加え、empty/consecutive/numeric/hyphen relation
segment、semantic major 0、invalid owner/semantics/column、duplicate key、key-role mismatch、duplicate reference
source/target、missing reference shape、functional/non-functional merge projectionをC++ runtime pathで検査する。
同じtestは `relation_registry::add()` がschema-invalid descriptorを拒否することも固定する。
さらに reference/conflict collection の全 permutation、同一 source/target relation で target columns または strength
だけが異なる reference、generated/manual descriptor parity を検査し、canonical form と descriptor digest の一致を固定する。
