# ADR 0008: truth・guarantee・verification・provenance を独立した積構造として合成する

- Status: Accepted
- Date: 2026-07-16
- Decision owner: semantic-kernel
- Decision issue: #62
- Tracking issue: #56

## Context

既存設計は boolean check に support-pair を採用したが、knowledge/truth order、evidence aggregation、filtering
との境界を固定していなかった。guarantee は `unknown/under/over/exact` と verification enum を持つだけで、join、
union、distinct、derivation、summary での合成や exact の必要条件が不足していた。特に compiler verification と
runtime observation は異なる modality であり、enum ordinal による強弱比較は過大評価を生む。

provenance の圧縮と `summary_guarantee()` も contributor を失わず drill-down できる条件が未定義だった。この
ままでは unknown/conflict の false 化、coverage/closure gap を持つ exact、異なる calibration の confidence 比較、
summary による保証の格上げが backend ごとに発生し得る。

## Decision

`schemas/cxxlens_ng_semantic_guarantee_contract.yaml` を exact authority とする。

truth は positive/negative support の独立二ビットとする。knowledge order は componentwise inclusion、truth order
は negative reversed / positive forward とする。NOT/AND/OR は support-pair 式、複数 evidence の集約は両 support
の union である。unknown/conflict から bool への暗黙 coercion を禁止し、filter policy は selection だけを変更する。

approximation は `sound_positives` と `complete_scope` の独立二軸とする。

```text
unknown = 0,0   under = 1,0   over = 0,1   exact = 1,1
```

positive relational composition は軸ごとの AND とし、under と over の合成は unknown になる。limit は完全性を
証明した declared scope 以外では exact を under に下げる。group/aggregate は sealed input contract まで deferred
とする。

verification は canonical modality set と implication closure で表す。比較は closure set inclusion だけで行う。
compiler/link/runtime/differential modality は既定では相互に incomparable である。合成結果は input closure の
intersection とし、各 contributor の modality は drill-down に保持する。

exact は soundness/completeness だけでは主張できない。declared scope/condition/interpretation/assumptions、blocking
state のない coverage、必要 closure、condition partition、same-domain conflict と overlapping unresolved の不在を
validator が確認する。

confidence は calibration ID、population ID、metric が同じ場合だけ比較でき、truth/approximation を格上げしない。

provenance compression は algorithm、canonical child roots、count、child-set digest、resolver、retention policy を持つ
content-addressed node だけを許可する。resolver なしの child omission は禁止する。
`summary_guarantee()` は全 fragment の conservative meet とし、fragment count/set digest/drill-down ref を必須に
する。exact summary は全 fragment の exact precondition と condition partition が成立するときだけ許可する。

## Consequences

- `verification_level` の ordinal comparison は次世代 path で使用しない。
- assumption は canonical set union し、evidence edge による明示 discharge 以外で削除しない。
- join/derivation は condition intersection、interpretation equality、全 contributor/provenance union を行う。
- union/distinct は interpretation partition を跨いで保証を統合しない。
- 公開 C++ value/signature は #66 が所有するが、意味と serializer は本 ADR に従う。

## Verification

`tools/quality/check_ng_semantic_guarantee.py` は truth/approximation/modality/exact/summary/compression/confidence を
実行する。conformance vectors と property/metamorphic tests は unknown/conflict coercion、incomparable modality、
under/over composition、coverage/closure gap、contributor retention、lossy compression、backend/order parity を検査する。
