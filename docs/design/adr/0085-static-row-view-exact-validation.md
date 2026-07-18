# ADR 0085: static row view は typed read 前に exact row を再検証する

- Status: Accepted
- Date: 2026-07-18
- Issue: #154

## Context

public `static_row_view<Relation>::get<Column>()` は row の descriptor ID と requested column IDだけを確認し、cellをそのまま
返していた。`detached_row` はpublic valueであり手組み可能なので、expected integerにUTF-8 cell、invalid digest/closed symbol、
state/value不整合、同一semantic majorの別descriptor shapeをtyped successとして取り出せた。

constructorは単なるborrowでありvalidated tokenを要求しないため、呼出側が事前に `validate_row()` 済みというpreconditionは
型でもAPIでも強制されていない。

## Decision

`static_row_view::get()` は毎回、次の順序でfail-closed validationを行う。

1. row descriptor ID と `Relation::descriptor().id` の一致
2. `Column::ref()` のdescriptor ID、column existence、exact `value_type` の一致
3. `validate_row(Relation::descriptor(), row)` による全required/unknown column、全cell type/state/scalar validation
4. requested cell read。optional columnが存在しない場合だけexact optional typeのabsent cellを返す

これによりstatic façadeはdynamic row builder/store/provider decodeと同じ `detached_cell::validate()` authorityを使用する。
rowにdescriptor digestを追加せずとも、current exact descriptor shapeに対するrequired/type/unknown-column validationで別minor shapeを
fail closedにする。将来、validated-row tokenを導入する場合もこのobservable validation contractを弱めない。

## Consequences

- wrong-type、invalid digest/closed symbol/UTF-8/stateはdeterministic errorを返す。
- foreign/unknown Column tagとunknown row columnを拒否する。
- optional missingだけはtyped absentとして読み出せる。
- validated rowではraw dynamic cellとstatic readのcanonical valueが一致する。
- readごとのrow validation costを受け入れ、安全なfactory/token APIは将来のadditive optimizationとする。

## Verification

`unit.sdk.static-row-view-validation` はwrong integer type、invalid UTF-8/digest/closed symbol、different descriptor shape、foreign Column、
optional absence、validated dynamic/static parityを検証する。SDK quality checkerはpublic headerがColumn bindingとcomplete
`validate_row()` を実行し、Issue #154 catalog entryとtest evidenceを保持することを固定する。
