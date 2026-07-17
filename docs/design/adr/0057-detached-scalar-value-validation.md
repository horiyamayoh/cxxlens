# ADR 0057: detached scalar は全 ingestion 境界で同じ値 contract を再検証する

- Status: Accepted
- Date: 2026-07-17
- Issue: #114

## Context

`detached_cell::validate()` は scalar kind と `std::variant` alternative の一致しか検査せず、digest/semantic version/strong ID の
grammar、closed symbol membership、set bytes の canonicality を検査していなかった。strict UTF-8 は後から追加されたが、raw
Logical Query IR の decoded literal は `detached_cell` validation を迂回した。このため row、provider、store、query surface の
accept/reject 集合が分岐し得た。

## Decision

relation registry 1.2.0 の `detached-cell-value-v2` を scalar value authority とする。digest は canonical lowercase SHA-256
grammar、semantic version は u32 三成分かつ leading zero 禁止、typed/strong ID と symbol value は strict UTF-8・nonempty・
control-free、typed ID parameter は canonical `*_id` とする。open symbol は未知値を保持し、closed symbol は registry の exact
closed contract/value membership を要求する。未知 closed contract も fail closed とする。general UTF-8 text 内の valid control
code の意味制約は column schema に残すが、unknown reason は control-free とする。

`set<T>` は empty を zero bytes、nonempty を `u32-le length || element bytes` の連結で表す。element は strict byte lexical order
かつ unique とし、nested scalar contract で再検証する。truncation、zero-length element、duplicate、unsorted、invalid nested value
は非 canonical とする。

authoritative implementation は `detached_cell::validate()` に集約し、row builder / `validate_row`、provider column decode、SQLite
payload reopen は既存の同一 call pathを使う。typed query builder に加え raw Logical Query IR の decoded literal も同じ validator
へ通し、surface 固有 fallback を設けない。

## Verification

digest、semantic version、closed/open symbol、全 invalid UTF-8 sequence class、typed ID/unknown reason、canonical/noncanonical set
corpusを C++ regression と machine-readable relation vector の両方で固定する。typed builder と raw Logical Query IR の set bytes
が同じ結果になること、row builder が invalid scalar を retention 前に拒否することを検証する。
