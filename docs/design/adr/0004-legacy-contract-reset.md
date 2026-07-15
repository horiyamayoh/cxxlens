# ADR 0004: 旧公開 API freeze の互換性リセット

- Status: Accepted
- Date: 2026-07-15
- Decision owner: architecture-governance
- Decision issue: #57
- Tracking issue: #56

## Context

旧 Phase B は22 package、124 API、124 atomic unitをfreezeし、47 conformant / 77 unimplementedを
記録した。Issue #55 は残り77 APIの実装を計画したが、次世代製品定義と同時に実行すると新旧二つの
authorityと長期bridgeを生む。

## Decision

- 旧 catalog、freeze、package candidate、readiness/task packetはlegacy provenanceとして保持する。
- 旧freezeのPhase C authorizationを失効し、Issue #55を再開しない。
- 旧API ID/signatureと47 conformant API evidenceをmigration baselineへ封印する。
- legacy catalogへの新規API追加、signature変更、旧atomic unit dispatchをfail-closedにする。
- 新規 public contract は次世代Relation Registry/Public C++ API Catalog/Acceptance Manifestへ追加する。
- 互換bridgeはone-way、partiality preserving、期限付きとし、Issue #72で完全除去する。

## Consequences

pre-1.0であるためsource compatibilityは約束しない。ただし既存の有用な意味契約とacceptance evidenceは
捨てず、asset ledgerで `inherit / migrate / replace / archive / delete` のterminal stateを持たせる。

## Verification

Issue #57 のno-new-legacy-API gateとrevoked runnerが新規dispatchを拒否する。Issue #72がlegacy
production pathを除去し、Issue #71がlegacy authority/reference/bridge count 0を最終検証する。
