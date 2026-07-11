# Canonical JSON and schema evolution

All semantic JSON is built as typed values and encoded by the common canonical writer. Objects are
sorted by UTF-8 key bytes; duplicate keys, invalid UTF-8, non-finite numbers, and excessive nesting
return structured errors. Arrays retain domain canonical order. Strings use deterministic JSON
escaping, integers use locale-independent decimal encoding, finite floating-point values use
round-trippable `to_chars` encoding, and negative zero is normalized to zero.

Every M0 document embeds `schema`, `semantics_version`, and `library_version`. These axes are
independent: consumers resolve the exact schema ID/version through `schema_registry` and do not infer
schema compatibility from the library version. Timestamp, elapsed time, PID/thread identity,
absolute roots, cache state, and similar operational metadata are not members of semantic envelopes
or digests.

Within a schema major version, adding a declared optional field is compatible. Removing an optional
or required field, adding a required field, or changing field type or meaning requires a major version
and cache rebuild. Unknown required IDs/versions fail instead of falling back. Persistent facts and
caches are rebuildable derivatives, so unsafe migration is forbidden.

Package serializers add typed projection rows and a schema entry; they do not concatenate JSON,
reimplement escaping, store a separate summary, or serialize unordered iteration. Update the
compatibility manifest, schema checksum, positive/negative vectors, process/root/locale golden, and
catalog traceability with every schema change.
