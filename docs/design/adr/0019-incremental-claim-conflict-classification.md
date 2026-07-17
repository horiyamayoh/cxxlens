# ADR 0019: incremental claim commitを既存claimとの同一classificationへ拡張する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: claim-kernel
- Decision issue: #76
- Depends on: ADR 0006, ADR 0008, ADR 0009

## Context

`claim_batch::commit(engine, existing)` は既存claimをvalidationとreference解決には使用したが、merge、same-domain
conflict、cross-domain differential disagreementの比較対象には含めていなかった。そのため同じclaim集合でも、
one-shot batchでは矛盾を検出し、既存snapshotへのincremental batchでは見逃すpublication-boundary依存があった。

## Decision

commitは次のpairだけをfunctional assertion classificationへ入力する。

```text
new × later-new
new × unique-existing
```

existing × existingは既知side channelの再掲を避けるため比較しない。各pairはdescriptor、semantic key、payload、
condition overlap、interpretation domainを同じ順で判定する。exact content duplicate、same authoritative payload、
disjoint conditionはrecordを生成しない。same-domain payload mismatchは `claim_conflict`、cross-domain payload
mismatchは `differential_disagreement` とする。

pairの左右はnew/existingの由来ではなくcanonical claim orderで決める。existing inputをcanonical sortし、exact
content duplicateを除去する。生成したconflict/differential recordも全identity fieldでsort/uniqueしてからbatch
digestへ入力する。これによりone-shotとsplit publicationで左右、overlap fragment、assertion/content pairを含む
classificationが一致する。

reference validationは従来どおりclassificationより先に行い、hard reference不足はbatch全体を拒否する。
accepted new claimsだけを `claim_batch_result::claims` に返し、existing claimsを再発行しない。

## Verification

`tests/unit/sdk/sdk_test.cpp` はexisting Aに対するnew Bについてsame-domain overlap conflict、same payload非競合、
disjoint condition非競合、cross-domain differentialを検査する。同じA/B集合のone-shotとsplit commitで完全な
conflict recordが一致すること、およびduplicate existing claimがrecordを重複させないことも固定する。
