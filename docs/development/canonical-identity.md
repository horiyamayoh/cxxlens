# Canonical identity

次世代 claim identity は一つの汎用 ID に潰さず、次の三段階へ分離します。

```text
semantic_key_id = H(relation name, semantic major, authoritative key tuple)
assertion_id = H(semantic key, universe, condition, interpretation, producer semantics)
content_digest = H(assertion, authoritative payload tuple)
```

hash input は versioned、domain-separated、length-prefixed binary tuple です。field order と type tag は
schema が定義し、integer、optional、set、map key、open symbol を canonical encoding します。JSON text、
locale formatting、unordered iteration、display path/prose は identity authority にしません。

absolute checkout root、pointer/address、timestamp、PID/thread ID、task/provider arrival order、cache state は
operational metadata です。path は `project://`、`build://`、`toolchain://`、`sysroot://`、`generated://`、
`provider://`、`external://` の logical domain と contract version に bind します。

現在の `fnv1a64x4` encoder と collision registry は移行 seed であり、次世代 identity contract の
algorithm/version は #63 で確定します。既存 digest を理由に新 relation key を旧形式へ固定してはいけません。
