# ADR 0007: Logical Query IR v1 を annotated multiset algebra として固定する

- Status: Accepted
- Date: 2026-07-15
- Decision owner: query-kernel
- Decision issue: #61
- Tracking issue: #56

## Context

統合設計書は NG0 query を `positive monotone` と呼びながら、`group`、`aggregate`、`order_by`、`limit` を
同じ operator set に含めていた。しかし、unordered input への `limit`、未完 group の aggregate、backend の
scan order に依存する partial result は単調でも決定的でもない。set/bag、duplicate、optional absence、semantic
unknown、condition、interpretation、claim contributor、provenance の operator 間伝播も規範化されていなかった。

このままでは static DSL と dynamic DSL が同じ列 ID を参照しても、memory/SQLite backend、parallel schedule、
cache、minor schema 差によって意味的に異なる結果を返し得る。また physical index や task order が Logical IR
digest に混入すると、backend-independent な query authority と continuation binding を構成できない。

## Decision

`schemas/cxxlens_ng_logical_query_contract.yaml` を `cxxlens.logical-query-contract.v1` の accepted authority とする。
Logical Query IR v1 の値は、次を持つ annotated multiset occurrence である。

```text
typed cells + positive multiplicity
+ condition universe / canonical alternatives
+ interpretation ID
+ canonical assertion contributor set
+ canonical provenance set
```

cell は `present(type,value)`、`absent`、`unknown(reason)` の明示 tag を持つ。SQL NULL と暗黙三値論理を意味契約に
使用しない。通常の等値比較は `equals_present` であり、absence と unknown は一致しない。

NG0 operator は次の11個に限定する。

```text
scan filter project inner_join semi_join union distinct
order_by limit condition_restrict interpretation_restrict
```

`scan` から `distinct` および二つの restriction は positive algebra である。`order_by` と `limit` は
non-monotone boundary operator として NG0 に残す。`group` と `aggregate` は sealed group / closure / partial
publication contract がないため NG1 以降へ移す。

既定 collection は bag である。`project` は multiplicity を保存し、`union` は bag addition を行う。
`distinct` だけが values + universe + interpretation で occurrence を統合し、condition、contributors、provenance
を canonical set union する。join は condition intersection、interpretation exact equality、multiplicity product、
evidence set union を行う。semi-join は left multiplicity を増やさず、全 compatible witness の condition/evidence
を統合する。

relation/query result は unordered を既定とする。`order_by` は user key の後ろに canonical annotated-row digest を
必須 tie-break として付加し、total order を作る。`limit` は total-ordered input だけを受理し、その sealed prefix
を返す。order なし limit は validation error とする。

partial result は次のときだけ publish する。

- upstream が logically complete な output cap は unordered なら canonical semantic set prefix、ordered なら
  total-order prefix を返し `truncated` とする。
- upstream interruption の unsealed row は破棄する。sealed row がなければ `failed_before_result` とする。
- `cancelled_with_partial` は少なくとも一つの sealed row がある場合だけ使用する。
- continuation は total-ordered sealed prefix に限り、IR digest、snapshot、descriptor digest、order specification、
  last total key、result schema digest のすべてに bind する。
- partial aggregate は NG0 では常に拒否する。

static/dynamic surface、source location、runtime budget、physical plan は normalized semantic digest から除く。
relation requirement は descriptor ID 順、commutative predicate は child digest 順へ正規化する。physical index、
join algorithm、thread、spill、page、cost、cache 情報を versioned Logical IR authority に含めない。

## Consequences

- NG0 全体を `positive monotone` と呼ばない。positive algebra と deterministic boundary operator を区別する。
- optional minor column が descriptor にない場合、通常参照は拒否する。明示的な
  `absent_if_schema_missing` だけが tagged absent を生成できる。required column missing は常に拒否する。
- dynamic literal は explicit type を必須とし、column の present type との exact match だけを許可する。
- static/dynamic builder の exact C++ signature は Issue #66 が所有するが、生成する normalized IR と digest は本
  ADR に従う。
- production memory/SQLite query runtime の qualification は後続実装 issue が所有する。本 ADR の reference
  evaluator は logical semantics の executable oracle であり、legacy query engine の完成を宣言しない。

## Verification

`tools/quality/check_ng_query_contract.py` は contract/IR schema、11 operator descriptor、arity/argument/column/type、
unordered limit、minor schema、physical field、partial/continuation binding を検査する。同 tool の reference
evaluator は同一 query/data を memory と SQLite、forward/reverse/seeded-shuffle の6経路で評価し、annotation を
含む canonical semantic result を比較する。

`schemas/cxxlens_ng_query_conformance_vectors.yaml` は全11 operator の negative vector、static/dynamic digest、
backend parity、optional/unknown/NULL、literal type、continuation、truncation、partial aggregate、physical authority
の positive/negative vector を固定する。report は contract と全 operator の digest、および backend comparison
数を出力する。
