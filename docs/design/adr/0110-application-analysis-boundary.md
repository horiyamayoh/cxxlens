# ADR 0110: GCC/MSVC-built application analysis boundary

Status: Accepted

## Context

cxxlens の host/package support authority は
`schemas/cxxlens_support_matrix.yaml` であり、Windows/MSVC と GCC host build は
unsupported のままである。一方、compiler-neutral capture、materialization、Store、query
境界が成立したため、GCC または MSVC で build される application を capture し、別の
frontend で replay した結果を Linux の既存 Store/query path へ投入できる。

この二つの support axis を混同すると、clang replay を production compiler exact と表示したり、
Windows worker の存在を Windows core/SDK support と誤認したりする。capture bundle や detached
provider result を自己申告 authority として採用することも、provider trust、coverage、unknown、
publication の invariant を破る。

## Decision

解析対象 toolchain の support authority を
`cxxlens.application-analysis-support.v1` として host/package support から分離する。
contract は production compiler、capture adapter、analysis frontend、target ABI、relation、
interpretation、guarantee floor を独立 axis として表す。

Phase 3 の固定 toolchain は次とする。

- GCC 16.2.0
- Visual Studio Enterprise 2026 18.9.12112.369 (build 12112.369)、MSVC toolset 14.51.36231 / compiler 19.51.36256
- Windows SDK package 10.1.26100.8249 (kit 10.0.26100.0)
- LLVM/clang-cl 23.1.0
- Windows runner `windows-2025-vs2026` image `20260824.214.3`。job は上記 exact toolchain を検査し、image の
  ambient default や `latest` alias を authority にしない

`cxxlens::sdk` に CH-1 Experimental application-analysis surface を追加する。外部 bundle は
bounded decoder/factory だけが immutable value へ変換し、aggregate の自己申告値を execution、
trust、Store publication authority として受け取らない。

GCC は compile database または shell-free wrapper から capture し、Clang 23.1.0 GCC-mode replay
を generic provider/materialization path へ接続する。MSVC は Windows の最小 capture/clang-cl
worker だけを build し、snapshot publication は Linux の既存 authority が再検証後に行う。

replay fidelity は `exact`、`semantics_preserving`、`approximation`、`unsupported`、
`nonsemantic` の閉じた集合とする。approximation/unsupported、欠落、redaction は scope 付き
unresolved と completion action を保持し、exact/complete へ昇格しない。

Phase 3 中の implementation state は `planned`、`capture-ready`、`replay-ready`、
`materialization-ready` の順でのみ進む。contract に tuple があるだけでは supported を意味せず、
`materialization-ready` と direct end-to-end test の両方を満たすまでは availability を
`unavailable` とする。

GCC native provider は adopted corpus の strict consumer gap が public GCC plugin API でのみ
解消できる場合に限り追加する。MSVC exact provider 調査は 10 engineer-days を上限とし、private
compiler ABI、不安定な PDB 内部表現、detached ownership 不成立、または有意な fidelity 改善なし
なら No-Go とする。

## Consequences

- cxxlens core、SDK、Store、query、Linux provider runtime 全体の GCC build や Windows/MSVC port
  はこの決定に含まれない。
- existing relation ID、Provider Protocol v2、wire/persistence semantics、provider trust、claim
  provenance、coverage、closure、unknown、conflict、guarantee は変更しない。
- application-analysis support contract は product capability contract であり、release
  qualification report や checkpoint ではない。
- 新しい relation、NG2、CFG/absence semantics はこの surface に便乗して追加しない。
- Stable 昇格は Phase 3 の実装完了とは別の release decision とする。
