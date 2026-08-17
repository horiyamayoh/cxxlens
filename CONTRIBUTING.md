# Contributing to cxxlens

## 開発経路

cxxlens の既定開発経路は **direct-to-main** です。通常の bounded implementation issue のために
feature branch や pull request を必須にしません。

1. `main` の current SHA と active work scope を確認する。
2. issue の exact contract、allowed paths、完了条件を固定する。
3. 変更を小さな atomic unit に限定し、影響 target/test/checker を実行する。
4. history rewrite を使わず、`main` に fast-forward commit を反映する。
5. 反映後の exact SHA に対する GitHub Actions を確認し、変更起因の失敗は follow-up commit で修正する。
6. commit SHA、bounded evidence、残余 gap、`Learning checkpoint` を issue に残して close する。

pull request は次の場合に任意で使用します。

- 外部 contributor からの変更
- security、重大な public semantics、不可逆な contract 変更など、独立反証 review が必要な変更
- 隔離しないと安全に並行できない競合リスクの高い作業
- repository owner が特定変更について明示的に PR review を要求した場合

PR を使う場合も、PR の存在や merge 操作だけを完成証拠にせず、最終 `main` SHA の evidence を使用してください。
force push、公開履歴の reset/rebase、secret/permission、課金、外部 production deploy は direct-to-main 方針に含まれません。

## Setup と focused validation

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

## Issue の完了条件

通常の issue は、次の bounded evidence が揃えば閉じられます。

- issue の exact scope と contract が実装され、scope 内に placeholder、silent fallback、既知 blocker が残っていない。
- 変更した振る舞いと直接 dependency closure の positive/negative test が成功する。
- 必要な format、public-header、schema、documentation、generated-inventory check が成功する。
- 公開 callable が [Doxygen 規約](docs/development/doxygen-style.md)を満たす。
- public header が LLVM/Clang 型を露出しない。
- absolute path、時刻、iteration order、診断文を stable identity に使用しない。
- scope 外に残る integration/native/install/platform/Nightly/release gap は、owner と依存順を持つ別 issue または
  tracked gap に記録する。
- issue evidence に `production qualification: not claimed`、または当該 issue が所有する限定的な qualification claim を記載する。
- `Learning checkpoint: none` または関連 DF ID を記載する。
- 最終反映 commit と、その commit を含む `main` SHA を記載する。

任意 PR を使用した場合は、上記に加えて review disposition を残します。通常の issue close に、製品全体の
static/shared/install/native matrix、`full`/`stress`、release evaluation、terminal production-scope closure、
無関係な gate の成功は要求しません。

## Production qualification の所有者

production qualification は個別 implementation issue ではなく、updated `main`、Nightly/release workflow、または
exact contract と label で明示された `integration-gate` / `readiness-gate` / qualification issue が所有します。

CI tier の責任は次のとおりです。

- local/pre-commit: issue-focused evidence と `fast`/focused checker
- optional PR: 独立 review または外部 contribution の追加 evidence
- `main` update: clean exact-SHA `full` と integration evaluation
- Nightly/release: `stress`、sanitizer、install/native、release evidence

cache や changed-file selection は final production correctness evidence ではありませんが、bounded implementation
completion の対象選択と反復高速化には使用できます。統合 gate の失敗だけを理由に閉じた implementation issue を再度開かず、
その failure が当該 issue の bounded acceptance を誤りと証明した場合だけ reopen します。

品質 evidence の owner と fail-closed fallback は `schemas/cxxlens_ng_quality_ownership.yaml` を参照してください。

## Optional PR automation

`.github/workflows/pr-integration.yml` は任意 PR の補助経路として残します。direct-to-main の通常作業では使用しません。

同一 repository の任意 PR branch を最新 `main` へ更新するには、PR conversation に `/update-branch` を単独行でコメントします。
Quality の exact-head run 成功後に自動 squash mergeしてよい任意 PR は、本文に次を単独行で含めます。

```text
automerge: squash
```

automation は draft、cross-repository head、SHA mismatch、branch protection/review failure を fail-closed で拒否します。
これは任意 PR の安全装置であり、通常開発を PR 前提へ戻す規則ではありません。
