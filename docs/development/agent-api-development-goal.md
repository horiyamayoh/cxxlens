# Agent-driven public API development goal

この文書は、Codex の `/goal` が cxxlens を継続開発するときの実行契約です。

```text
/goal docs/development/agent-api-development-goal.md を実行契約として CXXLENS_AGENT_AUTHORIZATION_V1 を適用し、cxxlens の製品機能を試験で完成させ、main が green になるまで継続してください。
```

## Authorization boundary

この文書と policy ID を明示参照した goal の実行中だけ standing authorization を有効にする。通常の質問、診断、read-only
review から暗黙に有効化しない。ユーザーはいつでも revoke または narrow できる。

| 区分 | 実行境界 |
| --- | --- |
| Standing authorization | repository 内の可逆な編集・生成・test/build、同一 issue の CI 修正、atomic commit、main の fast-forward update、active issue の close |
| Fresh approval | destructive/history rewrite、branch protection/ruleset、secret/permission、課金、外部 production deploy、active workflow 外の第三者連絡、解消不能な競合、authority で決められない重大な public semantics |
| Platform approval | sandbox/system/host platform の権限確認は standing authorization で迂回しない |

binding marker:

- `activation: explicit-goal-contract-reference`
- `non-activation: ordinary-request`
- `standing-scope: canonical-repository-active-unit`
- `platform-approval: never-bypass`
- `completion-class: test-only-main-green`
- `release-qualification: release-workflow-tests-only`

## Mission

cxxlens は、コンパイラの観測結果を再現可能・問い合わせ可能な意味知識へ変換し、
分からない場合に不足理由まで返す基盤である。結果は次を区別する。

```text
proved | disproved | unknown | partial | conflicting
```

coverage、closure、unresolved、conflict、differential disagreement、guarantee、
provenance、runtime receipt を損失なく保持する。`unknown` は missing input/capability/model/
evidence と依存順の completion plan を返し、empty success や omission へ畳まない。

## Development policy

通常の delivery は direct-to-main であり、履歴 rewrite はしない。新しい public surface、
relation、provider、analysis、model、recipe は、consumer/use case、capability gap、結果の
modality、coverage/provenance の保持、exact contract ID、write scope、support disposition
を定義してから実装する。high-risk public semantics、identity、protocol、persistence、
resource bound は仕様/ADR と positive・negative・fault test で反証可能にする。独立 review は
任意で、試験を代替しない。

## Completion contract

実装完了の必須条件は次だけである。

1. 変更固有の positive・negative・fault・determinism/resource/error test が成功する。
2. `main` workflow が static/shared の全決定的 CTest、contract/security/docs、installed
   consumer、GCC public header を成功させる。

issue は main が green になったら、追加コメント、exact-SHA 複製、review receipt、work-unit、
checksum、qualification JSON、集約 report、Learning checkpoint なしで close する。製品の
runtime provenance、coverage、unknown、materialization report、SQLite/source-closure receipt、
provider trust/certification はこの廃止対象ではない。

## CI and release contract

`.github/workflows/quality.yml` は Clang 22 static/shared を build し、無選択の全決定的 CTest、
contract/security/docs checks、static/shared install consumer、GCC public headers を実行する。
path selection、fast report、JUnit/timing JSON、toolchain provenance、artifact upload/download、
結果集約 job は作らない。

`.github/workflows/release.yml` は manual または `v*` tag で main 全件、ASan/UBSan、TSan、
static analysis、stress/repeat、最大 scale、real-project、relocated-install を実行する。
一件でも失敗すれば package job は実行しない。試験 artifact は保存しない。

Compatibility request/report は v2 とし、`os`、`architecture`、`toolchain`、`linkage` を
照合する。v1 の `runtime_qualified`、`evidence_refs`、`qualification_state`、
`compat.release-not-qualified` は存在しない。対応環境は
`schemas/cxxlens_support_matrix.yaml` の単純な version/surface/environment 表で宣言し、
未掲載環境と Windows/MSVC は unsupported とする。

## Execution loop

1. `AGENTS.md` と統合設計書の対象章、contract、現行 issue/CI を読む。
2. 競合しない最小 scope と affected tests/checkers を決める。
3. 実装、product schema/API、docs、positive/negative/fault tests を完成させる。
4. focused test/build と必要な `cxxlens-quality` を実行する。
5. current main の状態を再確認し、atomic commit を作る。
6. main workflow の green を待ち、失敗は通常 commit で修正する。
7. green 後に issue を close する。運用証跡を生成しない。

共有 schema、checksum、workflow、CMake の同時編集は直列化する。未追跡の利用者資料、Git
履歴、GitHub Actions の通常 job log はユーザーの明示指示なしに削除しない。

## Required implementation rules

- C++23、public namespace/type/function は lower snake case。
- public header に LLVM/Clang 型を露出しない。filesystem/process/time/hash は port 越し。
- AST pointer を保存・所有・移送しない。unordered iteration を serialization/ID に使わない。
- empty と unresolved を区別し、claim/provenance、coverage、guarantee、unknown reason を落とさない。
- mutation/generation は plan、独立 validator、dry-run、transaction の順。
- public API/relation/provider 変更時は catalog/registry、Doxygen、直接対応する試験を整合させる。

## Forbidden shortcuts

- name/pretty type string だけの semantic identity、compile command の silent fallback、macro range edit
- conflict/stale digest/variant/reparse failure の無視
- unsupported surface/consumer gap/actionable unknown reason の omission
- test に合わせた上位 contract の縮小、diagnostic prose substring 制御、shell command 文字列連結
- 運用証跡 namespace、artifact upload、qualification report、issue checkpoint の再導入

## Commands

```sh
CXX=clang++ cmake --preset dev-clang
cmake --build --preset dev-clang --target <affected-targets>
ctest --preset dev-clang --output-on-failure
cmake --build --preset dev-clang --target cxxlens-quality
```

`cxxlens-quality` は product contract を直接 assert して終了コードだけを返す。試験結果を
repository の report/receipt/checksum として複製しない。
