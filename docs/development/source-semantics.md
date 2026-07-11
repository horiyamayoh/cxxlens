# Source semantics

Source coordinates are byte-based. `byte_offset` is zero-based; line and UTF-8 byte column are
one-based. A `file_range` is always half-open: `begin` is included and `end` is excluded. Empty token
and EOF ranges therefore have equal offsets and remain valid.

`file_id` stores only a normalized project-relative semantic key such as `file:src/main.cpp`. It never
stores a checkout root or display path. Absolute keys, empty/dot/dot-dot components, backslashes,
duplicate separators, and JSON-unsafe quotes are invalid. A UI may join the key with a checkout root
for display, but that projection must not feed identity or canonical JSON.

A valid span retains primary, spelling, and expansion ranges independently. Macro frames are ordered
from the outermost invocation to the innermost. Argument indexes are zero-based. Invalid and unknown
locations use distinct states and must never be converted to a fabricated valid offset zero.

Direct edit eligibility is conservative: only a validated, directly-spelled, non-macro, non-read-only
span with a versioned source digest is eligible. Macro argument/body/expansion, implicit, generated,
system, builtin, unknown, and invalid spans are not directly editable.
