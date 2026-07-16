# ADR 0014: Logical Query IR は annotation-aware deterministic reference runtime で実行する

- Status: Accepted
- Date: 2026-07-16
- Decision owner: query-runtime
- Decision issue: #69
- Tracking issue: #56

## Context

ADR 0007 は Logical Query IR v1 の annotated multiset algebra と executable oracle を固定したが、production C++
SDK は IR builder までしか持たず、published snapshot の cursor は detached row だけを返していた。row だけでは
condition、interpretation、claim contributor、provenance、guarantee を join/distinct/result へ伝播できない。
また SQLite の scan/page order や cancellation の観測時点をそのまま公開すると、同じ IR と snapshot から
backend ごとに異なる partial result が生成される。

## Decision

`schemas/cxxlens_ng_query_runtime_contract.yaml` を query runtime v1 の実装 authority とする。

- snapshot handle は row cursor に加え、query に必要な claim annotation の thread-affine cursor を提供する。
- SQLite payload v2 は annotation projection を保存する。旧 payload v1 は row read を維持できるが、annotation を
  推測せず query execution を structured failure にする。
- Logical IR の canonical JSON arguments は wire/schema authority として維持し、public typed decoder が exact key、
  type、operator correspondence を fail closed で検証する。
- planner は `cxxlens.reference-query-planner.v1` の deterministic internal plan とする。backend、index、join
  algorithm、spill、page order は physical explanation にだけ現れ、Logical IR digest へ入れない。
- 11 個の NG0 operator は ADR 0007 の multiplicity、condition、interpretation、evidence 規則をそのまま実行する。
- runtime budget と cancellation probe は IR digest から除外する。scan/intermediate exhaustion は未封印行を返さず、
  output cap は total-order または canonical semantic prefix を返す。
- cancellation は `before-execution` と logically complete output の `before-publish-row` だけで観測する。これにより
  backend schedule に依存しない sealed prefix と status を返す。
- result は execution success と input completeness/closure を別軸にし、coverage、closure ID、unresolved、conflict、
  conservative guarantee、logical/physical explanation を保持する。

## Consequences

- memory と SQLite は同じ executor を使用するが、各 snapshot backend の annotation cursor を実際に読むため、
  persistence/reopen を含む backend parity を検証できる。
- SQLite を SQL の NULL や scan order の意味へ使用しない。将来 pushdown/index を追加しても reference runtime と
  canonical semantic result が qualification oracle になる。
- physical planner は public semantic extension point ではない。lower-level 開発者は typed decoded arguments、
  annotation cursor、logical/physical explanation を利用できる。
- 同一 semantic snapshot ID に別 publication の改善された evidence/guarantee が付くことを許し、それらを snapshot
  identity へ誤って混入させない。

## Verification

unit acceptance は static/dynamic digest、11 operator、optional/unknown、condition intersection/union、join/semi-join
evidence、unordered canonicalization、ordered limit、budget、cancellation、cursor lifetime、success/completeness/closure
分離を検証する。同一 claim set を forward/reverse で memory/SQLite に publish し、canonical result と side channel を
比較する。installed static/shared consumer も両 backend 上で reference engine を実行する。
