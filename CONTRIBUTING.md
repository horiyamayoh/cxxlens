# Contributing to cxxlens

## 開発手順

```sh
python3 -m pip install --requirement tools/quality/requirements.txt
npm install --global markdownlint-cli2@0.18.1
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang
ctest --preset dev-clang
cmake --build --preset dev-clang --target cxxlens-quality
```

変更は out-of-source build で検証し、生成物を source tree に追加しないでください。

## 公開 API と schema の変更

1. 次世代統合設計、要求 ID、relation/API/provider catalog ID または担当移行 issue を確認する。
2. field semantics と invariant を定義する。
3. canonical identity、C++ value type、serializer/schema、validator の順で変更する。
4. positive、negative、determinism、root relocation のテストを追加する。
5. Doxygen 契約、次世代 catalog/registry、設計書、release note を同時に更新する。
6. 既存資産を継承・移行・置換・archive・削除する場合は asset migration policy を更新し、ledger を
   generator から再生成する。新規資産は先に Git index へ追加し、`python3
   tools/quality/check_documentation_consistency.py generate --root .` を実行する。CI setup が作る
   untracked helper は repository asset として扱わない。

旧 `schemas/cxxlens_public_api_contract.yaml` は移行 baselineです。新しい API ID、signature、packageを
追加せず、旧atomic unit runnerを実装dispatchに使用しないでください。

次世代 contract の正式な入口は [catalog/registry index](docs/design/catalogs/README.md) です。各 catalog の
bootstrap entry は exact API や relation の完成を意味しません。担当 issue で semantics、schema、validator、
test、service の順に具体化してください。

公開 API の意味、LLVM 対応方針、schema compatibility、決定性、安全 gate を変更する場合は
ADR と設計レビューが必要です。単純な実装詳細や既存契約内のバグ修正には ADR は不要です。

## Pull request の完了条件

- format、静的解析、unit/public-header/install テストが成功する。
- 公開 callable が [Doxygen 規約](docs/development/doxygen-style.md)を満たす。
- public header が LLVM/Clang 型を露出しない。
- absolute path、時刻、iteration order、診断文を stable identity に使用しない。
- 新しい機能に positive/negative test と利用例がある。
- `cxxlens-documentation-consistency-check` が通り、active 文書から archive を規範参照していない。
