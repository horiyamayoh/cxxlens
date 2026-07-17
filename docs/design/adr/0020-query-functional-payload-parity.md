# ADR 0020: query side channelをclaim kernelのfunctional payload lawへ統一する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: query-runtime
- Decision issue: #77
- Depends on: ADR 0006, ADR 0007, ADR 0014, ADR 0019

## Context

query runtimeはsnapshot annotationからconflict/differential side channelを再構成する際、claim `content` IDの
不一致をpayload不一致として扱っていた。content IDはassertion IDを含み、assertionはcondition、interpretation、
producer semantic contractにbindされる。そのため同じrow payloadでもcondition/domainが異なる正常なclaimを
false conflict/differentialとして報告し、claim ingestion時の分類と食い違っていた。

## Decision

claim kernelのfunctional payload projectionを`src/sdk/claim_internal.hpp`で共有する。projectionはrelation
descriptorの`conflict_columns`順canonical tupleを`conflict-payload` identity domainでdigestする。claim batchと
query runtimeはこの1実装だけをpayload equalityへ使用し、claim content IDをpayload比較へ使用しない。

query conflict/differential reconstructionはfunctional assertion relationだけを対象とする。同じdescriptorと
semantic keyのannotation pairをcanonical claim orderで比較し、payload digestが異なりcondition overlapがある
場合だけ、same-domainを`claim_conflict`、cross-domainを`differential_disagreement`へ分類する。relation fieldは
descriptor IDではなくclaim kernelと同じrelation nameを返す。recordは全identity fieldでcanonical sort/unique
する。

shared projection失敗はside channel omissionにせずquery executionのstructured failureとして伝播する。

## Verification

`tests/unit/sdk/query_runtime_test.cpp` は同一key/payloadでconditionだけ異なるsame-domain pair、同一payloadでdomain
だけ異なるpairを非 disagreementとして固定する。payloadが異なるsame-domain/cross-domain pairはそれぞれexact
1 recordを生成し、同じ8 claimを`claim_batch::commit()`へ渡した結果とquery resultの全record fieldが一致する。
