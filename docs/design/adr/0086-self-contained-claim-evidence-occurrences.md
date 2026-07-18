# ADR 0086: claim evidence occurrence は self-contained envelope とする

- Status: Accepted
- Date: 2026-07-18
- Issue: #155

## Context

Issue #155 は `claim.evidence` が occurrence ID を参照し、batch が別 collection の `claim_evidence` record を所有するモデルを前提に、
missing、orphan、ambiguous、別 claim subject への付け替えを指摘した。しかし対象 commit `55efb42` と current public API のいずれにも
その参照 model は存在しない。ADR 0037 が定めた evidence occurrence は、row、semantic identities、condition、interpretation、producer、
input basis、provenance、guarantee、stage を含む一つの完全な `claim` envelope である。

存在しない参照 graph を後付けすると、同じ情報を envelope と detached record の二箇所で所有し、かえって両者の不一致状態を新設する。

## Decision

- evidence subject kind は closed な `claim-envelope` 一種類とする。row-only subject と detached evidence record は public/persisted claim
  model に導入しない。
- 一 occurrence の subject は同じ record 内の descriptor、semantic key、assertion、content、detached row で構造的に決まる。文字列 ID
  の外部参照や偶然一致で帰属を決めない。
- 一 occurrence はちょうど一 semantic claim content に属する。複数 occurrence が同じ content を支持できるが、一 occurrence を異なる
  content 間で共有しない。
- batch の occurrence collection は `claim_batch_result::claims` そのものである。従って reference union と record set は構造上同一で、
  missing、orphan、one-to-many resolution は表現不能である。full-envelope exact duplicate のみを canonicalization 時に縮約する。
- builder commit と Store adoption は `validate_claim()` で envelope identity を再計算する。persisted partition envelope の load も
  `decode_claim()` から同じ validator を呼び、subject field の付け替えを fail closed にする。
- 将来 detached evidence kind を追加する場合は、新しい tagged union、exact reference cardinality、subject law、wire version、load validator を
  schema-first で追加する。現行の opaque string field にその意味を推測させない。

## Consequences

- claim が存在しない evidence を参照する状態と、どの claim にも属さない evidence record は構築できない。
- semantic key、assertion、content、row のいずれかを別 claim へ付け替えると identity revalidation が拒否する。
- query annotation は persisted claim envelope からの projection であり、独立した attribution authority ではない。
- detached record の共有を許さないため、共有 cardinality は「一 occurrence : 一 claim content」で固定される。

## Verification

`tests/unit/sdk/sdk_test.cpp` は public `claim` / `claim_batch_result` に detached `evidence` member がないことを compile-time に固定し、
semantic subject の付け替え拒否、metadata の異なる同一 content occurrence の保持、exact duplicate の縮約を検証する。
`tests/unit/sdk/store_test.cpp` は memory、SQLite、reopen 後の occurrence envelope parity を検証する。SDK quality checker は catalog、public
shape、claim occurrence projection、commit/store/load の共通 `validate_claim()` 経路を固定する。
