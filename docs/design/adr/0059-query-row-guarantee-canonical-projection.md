# ADR 0059: query row canonical projection は contributor guarantee を lossless に含める

- Status: Accepted
- Date: 2026-07-17
- Decision owner: query-runtime
- Decision issue: #116
- Depends on: ADR 0008, ADR 0014, ADR 0037

## Context

public `annotated_row` は各 occurrence に寄与した `claim_guarantee` の canonical set を保持していたが、
`annotated_row::canonical_form()` はこの field を出力しなかった。result 全体の `summary_guarantee` は conservative meet であり、
どの row にどの guarantee が帰属したかを復元できない。したがって row への割当だけが異なる結果、join / semi-join / distinct
で union した guarantee、canonical row ordering の tie-break が canonical artifact から脱落していた。

## Decision

query execution result v1 の全 row は nonempty `contributor_guarantees` を required field として持つ。各 guarantee は
`approximation`、`scope`、`assumptions`、sorted unique `verification_modalities` を structured JSON object で保持する。
row canonicalizer は guarantee object の canonical JSON を key に sort/deduplicate し、入力 insertion order に依存しない
`contributor_guarantees` array を出力する。

cursor の owned row copy と canonical export は同じ field set を観測可能にする。`summary_guarantee` は result-wide conservative
summary のまま独立に保持し、row attribution の代用にしない。unordered result の canonical sort と distinct key は完全な row
canonical form を使うため、row guarantee の差も identity/tie-break に含まれる。

## Verification

同じ row の guarantee だけを変えると row canonical form が変わり、insertion order だけを反転しても同一になることを固定する。
二 row 間で exact / under-approximation を swap した canonical row collection は異なり、scan、inner join、semi join、distinct 後の
guarantee set が structured canonical form に残ることを検証する。result schema は field 欠落を拒否し、memory / SQLite の
canonical semantic rows は byte-identical で比較する。
