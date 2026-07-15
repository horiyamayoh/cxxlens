# ADR 0003: Versioned Relation Kernel を安定核とする

- Status: Accepted
- Date: 2026-07-15
- Decision owner: schema-kernel
- Decision issue: #57
- Tracking issue: #56

## Context

旧 `fact_kind` と use-case profile は built-in fact を扱えるが、runtime extension を opaque
string/JSON payloadへ追いやり、typed query、reference validation、merge、condition、coverage、
provenance を built-in と同じ契約で利用できない。任意 reducer callback は再現性と trust boundaryも
弱める。

## Decision

Versioned Relation Schema、Semantic Claim、Immutable Snapshotをkernelのデータ契約とする。

- relation はnamespaced name、semantic major、exact descriptor digestを持つ。
- schema はcolumn、authoritative key、cardinality、reference、merge、condition、interpretation、
  coverage、closure、provenance、evolutionを宣言する。
- build-time schema はgenerated static API、runtime schema はdynamic APIを提供し、両者は同じ
  descriptorとLogical Query IRを使う。
- observation、assertion、canonical claim、derived claimを分離する。
- arbitrary normalization/reductionはversioned providerとして実行し、kernel callbackにしない。

## Consequences

Exact NG0 relation IDL、query algebra、claim/snapshot identity、provider protocol は後続 contract issue
Issue #60〜#65 が固定する。これらが未確定の間、個別 relation/API のstable宣言を増やさない。

## Verification

Issue #67 はexternal relation追加のcore diff 0、static/dynamic descriptor parity、claim validation、
same-domain conflictとcross-domain disagreementをproduction walking sliceで検証する。
