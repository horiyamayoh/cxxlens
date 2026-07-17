# ADR 0061: static/shared installed package は同一 matrix で relocation qualification する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: release-packaging
- Decision issue: #118
- Depends on: ADR 0005, ADR 0012, ADR 0015, ADR 0056

## Context

`CXXLENS_BUILD_SHARED=ON` は公開されていたが CI と install test は default static build だけを検査していた。shared installed tool は
prefix 内の cxxlens library へ依存する一方で relocatable runtime search path を持たず、非 system prefix から追加環境変数なしに
起動できなかった。consumer の build-tree RPATH と artifact の存在確認では、この配布 failure を検出できない。

## Decision

`install-consumer` と通常の build/test job を `CXXLENS_BUILD_SHARED=OFF/ON` の matrix とする。install test は全 installed tool を clean
environment で直接実行し、prefix 移設後に再実行する。core、portable provider SDK、Clang 22 provider SDK、example consumer は build
tree と installed executable の双方を同じ環境で実行する。CI artifact は別 configure で再生成せず、検査済みの relocated prefix
そのものから作る。

shared library は project version と distribution-major SONAME を持つ。Linux の installed library は `$ORIGIN`、tool は
`$ORIGIN/../<install-libdir>`、macOS は対応する `@loader_path` を使用し、link directory 由来の build-tree path を install RPATH へ
取り込まない。Linux qualification は dynamic section の RUNPATH/SONAME と build-tree path 不在を検査する。required SONAME link を
一時的に除去する negative test は、外部 library-path override なしで起動が失敗し、loader diagnostic が欠落 SONAME を含むことを
要求する。

## Verification

static/shared の双方で package install、全 tool の original/relocated prefix 実行、全 installed consumer の configure/build/install/run
を行う。shared Linux では `readelf` による RUNPATH/SONAME、build-tree path の不在、欠落 dependency diagnostic を検査する。
release bundle schema/checker は matrix、artifact provenance、runtime policy と実装/CI marker の省略を fail closed にする。
