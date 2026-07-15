# Evidence, execution coverage, closure, and unresolved

evidence graph は source observation、compile context、provider execution、canonicalization、assumption、
derivation、verification、exclusion、closure proof を結ぶ DAG です。summary prose は表示専用で、control flow
や semantic identity に使用しません。

provider/materialization は requested condition fragment を次の terminal state へ exactly once で会計します。

```text
requested = covered + excluded + not_applicable + failed + unresolved
          + unsupported + stale + truncated
```

coverage complete は execution accounting の完了であり、closed world を意味しません。absence、anti-join、
difference、unreachable を確定するには relation/key domain/condition/interpretation/assumption/snapshot に bind
された closure certificate が必要です。certificate がなければ positive rows は返せますが absence は unknown
で、`closure_missing` unresolved を保持します。

query result は rows、execution status、input coverage、closures、unresolved、conflicts、summary guarantee を
一体として返します。empty rows、failed operation、successful partial result、complete closed resultを相互に
代用してはいけません。
