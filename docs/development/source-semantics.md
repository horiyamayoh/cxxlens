# Source snapshot and span semantics

source content は mutable filesystem path ではなく、content digest と encoding を持つ immutable
`source_file_snapshot` です。authoritative coordinate は snapshot と file に bind された half-open byte range
`[begin,end)` で、line/column は projection です。

invalid、unknown、stale span を offset zero の valid range に置き換えません。source snapshot mismatch は
structured stale state です。path identity は logical path domain と normalized path contract に基づき、host
mount や absolute checkout root は evidence/operational metadata に分離します。

spelling、macro argument/body/expansion、instantiation、generation、inline/lowering/import の由来は一つの
「元位置」へ潰さず、many-to-many origin DAG として保持します。cycle は batch rejection です。

直接編集は snapshot digest、origin、read-only policy を検証した spelled range だけを候補にできます。
mutation は NG3 の plan、独立 validator、dry-run、journaled transaction を経るまで apply しません。
