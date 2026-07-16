# ADR 0012: Typed/dynamic query と provider 作者向け SDK surface

- Status: Accepted
- Date: 2026-07-16
- Owner: Issue #66
- Depends on: ADR 0003, ADR 0005, ADR 0007, ADR 0010, ADR 0011

## Context

relation と Logical Query IR と provider protocol の契約だけでは、利用者は descriptor の検証、typed tag の生成、
wire framing、credit、checksum、coverage、native object の detachment を個別に実装しなければならない。これは recipe
利用者だけでなく、dynamic query、portable provider、Clang major-specific native provider を開発する利用者にも
LLVM/Clang の直接利用に近い負担を残す。

## Decision

公開 author SDK を二つの installable package に分離する。

- `cxxlensProviderSDK` / `cxxlens::provider_sdk` は標準ライブラリだけに依存し、relation descriptor、generated tag、
  dynamic relation、detached row/snapshot、Logical Query IR、portable provider protocol helper、conformance harness、
  recipe lowering を提供する。通常 header と link interface に LLVM/Clang を露出しない。
- `cxxlensClang22ProviderSDK` / `cxxlens::clang22_provider_sdk` は明示的な native opt-in surface とし、
  `<cxxlens/provider/clang22.hpp>` だけが Clang 型を受け取る。borrowed translation unit と AST object は callback
  scope を越えて保存、所有、thread 間移送できず、公開結果は detached value に正規化する。

generated typed query と runtime dynamic query は同じ `relation_descriptor`、`column_ref`、`query::builder`、
`logical_query_ir` を用いる。その canonical JSON は ADR 0007 の `cxxlens.logical-query-ir.v1` に一致し、SDK 独自の
同名 IR を作らない。relation/provider の追加は central enum、switch、source registry の変更を要求しない。
IDL compiler は accepted relation registry から tag と column reference を生成する。

portable provider 作者は `relation_sink`、coverage/unresolved/evidence builder、`run_worker` を使用し、104-byte frame、
SHA-256、sequence、credit を直接操作しない。production codec と同じ経路を使う in-memory harness が checksum、truncate、
cancel、no-credit fault を注入する。scaffold は source、manifest、test、CMake を一組で生成する。
`provider::manifest::canonical_json()` と scaffold の manifest はともに
`cxxlens.provider-manifest.v1` の exact schema に適合する。

`sdk::result<T>` は toolchain の `std::expected` 実装有無に公開 ABI を左右されない value-or-structured-error 型とする。
例外や diagnostic prose を制御契約に使わず、`error.code` を stable machine-readable discriminator とする。

## Consequences

- recipe 利用者から native provider 作者まで、同じ descriptor/IR/partiality model を段階的に利用できる。
- ordinary SDK consumer は LLVM/Clang package を発見せずに configure、link、run できる。
- native SDK の Clang major は package/header 名に現れ、別 major への silent fallback は起きない。
- product 全体の component target 分割と end-to-end provider adoption は Issue #67 以降の migration であり、本 ADR は
  author SDK の独立 package を先に固定する。

## Verification

`tools/quality/check_ng_sdk_contract.py` は exact catalog、5 author path、IDL 生成、compile-fail 例、通常 SDK の
LLVM-free boundary、非中央集権的な extension surface を検証する。さらに C++ が出力した Logical Query IR と
provider manifest を既存の accepted schema/reference validator へ入力する。C++ unit/public-header/install tests は typed/dynamic
IR parity、row-view expiry/thread affinity、frame checksum/credit、coverage accounting、native pointer rejection を検証する。
