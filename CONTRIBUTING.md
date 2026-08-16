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

## Issue / change の完了条件

通常の implementation issue は、担当 scope の **bounded implementation completion** を満たした時点で完了できます。
製品全体の production qualification を個別 issue の close 条件へ重複して持ち込みません。

最低限、次を issue/commit evidence に残してください。

- issue の exact contract と write scope
- 実装した差分と直接 dependency closure
- affected positive/negative test と必要な quality check
- 直接影響する contract/schema/catalog/documentation の更新
- scope 外に残る gap の owner、依存順、完了条件
- `production qualification: not claimed` または当該 issue が所有する限定的な claim
- `Learning checkpoint: none` または関連 DF ID

## Direct-to-main workflow

通常の変更は PR を経由せず、次の順で `main` に直接反映します。

1. 最新の `origin/main` を取得し、active unit の contract/path が他作業と競合しないことを確認する。
2. issue scope の変更だけを実装し、affected build/test/quality check と self-review を行う。
3. unrelated change を含めない issue-scoped commit を作成する。
4. remote head が変わっていないことを確認して、fast-forward で `main` へ push する。
5. push された exact main SHA の CI を監視し、失敗時は根本修正 commit または revert を直ちに追加する。
6. bounded completion evidence と CI run URL を issue に記録して close 判定を行う。

force-push、history rewrite、未解決 conflict の押し込みは禁止です。remote `main` が進んだ場合は最新 head へ安全に載せ直し、
affected checks を再実行してから push します。

PR は high-risk contract の独立反証 review、外部 contributor、または明示要求に使える任意の mechanism です。通常変更の必須 gate
ではありません。PR を使う場合も、その成否を issue の production qualification claim と混同しません。

## Production qualification の所有者

CI の責務は次の三層に分けます。

- **pre-push / affected checks**: 担当 issue の変更範囲と直接 dependency closure を確認する。
- **pushed `main` / exact-SHA integration**: push 後の正確な `main` SHA に対して full integration evidence を作る。
- **Nightly / release / production-scope**: stress、release qualification、production support claim を所有する。

通常 issue は第一層を必須とし、第二層の失敗は integration owner に記録しつつ、当該 scope の regression なら修正します。
第三層は明示的な integration/readiness/qualification issue だけが close 条件として所有します。
