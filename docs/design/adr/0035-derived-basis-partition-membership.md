# ADR 0035: derived basis は exact input snapshot の partition membership を検証する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: store-kernel
- Decision issue: #92
- Depends on: ADR 0009, ADR 0033

## Context

derived claim の basis は input snapshot ID と consumed partition content digest 集合を identity に含めるが、writer は同じ
snapshot ID の committed publication が存在することしか確認しなかった。well-formed な任意 digest や別 snapshot の partition を
指定しても publication でき、provenance DAG に実在しない edge を作れた。

## Decision

snapshot writer の独立 validation は各 derived claim について次を atomic に検証する。

- input snapshot ID を committed、non-corrupt、strict-prior physical generation の publication へ解決する。
- resolved immutable manifest の partition content digest 集合を authority とする。
- canonical sorted/unique/nonempty の consumed digest 全件が、その exact manifest 集合に含まれることを要求する。
- 一件でも不存在または別 snapshot 所属なら candidate 全体を拒否する。
- cross-series input は許可するが exact snapshot membership を省略しない。
- 同じ semantic snapshot ID の複数 physical publication は collision validation により同じ manifest を持つため、同じ membership
  結果を返す。

`make_derived_claim()` / `validate_claim()` は store を所有しない value-level API なので basis の形と identity だけを検証し、
publication validator が existence/membership authority を担う。

## Consequences

- derived provenance edge は実在する prior snapshot partition にだけ向く。
- arbitrary digest や別 snapshot partition による dependency set の分岐を publication 前に拒否する。
- incremental invalidation と監査は verified manifest membership を信頼できる。
- corrupt/uncommitted/rejected publication は input authority にならない。

## Verification

`tests/unit/sdk/store_test.cpp` は exact prior membership の成功、well-formed nonexistent、別 snapshot、複数中一部 mismatch、
uncommitted/corrupt input の拒否、同一 semantic snapshot の複数 physical publicationでの同一 membership を検証する。
