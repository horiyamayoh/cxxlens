# ADR 0037: semantic claim set と evidence occurrence set を分離する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: claim-kernel/store/query-runtime
- Decision issue: #94
- Depends on: ADR 0007, ADR 0009, ADR 0013, ADR 0019

## Context

非 multiset relation の `claim_batch::commit()` は同じ content ID を一件へ縮約したが、content identity に含まれない producer ID、
input basis、provenance、guarantee、stage の差を同時に捨てた。比較器にもこれらの field がなく、残る metadata は入力順と不安定 sort に
依存した。Store の partition は同一 content occurrence をすべて拒否し、複数の独立 evidence を lossless に保持できなかった。

## Decision

semantic claim content と evidence occurrence は別の canonical set とする。

- 非 multiset relation の semantic claim set は canonical sorted unique content ID 集合である。
- evidence occurrence は claim envelope の全 field を versioned canonical tuple へ投影して total order と exact equality を決める。
- 全 metadata が同一の occurrence だけを deduplicate し、同じ content でも producer、basis、provenance、guarantee、stage のいずれかが
  異なる occurrence はすべて保持する。
- batch content identity v2 は公開 `claim_batch_content_encoding()` と、それだけを hash する `claim_batch_content_digest()` が所有し、
  schema tag、claims、unresolved references、conflicts、differential disagreements の4 collectionを `canonical_value` の型付き
  self-delimiting tuple で符号化する。各 record の全 field と collection count を bind し、record は canonical total order、内部 set は
  canonical sorted unique、claim multiplicity は relation law を維持する。区切り文字の禁止や prose 連結には依存しない。
- partition manifest の claim set/count は semantic content の一意集合を表し、payload v5 partition envelope と annotation projection は
  canonical evidence occurrence set を lossless に保持する。
- query scan は非 multiset relation の同じ content occurrence を一つの semantic rowへ集約し、producer、provenance、contributor
  guarantee を canonical set union する。summary guarantee は既存の conservative meet law を適用する。

## Consequences

- 複数 provider/evidence/guarantee が同じ事実を裏付けても attribution を失わない。
- exact duplicate は multiplicity を増やさず、全入力 permutation と backend で同じ byte projection になる。
- semantic snapshot identity の claim set は evidence occurrence 数で分岐しない。payload の occurrence 差は既存 collision validation が
  検出する。
- multiset relation の semantic multiplicity law は変更しない。

## Verification

`tests/unit/sdk/sdk_test.cpp` は 5 occurrence の 120 permutation、metadata 差の保持、exact duplicate dedup に加え、batch content identity の
delimiter/NUL collision corpus、全 nested field の binding、4 collection の permutation 不変性を検証する。
同 test は公開 `canonical_binary_decode()` による encode→decode→encode の byte-for-byte round trip と batch v2 golden digest も固定する。
`tests/unit/sdk/store_test.cpp` は memory/SQLite/reopen の lossless occurrence set と canonical export parity を検証する。
`tests/unit/sdk/query_runtime_test.cpp` は同一 content が一 row となり producer/provenance/guarantee 集合へ統合されることを検証する。
