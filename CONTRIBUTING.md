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

1. ユースケース、要求 ID、API catalog ID を確認する。
2. field semantics と invariant を定義する。
3. canonical identity、C++ value type、serializer/schema、validator の順で変更する。
4. positive、negative、determinism、root relocation のテストを追加する。
5. Doxygen 契約、API catalog、設計書、release note を同時に更新する。

公開 API の意味、LLVM 対応方針、schema compatibility、決定性、安全 gate を変更する場合は
ADR と設計レビューが必要です。単純な実装詳細や既存契約内のバグ修正には ADR は不要です。

## Pull request の完了条件

- format、静的解析、unit/public-header/install テストが成功する。
- 公開 callable が [Doxygen 規約](docs/development/doxygen-style.md)を満たす。
- public header が LLVM/Clang 型を露出しない。
- absolute path、時刻、iteration order、診断文を stable identity に使用しない。
- 新しい機能に positive/negative test と利用例がある。
