# ADR 0006: NG0 relation IDL と system claim envelope を単一 ID authority にする

- Status: Accepted
- Date: 2026-07-15
- Decision owner: schema-kernel
- Decision issue: #60
- Tracking issue: #56

> Amendment: ADR 0009 / Issue #63 upgrades the system envelope to
> `cxxlens.claim-envelope.v2`. The condition authority and relation ID decisions in this
> ADR remain accepted; producer input is now a tagged direct/derived basis.

## Context

bootstrap relation registry は6個の relation 名だけを列挙し、列、key、reference、merge、coverage、closure、
identity、evolutionを定義していなかった。統合設計書にも、claim envelope の `presence` と relation 列の
`presence` が重複し、`cc.call_site` に存在しない `direct_target` を query 例が参照し、存在しない
`compile_unit` を partition hint が参照する矛盾があった。

この状態では static generated API、runtime dynamic API、provider、store、query が同じ descriptor/column
identity を共有できず、minor evolution や hard/soft reference の判定も実装ごとに分岐する。

## Decision

`schemas/cxxlens_ng_relation_registry.yaml` を `cxxlens.relation-registry.v1` の accepted exact contract とする。
NG0 standard relation 17個と external exemplar `company.lock.acquire` の全 descriptor は、次を固定する。

- globally stable な descriptor ID と column ID
- authoritative key と循環しない domain ID projection
- functional/multivalued cardinality と conflict projection
- hard/soft reference と missing 時の `reject_batch` / `unresolved`
- envelope condition、interpretation、partition/index hint、coverage、closure、provenance
- optional additive minor と breaking major の evolution policy
- generated C++ tag と dynamic lookup が利用する共通 ID source

condition は system claim envelope の `system.claim.v2.presence` だけを authority とする。user relation は
`presence` または `condition_ref` 列を重複して持たない。condition fragment は envelope から partition、
conflict、coverage へ投影する。

call occurrence と direct target は次のように分離する。

```text
cc.call_site(call, compile_unit, caller, kind, source, receiver_static_type, ordinal)
cc.call_direct_target(call, target, resolution)
```

`cc.call_site.call = cc.call_direct_target.call` を join し、target から `cc.entity.entity` を参照する。
direct target は functional assertion であり、同一 call/condition/interpretation の異なる target は
same-domain conflict になる。target entity の未 materialize は soft reference として row を保持し、
`core.unresolved` で会計する。

static API は `relations[].descriptor_id` と `relations[].columns[].id` を定数として生成し、dynamic API は
relation name + semantic major と column name の lookup から同じ ID を返す。Logical Query IR は名前や
C++ enum ordinal ではなく、その descriptor ID / column ID を operand とする。

external relation は descriptor document または installation manifest から登録する。中央 enum、switch、
source list の変更は禁止する。unknown open symbol と unknown optional minor column は保持し、unknown closed
symbol、required minor column、key/cardinality/condition/identity change は fail closed または major change とする。

## Consequences

- bootstrap の hyphenated `build.compile-unit` / `cc.call-site` は exact underscore relation 名へ置換される。
- system envelope は通常の user projection に現れず、明示要求時だけ system column として参照できる。
- relation descriptor digest は canonical JSON projection から計算され、conformance report に全18件を出力する。
- provider batch、snapshot store、query runtime、public C++ generated surface の実装は後続 issue が所有する。
  本 ADR はそれらを実装済みとは宣言しないが、別 ID や別 call model の採用を認めない。
- external namespace の relation 追加は core source diff ではなく、descriptor/install input の追加として扱う。

## Verification

`tools/quality/check_ng_relation_contract.py` は schema、ID/key projection、hard-reference DAG、column/reference/
partition/index、merge/cardinality、system envelope、call model、evolution、static/dynamic ID parity を検査する。
`schemas/cxxlens_ng_relation_conformance_vectors.yaml` は hard/soft reference、condition 重複、identity cycle、
open/closed symbol、minor evolution、external registration、call query の positive/negative vector を固定する。
`report` mode は registry と全 descriptor の digest、および各 vector の stable decision/reason code を出力する。
