# ADR 0100: NG1 resume provider identity digest grammar

- Status: Accepted
- Date: 2026-08-15
- Amends: ADR 0099, ADR 0016

## Context

The provider manifest carries the measured provider executable digest and the
selected provider semantic-contract digest as exact content digests in the
`sha256:<64 lowercase hex>` namespace. The NG1 resume binding had instead
typed both fields as semantic-v2 digests. A resume control populated from the
manifest was consequently rejected before its token projection could be
validated.

The two identity domains are intentionally different. A prefix alias,
silent rehash, or unconditional acceptance of both spellings would erase the
authority that selected and measured the provider.

## Decision

For `cxxlens.provider-control.resume.v2`:

| Field | Exact grammar and authority |
| --- | --- |
| `provider_binary_digest` | The exact selected manifest content digest, `sha256:<64 lowercase hex>`, from `cxxlens.provider-manifest.v1`. |
| `provider_semantic_contract_digest` | The exact selected manifest content digest, `sha256:<64 lowercase hex>`, from `cxxlens.provider-manifest.v1`. |
| Other binding digests and `staged_digest` | `semantic-v2:sha256:<64 lowercase hex>` under the existing field-specific authority. |
| `token_digest` | `semantic-v2:sha256:<64 lowercase hex>` over the canonical ordered tuple of every resume field except `token_digest`. |

The token projection includes the two manifest digest strings as exact
canonical text values. It does not convert, prefix, rehash, or dual-accept
either identity. A manifest content digest in a semantic-v2-only field, or a
semantic-v2 digest in either manifest identity field, is rejected.

This is an authority and source-private validation amendment only. NG1
remains `proposed`, unadvertised, and unavailable for live transport until its
implementation has direct positive, negative, fault, and determinism/resource
test coverage. The support surface is unchanged by this amendment.

## Consequences

The manifest-to-resume boundary is directly usable without inventing a
semantic projection, while replay identity remains domain-separated by the
semantic-v2 token digest. Provider manifests and runtime trust checks continue
to bind the measured binary and selected contract with their exact `sha256:`
content digests.

The source-private validator and deterministic control codec must keep the
field-specific grammar explicit. Positive and negative vectors must cover the
manifest-derived path and reject namespace substitution.

## Acceptance gate

DF-0243 is resolved by this accepted amendment and its mirrored hardening,
protocol, vector, checker, and focused direct-test coverage. This ADR does not
authorize live NG1 transport, capability advertisement, or a production-support
claim.
