# ADR 0021: query closure は到達可能な input range ごとの exact certificate で判定する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: query-runtime / snapshot-store
- Decision issue: #78
- Depends on: ADR 0007, ADR 0009, ADR 0013, ADR 0014

## Context

reference query runtime は `inputs_complete` のとき snapshot manifest に closure ID が一つでもあれば `closed` と
判定していた。manifest は certificate ID だけを保持していたため、その certificate が query の relation、partition、
condition、interpretation、content、coverage、assumption、kind に対応するかを実行時に確認できなかった。この判定では
unrelated partition の certificate が別 relation の query や join の未証明側を closed-world にしてしまう。

## Decision

snapshot physical payload v4 は、各 partition ID を再計算できる exact identity binding と、独立 validator を通過した
closure certificate の完全な subject を保存する。decode 時に partition ID と certificate ID を再計算し、manifest の
partition/closure ID 集合と完全一致しなければ `store.corrupt` とする。semantic snapshot identity は従来どおり
partition projection と closure ID を authority とし、physical projection は snapshot ID を変更しない。

query runtime は Logical Query IR の root から scan へ逆向きに condition/interpretation restriction を伝播する。
各到達可能な scan path について次をすべて満たす場合だけ closure proof が成立する。

- relation と interpretation が一致し、query condition と交差する全 partition が exact certificate を持つ。
- certificate は partition ID、content、coverage、condition、interpretation、assumption set、producer semantics、
  `relation-key-enumeration` kind に exact bind する。
- 明示 condition query の全 alternative が certificate 付き partition condition の和集合で覆われる。
- join/semi-join/union を含む複数 input では全 scan path が個別に証明される。

partition condition 全体の exact certificate は、その condition の明示 subset query を証明できる。relation、condition、
interpretation が無関係な certificate は数えない。`query_result::closure_ids()` は snapshot 全体ではなく、この判定に
実際に適用した certificate の sorted unique ID だけを返す。

payload v1–v3 は full subject を持たない。ID から subject を推測せず、query annotation が利用可能でも `closed=false`
とする。SQLite physical format は 2.3.0、write payload は v4 とする。

## Consequences

- execution success、input coverage completeness、semantic closure は引き続き独立した軸である。
- certificate ID の存在数だけでは absence/closed-world を主張できない。
- memory publication と SQLite reopen は同じ partition/certificate projection と applicability law を使用する。
- 将来の closure kind は relation/operator 固有の validator と applicability rule を追加するまで generic query closure に
  利用しない。

## Verification

`tests/unit/sdk/query_runtime_test.cpp` は (1) A/B complete で A certificate のみの B query、(2) exact B
certificate、(3) condition/interpretation mismatch、(4) join 片側 certificate 欠落、(5) exact superset certificate
による明示 subset query を固定する。exact case は SQLite publish/reopen 後にも full subject が復元され、snapshot に
複数 certificate があっても result には B に適用した ID だけが現れることを検証する。
