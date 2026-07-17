# ADR 0080: Provider columnar typed digests

- Status: accepted
- Date: 2026-07-18
- Issue: #149

## Context

Column chunk and batch digests projected metadata by joining raw text with
newlines and pipes. The CBOR schemas allow valid UTF-8 control characters, so a
delimiter could move between adjacent fields while preserving the joined byte
sequence. Batch collections also lacked explicit collection and element
framing.

## Decision

Chunk and batch digest inputs use a shared named-field encoder built from
`cxxlens-canonical-tuple-v1`:

- every field is a `(field-name, typed-value)` tuple;
- text remains an exact UTF-8 string, including LF, pipe, and NUL;
- unsigned integers are fixed-width eight-byte big-endian values;
- column summaries and ordered chunk digests are nested ordered tuples whose
  canonical encoding carries collection count, element length, and type tag;
- column order remains descriptor order and chunk digest order remains wire
  order.

The typed encoding is passed to semantic-digest-v2 under the new domains
`cxxlens.provider-column-chunk.v2` and
`cxxlens.provider-columnar-batch.v2`. Encode and decode both call the same
projection and independently bind the deterministic CBOR fields to the digest.

## Consequences

- Moving delimiters between metadata fields changes the chunk and batch digest.
- Moving bytes between collection elements changes the batch digest.
- Ordered collection permutations change identity; no new set semantics are
  introduced.
- The v1 delimiter digest domains are no longer emitted or accepted.
