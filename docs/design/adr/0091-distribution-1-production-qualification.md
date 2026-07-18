# ADR 0091: distribution 1.0 の production support を exact GR report tuple に限定する

- Status: Accepted
- Date: 2026-07-18
- Issue: #167

## Context

G0–G5 と R0–R4 が完了しても、それぞれの contract が別 revision、別 toolchain、別 install prefix で通っただけでは
配布物を production-supported と宣言できない。source-tree の provider support matrix に binary digest `pending`、platform
の広い名前、または wildcard を書いて production authority にすると、実際に検証していない rebuild や環境へ support claim
が拡張される。さらに G5 performance、security conformance、Doxygen、static/shared relocation、real project consumer、
license/notice のいずれかが欠けた package を release と呼べてしまう。

## Decision

`schemas/cxxlens_ng_release_qualification.yaml` を GR の machine-readable authority とする。`release-qualification` CI job は
clean `main` の同一 revision/tree に対して、G5 report、static/shared の relocated install manifest と JUnit、runtime JUnit、
security conformance report、quality/Doxygen log を収集し、独立 validator で再検証する。全 prerequisite が一致したときだけ
`cxxlens.ng-release-qualification-report.v1` を生成する。

source-tree の provider support matrix entry は conformance template であり production grant ではない。production support の
唯一の authority は GR report 内の exact tuple とする。tuple は provider ID/version、installed worker の byte digest、relation、
interpretation、toolchain identity、platform と static/shared configuration を含み、さらに capability、guarantee、security profile
digest、evidence digest を保持する。`pending`、wildcard、report に存在しない relation/provider/platform/rebuild は unsupported である。

Clang 22 reference provider は、actual source を parse する normalizer/worker test、process failure/security negative test、
`cc.entity@1`、`cc.call_site@1`、`cc.call_direct_target@1`（registry descriptor は対応する `.v1`）の canonical interpretation、memory/SQLite を使う独立 multi-TU
installed consumer が同じ SHA で通った場合に限り、report に現れた static/shared worker bytes を production-supported とする。

distribution package version は 1.0.0、shared SONAME は 1 とし、Apache-2.0 `LICENSE` と `NOTICE` を installed prefix に含める。
G5/R0–R4 を省略した report、stale revision/tree、manifest digest/file digest の差し替え、unqualified surface の広告は validator が
fail closed に拒否する。

## Consequences

- 同じ source からの rebuild でも worker byte digest または toolchain が異なれば別 tuple であり、再 qualification が必要になる。
- support matrix の静的 entry を production-supported に変更しても release claim にはならず、checker が拒否する。
- distribution 1.0 の production-supported は「GR report に列挙された exact tuple」に限定され、1.x 全体や未列挙 provider を含まない。
- release evidence は 365 日保持し、report から package、security、performance、documentation の根拠を追跡できる。
