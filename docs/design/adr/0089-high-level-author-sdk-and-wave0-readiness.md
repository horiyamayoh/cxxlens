# ADR 0089: provider SDK target は高水準 author SDK として固定する

- Status: Accepted
- Date: 2026-07-18

## Context

`cxxlens::provider_sdk` は名前に反して portable provider helper だけの target ではない。公開
`<cxxlens/sdk.hpp>`、Public API Catalog、installed consumer は relation、snapshot、Logical Query、provider、testing、recipe を一つの
author surface として使用しており、実 CMake も `cxxlens::cxxlens` と `cxxlens::recipes` を public link dependency に持つ。一方、architecture と
旧 package 定義は base/kernel だけに依存する最小 SDK と記述していた。この不一致は installed transitive dependency と今後の API unit の配置を
曖昧にする。

## Decision

`cxxlens::provider_sdk` / `cxxlensProviderSDK` は高水準 author SDK とする。公開 umbrella は relation、claim、store、query、provider、testing、recipe
を含み、CMake の direct public dependency は `cxxlens::cxxlens` と `cxxlens::recipes` である。compiler-native type は引き続き禁止し、Clang 22
surface は `cxxlens::clang22_provider_sdk` にだけ置く。

package metadata の `direct_dependencies` は transitive closure ではなく実 CMake の `PUBLIC` / `INTERFACE` edge を表す。直接の contract
check と installed consumer test は、package catalog、umbrella header、installed consumer の一致を検証し、intentional mutation を fail closed にする。

public header inventory の admission authority は Public API Catalog とする。Relation Registry は catalog に列挙された generated relation header が
accepted relation から導出可能であることを検証する。移行時の禁止リストは superseded asset に限定し、新 API header の手書き allowlist
を持たない。

## Consequences

- portable provider-only の最小 target は現時点では別 public API として作らない。
- author SDK の高水準依存を hidden transitive dependency として扱わない。
- 新 relation header は registry/catalog 更新だけで inventory に入り、移行時の禁止リスト編集を要求しない。
- API unit の active write owner と required stage は machine-readable contract と直接の CI 試験で管理する。
