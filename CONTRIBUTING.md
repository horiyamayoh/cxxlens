# Contributing to cxxlens

## 開発経路

通常の経路は direct-to-main です。feature branch や pull request は必須ではありません。
変更を小さく保ち、履歴 rewrite をせず、影響する試験を通してから main に atomic commit を
反映します。独立 review は任意で、試験を代替しません。

## Setup と validation

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang --target <affected-targets>
ctest --preset dev-clang -R '<affected-tests>' --output-on-failure
ctest --preset dev-clang --output-on-failure
cmake --build --preset dev-clang --target cxxlens-quality
```

変更固有の positive・negative・fault・determinism/resource/error test と、main に登録された
全決定的 CTest が開発完了条件です。`cxxlens-quality` は契約/security/docs を直接 assert
し、終了コードだけを返します。

`.github/workflows/quality.yml` は Clang 22 static/shared、全決定的 CTest、static/shared
installed consumer、GCC public headers、contract/security/docs を実行します。
`.github/workflows/release.yml` は manual または `v*` tag で ASan/UBSan、TSan、static
analysis、stress/repeat、scale、real-project、relocated-install を実行し、全て成功した
場合だけ package を作成・公開します。

試験用 report、receipt、checksum、toolchain provenance、artifact upload/download、issue
checkpoint は生成・保存しません。GitHub Actions の job log と Git 履歴は通常機能として残します。

## 公開 API と schema

統合設計、relation/API/provider catalog、contract の semantics と invariant を確認します。
canonical identity、value type、schema、validator、test、service の順に変更し、positive・
negative・fault test と Doxygen/catalog を直接影響範囲で更新します。claim/provenance、
coverage、unknown、materialization report、SQLite/source-closure receipt、provider trust は
製品機能なので運用証跡廃止の対象にしません。

Compatibility request/report は v2、support table は
`{release_version, surface, os, architecture, compiler_provider_major, linkage}` です。
未掲載環境と Windows/MSVC は `unsupported` です。v1 の qualification/evidence field は使いません。

## Issue の完了

main workflow が green になったら issue を close します。追加コメント、exact-SHA 記録、review
receipt、work-unit、qualification JSON、Learning checkpoint は要求しません。scope 外の未対応は
製品 issue として別途扱い、運用証跡のための記録は作りません。

## 禁止事項

- name/pretty type string だけの semantic identity、compile command の silent fallback、macro range edit
- conflict/stale digest/variant/reparse failure の無視
- actionable な unknown reason、unsupported surface、consumer gap の omission
- test に合わせた上位 contract の縮小、診断文 substring 制御、shell command の文字列連結
- 運用証跡 namespace、artifact upload、qualification report、issue checkpoint の再導入
