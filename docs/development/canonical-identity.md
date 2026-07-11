# Canonical identity

Stable IDs are generated from a versioned, domain-separated, field-tagged binary encoding. Every
field includes its name, type, and byte length. Fields and map entries are lexically ordered; sets are
sorted and deduplicated; sequences preserve semantic order. Integers use fixed-width big-endian bytes.
The encoder binds its format, domain, schema version, and semantics version before any payload field.

The current digest contract is `fnv1a64x4` version 1: four independently domain-separated FNV-1a 64
lanes form a 64-hex full digest. It makes no cryptographic-security claim. A stable ID contains its
domain prefix and the full digest. `short_display()` is UI-only and must never be stored, compared, or
used for collision safety.

Identity inputs may contain semantic values and validated project-relative source keys. Absolute
paths, wall time, process/thread/pointer values, cache state, observation order, diagnostic message
text, ambient randomness, and unordered-container iteration are forbidden. Root/display information
belongs to operational presentation metadata.

The collision registry stores a canonical-payload fingerprint for every prefixed full ID.
Re-registering the same payload is idempotent; a distinct payload at the same key is a hard error.
Downstream packages define domain payload factories, but they must not create another encoder, digest
format, or collision policy.
