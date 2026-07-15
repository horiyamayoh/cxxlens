# ADR 0005: 製品境界・release profile・compatibility bundle を独立契約にする

- Status: Accepted
- Date: 2026-07-15
- Decision owner: architecture-release
- Decision issue: #59
- Tracking issue: #56

## Context

次世代設計は language-neutral relation kernel と C/C++ semantic provider を分離し、NG0–NG3 を
定義した。しかし、NG0 を「1.0 候補」としながら旧 milestone graph は R6/R7 を 1.0 review の依存に
置いていた。また distribution、kernel semantics、relation descriptor、identity、snapshot、protocol、
provider、Query IR、recipe 等の独立 version axis を library version 一つで判定できない。

この曖昧さは、recipe 利用者だけでなく typed query、portable provider SDK、major-specific native
provider を開発する利用者にも、不必要な C++/compiler 依存、silent fallback、誤った support 表示を
生じさせる。

## Decision

`schemas/cxxlens_ng_release_bundle.yaml` を製品境界、profile-to-release mapping、public target DAG、
stability tier、version compatibility の machine-readable authority とする。

- `relation-kernel` と `logical-query` は language-neutral であり、C/C++ semantic type、Clang/LLVM type、
  native provider implementation に依存しない。
- C/C++ standard relation と semantic service は `cc-cpp-semantics` に置く。
- portable provider SDK は detached protocol value のみを扱い、compiler-native object は
  major-specific provider process 内に閉じ込める。
- NG0 は pre-1.0 で候補化する最小 vertical slice、NG1 は distribution 1.0 に必須の production
  hardening とする。1.0 blocker は R0–R4 と G0–G5/GR である。
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
  実装済みとは宣言しない。実装と installed consumer qualification は #67 と #71 が所有する。
- provider protocol の NG0/NG1 exact wire contract は #64、各 version axis の意味契約は owner issue が
  固定する。release bundle はそれらの独立性と negotiation/rejection rule を先に固定する。
- production evidence がない bundle は `doctor` で `compat.release-not-qualified` となる。
- offline migration が存在しても runtime compatible とは表示せず、`migration-required` とする。

## Verification

`tools/quality/check_ng_release_contract.py` が schema、component/target DAG、profile/release graph、acceptance
gate edge、version axis/context、reason code を検査する。同じ checker の `inspect` / `doctor` が request
から schema-valid report を生成する。負例は C/C++ dependency の kernel 逆流、duplicate axis、missing
axis、major mismatch、unknown required feature、unqualified runtime、implicit fallback を検査する。
