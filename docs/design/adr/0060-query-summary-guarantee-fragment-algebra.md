# ADR 0060: query summary guarantee は寄与 fragment の lossless meet とする

- Status: Accepted
- Date: 2026-07-17
- Decision owner: query-runtime
- Decision issue: #117
- Depends on: ADR 0008, ADR 0014, ADR 0021, ADR 0029, ADR 0059

## Context

旧 `summary_guarantee()` は最終 result に寄与した fragment ではなく、到達した scan の全 annotation を平坦化していた。
このため filter / interpretation restriction で除外した weak claim が summary を downgrade し、逆に同一 domain の overlap
conflict、unresolved、coverage、relation/scope-specific closure を exact 判定へ反映できなかった。公開値も accepted semantic
guarantee contract が要求する condition / interpretation partition と digest-bound drill-down を表現できなかった。

## Decision

各 operator は `annotated_row` の condition、interpretation、contributor guarantee、claim contributor、provenance を保持し、join、
semi join、union、distinct は既定の set composition を適用する。最終 summary は公開された row に実際に寄与する fragment だけを
canonicalize して conservative meet する。empty result だけは absence proof のため reachable scan fragment を使用し、applicable
closure がなければ exact にしない。

`query_summary_guarantee` は approximation、scope、condition partition、interpretation partitions、canonical assumption ID set、
implication-closed modality intersection、fragment count、semantic fragment-set digest、drill-down ref、全 fragment を保持する。
drill-down ref は同じ canonical fragment bytes の digest に bind する。exact は全 fragment exact、input/execution complete、applicable
closure、blocking coverage/conflict/unresolved の不在、complete condition partition がすべて成立した場合だけ許可する。複数の
incomparable scope/domain を横断して exact へ upgrade しない。

## Verification

除外した weak interpretation/row が surviving summary に入らないこと、overlap conflict が fragment に記録され exact を拒否する
こと、condition/interpretation partition と全 fragment が canonical result へ残ることを検証する。assumption union と modality
implication closure は accepted conformance vector と同じ exact/under/unknown component law で固定し、memory / SQLite は同じ
canonical summary bytes を生成する。
