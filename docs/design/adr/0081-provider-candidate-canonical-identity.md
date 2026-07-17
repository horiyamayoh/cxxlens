# ADR 0081: provider candidate canonical identity

- Status: Accepted
- Date: 2026-07-18
- Owner: Issue #150

## Context

Provider selection previously ordered candidates only by discovery source, provider ID/version, and binary digest. Candidates sharing those fields could still differ in manifest metadata, executable argv, trust/certification verdicts, or sandbox evidence. The non-stable sort and first-valid selection then made the selected candidate and decision artifact depend on discovery input order.

## Decision

Every discovered candidate receives a `semantic-digest-v2` identity in domain `cxxlens.provider-candidate.v1`. Its canonical projection binds the complete manifest, ordered executable argv, authoritative-path verdict, trust verdict, certification verdict, canonical certified qualifications, canonical sandbox report, and validation error. Discovery source remains a separate decision field so the same physical candidate found by multiple sources retains precedence evidence without changing identity.

For one provider ID/version, distinct candidate digests are `security.provider-shadowing` and reject the entire set. An exact candidate duplicated in the same source is also rejected. An exact full identity found through different sources is allowed and normalized by source precedence with every occurrence retained in decision evidence.

The strict decision order is discovery-source precedence, provider ID, provider version, binary digest, then candidate digest. Every `provider_candidate_decision` records the candidate digest, and selection validation rebinds the selected decision to a recomputed full candidate digest.

## Consequences

- Manifest, argv, sandbox, certification, and validation differences cannot bypass shadowing defense.
- Exact and fallback selection are invariant under discovery permutations.
- Decision source plus candidate digest identifies every accepted discovery occurrence.
- Adding a candidate authority field requires a candidate identity domain/version change.
