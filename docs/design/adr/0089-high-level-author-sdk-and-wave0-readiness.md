# ADR 0089: provider SDK target は高水準 author SDK として固定する

- Status: Accepted
- Date: 2026-07-18
- Issue: #168

## Context

`cxxlens::provider_sdk` は名前に反して portable provider helper だけの target ではない。公開
`<cxxlens/sdk.hpp>`、Public API Catalog、installed consumer は relation、snapshot、Logical Query、provider、testing、recipe を一つの
author surface として使用しており、実 CMake も `cxxlens::cxxlens` と `cxxlens::recipes` を public link dependency に持つ。一方、architecture と
release bundle は base/kernel だけに依存する最小 SDK と記述していた。この不一致は installed transitive dependency と今後の API unit の配置を
曖昧にする。

## Decision

`cxxlens::provider_sdk` / `cxxlensProviderSDK` は高水準 author SDK とする。公開 umbrella は relation、claim、store、query、provider、testing、recipe
を含み、CMake の direct public dependency は `cxxlens::cxxlens` と `cxxlens::recipes` である。compiler-native type は引き続き禁止し、Clang 22
surface は `cxxlens::clang22_provider_sdk` にだけ置く。

release bundle の `direct_dependencies` は transitive closure ではなく実 CMake の `PUBLIC` / `INTERFACE` edge を表す。Wave 0 readiness checker は
release bundle、CMake、package catalog、umbrella header、installed consumer の一致を検証し、intentional mutation を fail closed にする。

public header inventory の admission authority は Public API Catalog とする。Relation Registry は catalog に列挙された generated relation header が
accepted relation から導出可能であることを検証する。migration checker は superseded asset の denylist に限定し、新 API header の手書き allowlist
を持たない。

## Consequences

- portable provider-only の最小 target は現時点では別 public API として作らない。
- author SDK の高水準依存を hidden transitive dependency として扱わない。
- 新 relation header は registry/catalog 更新だけで inventory に入り、migration checker の編集を要求しない。
- API unit の active write owner、required stage、main baseline は machine-readable readiness contract と CI artifact で管理する。
