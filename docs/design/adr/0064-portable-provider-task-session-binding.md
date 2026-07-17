# ADR 0064: portable provider task は session と全 semantic input を受理前に bind する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: steward.ng-provider-runtime
- Decision issue: #122
- Depends on: ADR 0010, ADR 0012, ADR 0043, ADR 0044, ADR 0045, ADR 0063

## Context

in-process `run_worker` は旧来、nonempty task ID、project catalog、nonzero budget しか task acceptance 前に検証しなかった。requested
descriptor、condition、interpretation、provider identity は task ID に bind されず、callback context は未要求の valid relation を stream
できた。logical surface は accepted provider protocol より弱く、同一 task/cache identity で異なる semantic work を実行できた。

## Decision

`provider_session` は provider ID、version、semantic contract digest、exact offered/required relation descriptor、interpretation domain、input/output
stage を value-own する。`task::make` は session、validated project catalog ID/digest、requested descriptor ID/digest、condition、interpretation、
offer/require、stage、dependency group を canonical binary tuple にし、semantic digest v2 から `task:` ID を導出する。集合は factory で canonical
sort し、validator は grammar、duplicate/conflict、descriptor shape/digest、offer subset、interpretation membership と task ID を bottom-up に
再検証する。

`run_worker` は task と callback provider の ID/version/semantic contract を `task_accepted` 前に exact compare する。callback `context` は task の
requested descriptor と dependency group だけを relation sink に許可する。provider が rejection を無視して success を返しても context は違反を
記憶し、task を failure にする。`batch_begin` control は task ID、descriptor ID/digest、dependency/atomic group、batch ID を持ち、shared logical/process
validator が exact task binding を検証する。

## Verification

未要求 relation、descriptor shape mismatch、duplicate/conflicting outputs、invalid condition/interpretation、task field mutation、invalid provider
ID/version を pre-accept negative test にする。exact session/task を positive test にし、同じ derived task ID を in-process と process surface に渡して
shared validator の semantic verdict が一致することを確認する。machine contract、public API catalog、provider protocol、Doxygen と acceptance
traceability を同時に更新する。
