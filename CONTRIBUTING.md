# Contributing to cxxlens

## 開発手順

初回 setup と通常の configure は次を使います。

```sh
python3 -m pip install --requirement tools/quality/requirements.txt
npm install --global markdownlint-cli2@0.18.1
CXX=clang++ cmake --preset dev-clang
```

通常の implementation issue は **bounded implementation completion** を既定とし、issue に宣言した対象だけを
build/test します。

```sh
cmake --build --preset dev-clang --target <affected-targets>
ctest --preset dev-clang -R '<affected-tests>' --output-on-failure
python3 tools/quality/run_gate.py fast --preset dev-clang \
  --report build/dev-clang/fast-report.json
```

public contract、schema、documentation、生成 inventory を変更した場合は、影響する validator/checker を追加して実行します。
`run_gate.py check|full|stress`、`cxxlens-quality` 全体、static/shared install matrix、native/platform matrix、Nightly、
release evaluation は、その surface を issue が明示的に所有する場合だけ issue の完了条件に含めます。

変更は out-of-source build で検証し、生成物を source tree に追加しないでください。

## 公開 API と schema の変更

1. 次世代統合設計、要求 ID、relation/API/provider catalog ID または担当移行 issue を確認する。
2. field semantics と invariant を定義する。
3. canonical identity、C++ value type、serializer/schema、validator の順で変更する。
4. positive、negative、determinism、root relocation のテストを追加する。
5. Doxygen 契約、次世代 catalog/registry、設計書、release note のうち変更範囲に直接影響するものを同時に更新する。
6. tracked asset を追加・削除した場合は `python3
   tools/quality/check_documentation_consistency.py generate --root .` で terminal-state ledger を再生成する。

次世代 contract の正式な入口は [catalog/registry index](docs/design/catalogs/README.md) です。各 catalog の
bootstrap entry は exact API や relation の完成を意味しません。担当 issue で semantics、schema、validator、
test、service の順に具体化してください。

公開 API の意味、LLVM 対応方針、schema compatibility、決定性、安全 gate を変更する場合は
ADR と設計レビューが必要です。単純な実装詳細や既存契約内のバグ修正には ADR は不要です。

## Issue / Pull request の完了条件

通常の issue とその PR は、次の bounded evidence が揃えば閉じられます。

- issue の exact scope と contract が実装され、scope 内に placeholder、silent fallback、既知 blocker が残っていない。
- 変更した振る舞いと直接 dependency closure の positive/negative test が成功する。
- 必要な format、public-header、schema、documentation、generated-inventory check が成功する。
- 公開 callable が [Doxygen 規約](docs/development/doxygen-style.md)を満たす。
- public header が LLVM/Clang 型を露出しない。
- absolute path、時刻、iteration order、診断文を stable identity に使用しない。
- scope 外に残る integration/native/install/platform/Nightly/release gap は、owner と依存順を持つ別 issue または
  tracked gap に記録する。
- PR に `production qualification: not claimed`、または当該 issue が所有する限定的な qualification claim を記載する。
- `Learning checkpoint: none` または関連 DF ID を記載する。

通常の issue close に、製品全体の static/shared/install/native matrix、`full`/`stress`、exact merged-main
release evaluation、terminal production-scope closure、無関係な gate の成功は要求しません。

## Production qualification の所有者

production qualification は個別 implementation issue ではなく、merged `main`、Nightly/release workflow、または
exact contract と label で明示された `integration-gate` / `readiness-gate` / qualification issue が所有します。

CI tier の責任は次のとおりです。

- PR: issue-focused evidence と `fast`/`check` の repository guard
- merged `main`: clean exact-SHA `full`
- Nightly/release: `stress`、sanitizer、install/native、release evidence

cache や changed-file selection は final production correctness evidence ではありませんが、bounded implementation
completion の対象選択と反復高速化には使用できます。統合 gate の失敗だけを理由に閉じた implementation issue を再度開かず、
その failure が当該 issue の bounded acceptance を誤りと証明した場合だけ reopen します。

品質 evidence の owner と fail-closed fallback は `schemas/cxxlens_ng_quality_ownership.yaml` を参照してください。
