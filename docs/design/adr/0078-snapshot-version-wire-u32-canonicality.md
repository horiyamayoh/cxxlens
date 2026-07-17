# ADR 0078: Snapshot version wire u32 canonicality

- Status: accepted
- Date: 2026-07-18
- Issue: #147

## Context

Snapshot payloads encode semantic-version components as unsigned 64-bit wire
integers, while the public value type stores unsigned 32-bit components. The
decoder narrowed without checking the range. An attacker could therefore encode
`2^32 + n`, bind the manifest to the truncated `n`, recompute the physical
checksum, and have a noncanonical payload accepted.

## Decision

The binary reader provides a typed unsigned-32 operation. It reads the complete
wire integer, rejects values above `UINT32_MAX` as `store.corrupt`, and narrows
only after that check. Snapshot version major, minor, and patch all use this
operation.

An accepted current-format v5 payload must also equal the byte sequence obtained
by canonical re-encoding its decoded value. Older declared readable formats
remain governed by their migration/compatibility rules and are not compared to
the v5 encoder.

## Consequences

- `UINT32_MAX` remains a valid value for every version component.
- `UINT32_MAX + 1` and larger values cannot wrap into a valid runtime version.
- A checksum and manifest identity recomputed around an oversized encoding do
  not bypass the typed-domain check.
- Every accepted v5 payload has one byte-stable decode/encode representation.
