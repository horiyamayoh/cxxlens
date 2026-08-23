# ADR 0005: 製品境界・release profile・compatibility を独立契約にする

- Status: Accepted
- Date: 2026-07-15

## Context

次世代設計は language-neutral relation kernel と C/C++ semantic provider を分離し、NG0–NG3 を
定義した。しかし、NG0 を「1.0 候補」としながら旧 milestone graph は R6/R7 を 1.0 判定の依存に
置いていた。また distribution、kernel semantics、relation descriptor、identity、snapshot、protocol、
provider、Query IR、recipe 等の独立 version axis を library version 一つで判定できない。

この曖昧さは、recipe 利用者だけでなく typed query、portable provider SDK、major-specific native
provider を開発する利用者にも、不必要な C++/compiler 依存、silent fallback、誤った support 表示を
生じさせる。

## Decision

製品 contract、`schemas/cxxlens_support_matrix.yaml`、Compatibility v2 schema を、製品境界、
stability tier、version compatibility の machine-readable authority とする。

- `relation-kernel` と `logical-query` は language-neutral であり、C/C++ semantic type、Clang/LLVM type、
  native provider implementation に依存しない。
- C/C++ standard relation と semantic service は `cc-cpp-semantics` に置く。
- portable provider SDK は detached protocol value のみを扱い、compiler-native object は
  major-specific provider process 内に閉じ込める。
- NG0 は pre-1.0 で候補化する最小 vertical slice、NG1 は distribution 1.0 に必要な production
  hardening とする。各 capability の直接試験が実装完了を判定する。
- R5–R7 と NG2/NG3 は 1.x へ追加可能な任意 capability であり、1.0 を block しない。future major は
  accepted stable version axis を破壊するときだけ要求する。
- installed public header を 1.0 source authority とし、C++ module は 1.0 stable installed surface に
  含めない。native SDK、provider executable、recipes は umbrella target から除外する。
- compatibility request は利用 context の全 required axis を明示し、exact release policy の下で比較する。
  implicit bundle selection、first-wins、major fallback を禁止する。
- 判定は `supported`、`migration-required`、`unsupported` と stable reason code を返す。unknown required
  feature、missing/duplicate axis、major/digest mismatch は structured failure にする。

「LLVM より便利」は人間被験者 KPI や数値 release gate として定義しない。ただし lower layer の利用者が
compiler framework の型、lifetime、build/link graph を背負わず、機械可読な契約と同じ validator を利用
できることを明確な設計便益とする。

## Consequences

- 現在の単一 `cxxlens::cxxlens` target は migration baseline のままであり、この ADR は target split を
  実装済みとは宣言しない。実装と installed consumer tests は対応する public target と直接試験が所有する。
- provider protocol の NG0/NG1 exact wire contract と各 version axis の意味契約は Provider Protocol schema と
  関連 ADR が固定する。support matrix はそれらの独立性と negotiation/rejection rule を公開する。
- 未掲載の環境は `doctor` で `compat.environment-unsupported` となる。
- offline migration が存在しても runtime compatible とは表示せず、`migration-required` とする。

## Verification

`tools/quality/check_ng_release_contract.py` が Compatibility v2、support 表、version axis/context、reason code
を検査する。同じ checker の `inspect` / `doctor` が request から schema-valid report を生成する。負例は
duplicate axis、missing axis、major mismatch、unknown required feature、未掲載環境、implicit fallback を検査する。
