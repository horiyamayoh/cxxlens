#!/usr/bin/env python3
"""Apply the one-time direct-to-main development-policy migration."""

from __future__ import annotations

import json
import pathlib
import re
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[2]


def replace_section(
    relative: str,
    pattern: str,
    replacement: str,
    *,
    flags: int = re.MULTILINE | re.DOTALL,
) -> None:
    path = ROOT / relative
    text = path.read_text(encoding="utf-8")
    updated, count = re.subn(pattern, replacement, text, flags=flags)
    if count != 1:
        raise RuntimeError(f"{relative}: expected one section replacement, got {count}")
    path.write_text(updated, encoding="utf-8")


def replace_exact(relative: str, old: str, new: str, *, expected: int = 1) -> None:
    path = ROOT / relative
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(
            f"{relative}: expected {expected} exact occurrence(s), got {count}: {old!r}"
        )
    path.write_text(text.replace(old, new), encoding="utf-8")


agents_section = """## Goal standing authorization

Repository policy `CXXLENS_AGENT_AUTHORIZATION_V1` を明示参照する goal は、この repository の active unit に限って
standing authorization を有効化します。

- `activation: explicit-goal-contract-reference`
- `non-activation: ordinary-request`
- `standing-scope: canonical-repository-active-unit`
- `platform-approval: never-bypass`
- `direct-main: issue-scoped-fast-forward-push-post-push-integration`

active unit 内の可逆な実装・生成・build/test、同一 issue の CI 根本修正、issue-scoped commit、最新 `main` への
fast-forward push、active issue の更新、push 後の exact-main CI 監視と修正は再承認不要です。通常の質問、診断、read-only
review、または policy を明示しない依頼から、この権限を暗黙に取得してはなりません。

PR は既定の搬送路ではありません。high-risk contract の独立反証 review、外部 contributor、またはユーザーが明示した場合に限る
任意の review mechanism とします。PR を使う場合も、active unit と bounded completion の責務境界は変えません。

`main` 更新前に最新の remote head を取得し、active unit の contract/path 非競合、対象差分、affected build/test を確認します。
`main` は fast-forward だけで更新し、force-push、history rewrite、未確認の unrelated change の同梱を禁止します。push 後は
その exact `main` SHA の CI を監視し、失敗時は根本修正 commit または revert を直ちに追加して issue に記録します。

destructive operation/history rewrite、branch protection・secret・permission の変更、課金、外部 production deploy、顧客・第三者への
連絡、解消不能なユーザー変更との競合、authority から決められない重大な public semantics は、対象と effect を開示した fresh user
approval を要求します。sandbox/system/host platform の approval gate はこの policy で迂回しません。"""
replace_section(
    "AGENTS.md",
    r"## Goal standing authorization\n.*?(?=\n## Required implementation rules)",
    agents_section,
)

goal_header = """# Agent-driven public API development goal

この文書は、Codex の `/goal` と複数のコーディングエージェントを使って、cxxlens を継続開発するための実行契約です。
次の短い goal からこの文書を参照します。

```text
/goal docs/development/agent-api-development-goal.md を実行契約として CXXLENS_AGENT_AUTHORIZATION_V1 を適用し、cxxlens を需要側・供給側の双方で閉じた証拠付き意味知識基盤へ育て、issue-scoped commit を fast-forward で main に直接 push し、exact main SHA の CI evidence まで継続してください。
```

## Autonomous execution and approval boundary

上の短い `/goal` のように、この文書と policy ID を明示参照した goal の実行中だけ standing authorization を有効にします。
単なる質問、診断、read-only review、またはこの実行契約を参照しない依頼から暗黙に有効化しません。ユーザーは実行中でも
authorization をいつでも revoke または narrow でき、その後の操作は狭められた範囲に従います。

| 区分 | 実行境界 |
| --- | --- |
| Standing authorization | read-only audit、active unit 内の編集・生成・test/build、同一 issue の CI 根本修正、issue-scoped commit、最新 `main` への fast-forward push、canonical repository 上の active issue 更新、push 後の exact-main check 監視・根本修正、bounded implementation evidence と learning checkpoint 後の active issue close は再承認不要。明示的な integration/readiness/qualification issue は自身の qualification evidence まで満たす |
| Notify and continue | 当初想定外の supporting test/file が必要でも、同一 contract・同一 issue 内で可逆なら、原因、追加 scope、検証方法を commentary で通知して継続します。これは approval gate ではありません |
| Fresh user approval | destructive operation/history rewrite、branch protection 変更、secret/permission 追加、課金、外部 production deploy、active issue workflow 外の顧客・第三者への連絡、ユーザー変更との解消不能な競合、authority で決められない重大な public semantics は停止します。対象、effect、不可逆性または rollback を開示し、exact target/effect に限定した承認を得ます |
| External blocker | 必須 reviewer、toolchain、service、permission を取得できない場合は、証拠と選択肢を示して停止します |
| Platform approval | sandbox、system、host platform が要求する権限確認は standing authorization で迂回しません |

checker が prose の偶然一致に依存せず境界を固定できるよう、次の binding marker を保持します。

- `activation: explicit-goal-contract-reference`
- `non-activation: ordinary-request`
- `standing-scope: canonical-repository-active-unit`
- `notify-and-continue: reversible-same-contract-issue`
- `fresh-approval: exact-target-effect-after-disclosure`
- `external-blocker: evidence-options-stop`
- `platform-approval: never-bypass`
- `skill-compatibility: prior-goal-authorization-satisfies-generic-approval`
- `direct-main: issue-scoped-fast-forward-push-post-push-integration`
- `pull-request: optional-for-risk-review-or-external-contribution`
- `fresh-approval-reuse: forbidden`
- `revocation: user-anytime`
- `completion-class: bounded-implementation`
- `production-qualification: not-claimed-by-default`
- `issue-close-owner: bounded-issue-or-explicit-qualification-gate`
- `aggregate-qualification-owner: exact-main-integration-readiness-release`
- `reopen-condition: bounded-acceptance-or-scope-regression-only`

skill が一般的な explicit approval を要求しても、操作が active policy の standing-authorization 範囲に明示されていれば、
goal 開始時の承認で満たされたものとします。skill の診断、focused plan、結果報告は実行しますが、その approval のためだけに
会話を停止しません。skill がより具体的な安全条件を持つ場合、または操作が列挙範囲外なら、その条件または fresh-approval gate を
維持します。

standing authorization は repository 内の active unit を越える mutable authority を与えません。canonical repository の active
issue における通常更新と、顧客・第三者への外部連絡を区別します。fresh approval は開示した exact target/effect にだけ有効で、
別 target、別 effect、後続操作へ categorical に流用しません。platform approval も別の capability gate であり、この contract は
迂回しません。PR は high-risk contract の独立反証 review、外部 contribution、または明示要求に使える任意の mechanism ですが、
通常の active unit を `main` に反映するための必須 gate ではありません。
"""
replace_section(
    "docs/development/agent-api-development-goal.md",
    r"\A# Agent-driven public API development goal\n.*?(?=\n## Mission)",
    goal_header,
)

completion_section = """## Issue completion and qualification boundary

通常の implementation issue の既定完了クラスは **bounded implementation completion** とします。
issue を閉じるために distribution 全体の production qualification を再実行・再証明してはなりません。

bounded implementation completion は、担当 issue の exact contract と明示 scope に対して次を要求します。

- 宣言した実装範囲が完成し、scope 内に placeholder、silent fallback、既知の correctness/security/invariant blocker が残っていない
- 変更した振る舞いと直接 dependency closure の positive/negative test、必要な determinism/resource/error evidence が成功する
- 変更した public contract、schema、catalog、Doxygen、example、生成 inventory のうち直接影響するものが整合する
- scope 外の native/platform/static/shared/install/consumer/Nightly/release evidence は、必要なら別 issue または tracked gap に
  owner、依存順、完了条件とともに残す
- commit と issue close evidence が、実装完了、support/stability、production qualification を混同せず、
  `production qualification: not claimed` または issue が所有する限定的な qualification claim を明示する
- Learning checkpoint を `none` または関連 DF ID として記録する

通常の issue には、全 static/shared matrix、installed consumer 全件、native toolchain/platform matrix、`full`/`stress`、
Nightly、release evaluation、terminal production-scope closure、無関係な issue/gate の完了を要求しません。それらは pushed `main`、
Nightly/release workflow、または exact contract と label で明示された `integration-gate` / `readiness-gate` /
qualification issue が所有します。

後続の統合 gate が失敗した場合は、まず統合 failure を owner issue に記録します。閉じた implementation issue を reopen するのは、
その failure が当該 issue の bounded acceptance を誤りと証明した場合、または当該 scope に regression がある場合だけです。
単に製品全体が未認定であることは reopen 理由にしません。

この境界は direct-to-main の fast-forward discipline、final aggregate qualification、fail-closed exact-SHA evidence を弱めません。
責任とタイミングを個別 implementation issue から exact-main integration/readiness/release gate へ移すだけです。"""
replace_section(
    "docs/development/agent-api-development-goal.md",
    r"## Issue completion and qualification boundary\n.*?(?=\n## Commit, push, issue closure)",
    completion_section,
)

commit_section = """## Commit, push, issue closure

1つの GitHub issue を1つの active write unit とし、issue、contract ID、repository-relative write path を宣言します。
同時 active unit は最大四つまで許可しますが、異なる unit の contract ID は disjoint、write path は同一・祖先・子孫関係を含めて
非重複でなければなりません。shared authority/contract、依存実装、同じ path prefix を所有する作業は直列化します。

各 unit は最新の `origin/main` から開始します。push 前に remote head を再取得し、対象差分だけを self-review して affected
build/test/quality command を実行します。1つの issue の差分を issue-scoped commit にまとめ、`main` へ fast-forward push します。
remote `main` が進んでいた場合は最新 head へ安全に載せ直して再検証し、force-push、history rewrite、未解決 conflict の押し込みを
行いません。

PR は high-risk contract の独立反証 review、外部 contributor、またはユーザーの明示要求に使える任意の review mechanism です。
通常 unit の completion や `main` 反映の必須条件にはしません。high-risk change に必要な independent review は、PR の有無ではなく
review evidence 自体で満たします。

通常の implementation issue は、commit が `main` に到達し、担当 scope の bounded implementation completion evidence、残余 gap の
明示 ownership、completion evidence、learning checkpoint が揃えば閉じられます。Foundation、Wave 0、G5、`release-evaluation`、normal/final
production-scope report は、その issue が明示的に所有しない限り integration/readiness/qualification gate の責務です。

それらの aggregate gate は exact main SHA の required checks と fail-closed evidence を引き続き検証します。
`release-evaluation: not-qualified` は評価器の fail-closed success だけを意味し、`gate.release`、GR、production support を満たしません。
その結果だけを理由に bounded implementation issue を reopen してはならず、bounded acceptance の誤りまたは scope regression がある場合に限ります。

全 tracked gap の解消後は `release-evaluation: qualified`、strict GR report、final-mode production-scope report を同じ exact
main SHA で確認します。複数 issue の無関係な変更を1 commit にまとめません。

issue には完了前に次の evidence をコメントします。

- commit SHA と変更した authority/contract/catalog/use-case ID
- production implementation または documentation-only boundary
- positive/negative/real-project tests
- result states、error/unresolved/coverage/guarantee/completion plan
- constructibility witness（該当する場合）
- 実行した build/test/quality command と CI run URL
- deferred 項目と follow-up issue
- learning checkpoint と `issue-ready` 結果
- 完了判定の根拠

通常の implementation issue は bounded implementation completion を満たした場合だけ `completed` として閉じます。明示的な
integration/readiness/qualification issue は、自身の exact contract に定めた aggregate qualification も満たした場合だけ閉じます。
未実装、未検証、未認定の作業を `not planned` で隠しません。"""
replace_section(
    "docs/development/agent-api-development-goal.md",
    r"## Commit, push, issue closure\n.*?(?=\n## CI monitoring and progress)",
    commit_section,
)

ci_section = """## CI monitoring and progress

各 unit の `main` push 後と全対象 issue の完了後に、push された exact main SHA の required CI を監視します。失敗した場合は job、
step、log、artifact を調査し、根本原因を修正した fast-forward commit または明示的 revert を追加します。新しい SHA の全 required
CI が緑になるまで継続し、過去 SHA の成功を最終 SHA の evidence として流用しません。これは repository
integration/release gate の証拠であり、通常 issue の bounded close 条件へ aggregate qualification を逆輸入するものではありません。

作業中は日本語で簡潔に、現在の use case/capability/issue、完了事項、根拠、検証、残作業、blocker、design feedback を報告します。
生の長大な log ではなく、結論と証拠を要約します。authority から決定できない重大な public semantics だけをユーザーへ確認し、
それ以外は安全で reversible な判断を行います。"""
replace_section(
    "docs/development/agent-api-development-goal.md",
    r"## CI monitoring and progress\n.*?(?=\n## Final completion criteria)",
    ci_section,
)

contributing_tail = """## Issue / change の完了条件

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
第三層は明示的な integration/readiness/qualification issue だけが close 条件として所有します。"""
replace_section(
    "CONTRIBUTING.md",
    r"## Issue / Pull request の完了条件\n.*\Z",
    contributing_tail + "\n",
)

adr = """# ADR 0094: `/goal` delegation と direct-to-main integration を risk-tiered に束縛する

- Status: Accepted
- Date: 2026-07-19
- Last amended: 2026-08-16
- Issue: #176
- Design feedback: DF-0177 / #177
- Depends on: ADR 0088, ADR 0089, ADR 0093
- Amended by: ADR 0095; workflow amendment #263; completion-policy amendment #291; direct-to-main amendment 2026-08-16

## Context

cxxlens は issue 単位の実装、検証、commit、push、CI 修復、issue 更新をコーディングエージェントへ委任する。
従来は全変更を branch/PR/merge に通し、個別 issue と全体 integration の証拠を同じ搬送路へ結合していた。この方式は
exact-head review には強い一方、bounded implementation completion を採用した現在の開発では待ち行列と重複確認を生み、
独立した小さな変更まで PR gate に滞留させる。

必要なのは品質証拠の削除ではなく、そのタイミングの変更である。局所品質は push 前に affected checks で確認し、全体品質は
push 後の exact `main` SHA で fail closed に評価する。履歴改変や競合の押し込みを許さず、失敗時に修正または revert するなら、
direct-to-main は bounded issue throughput と aggregate qualification を両立できる。

## Decision

repository 限定 policy `CXXLENS_AGENT_AUTHORIZATION_V1` を採用し続ける。この policy は
`docs/development/agent-api-development-goal.md` と policy ID を明示参照した goal の実行中だけ有効で、通常の質問や read-only
review から暗黙に起動しない。

通常の active unit は次の順で処理する。

1. issue、contract ID、repository-relative write path を宣言し、他 unit との contract/path conflict を排除する。
2. 最新の `origin/main` から作業し、affected positive/negative test、必要な quality check、self-review を実行する。
3. unrelated change を含めない issue-scoped commit を作る。
4. remote head を再確認し、fast-forward で `main` へ直接 push する。
5. push された exact main SHA の CI を監視し、失敗時は根本修正 commit または明示的 revert を追加する。
6. bounded implementation evidence、残余 gap ownership、learning checkpoint を issue に記録する。

force-push、history rewrite、未解決 conflict の押し込み、複数 issue の無関係な差分の同梱は禁止する。remote `main` が進んだ場合は
最新 head へ安全に載せ直し、affected checks を再実行する。

PR は high-risk public contract の independent counterexample review、外部 contributor、またはユーザーの明示要求に使える任意の
review mechanism とする。通常 unit の `main` 反映や close の必須 gate にはしない。high-risk change に必要な review は PR の有無ではなく、
review evidence と constructibility gate で判定する。

standing authorization は active unit 内の可逆な編集、build/test、CI 根本修正、issue-scoped commit、fast-forward main push、
active issue 更新、exact-main CI 監視、bounded close を含む。destructive operation/history rewrite、branch protection、
secret/permission、課金、外部 production deploy、第三者連絡、重大な未決 public semantics は fresh user approval を要求する。
platform approval は迂回しない。

binding marker は次とする。

- `activation: explicit-goal-contract-reference`
- `non-activation: ordinary-request`
- `standing-scope: canonical-repository-active-unit`
- `direct-main: issue-scoped-fast-forward-push-post-push-integration`
- `pull-request: optional-for-risk-review-or-external-contribution`
- `history-rewrite: prohibited`
- `platform-approval: never-bypass`

通常の implementation issue は bounded implementation completion を所有し、full/stress/Nightly/release/production-scope qualification は
exact main SHA の integration/readiness/release owner が一度だけ所有する。後続 failure で issue を reopen するのは、bounded acceptance
の誤りまたは当該 scope の regression が証明された場合だけとする。

## Consequences

- PR 待ちを通常変更のクリティカルパスから外し、独立 issue の throughput を上げられる。
- push 前の affected checks と push 後の exact-main CI の責務が分離される。
- main CI failure は隠さず、修正 commit または revert により履歴上で追跡できる。
- force-push と history rewrite は引き続き禁止され、fast-forward の線形履歴を維持する。
- PR と独立 review は必要な高リスク変更・外部 contribution で引き続き利用できる。
- production qualification の強度は維持しつつ、個別 issue への重複要求を避ける。

## Verification

readiness checker は policy ID の exact binding、direct-main marker、PR optional marker、fast-forward/post-push integration contract、
fresh approval、platform carve-out を fail closed に検証する。旧 PR-mandatory marker または direct-main prohibition の再導入を拒否する。

scenario test は、通常 unit の fast-forward main pushが許可されること、remote head 変化時に再同期・再検証すること、CI failure が
修正または revert に至ること、force-push・history rewrite・branch protection 変更が standing authorization に含まれないことを確認する。
"""
(ROOT / "docs/design/adr/0094-risk-tiered-goal-authorization.md").write_text(
    adr, encoding="utf-8"
)

wrapper_path = "tools/quality/check_ng_api_development_readiness.py"
wrapper_constants = """DIRECT_MAIN_AGENT_CONTRACT = pathlib.Path("AGENTS.md")
DIRECT_MAIN_GOAL_CONTRACT = pathlib.Path(
    "docs/development/agent-api-development-goal.md"
)
DIRECT_MAIN_DECISION_ADR = pathlib.Path(
    "docs/design/adr/0094-risk-tiered-goal-authorization.md"
)
DIRECT_MAIN_POLICY_ID = "CXXLENS_AGENT_AUTHORIZATION_V1"
DIRECT_MAIN_POLICY_TOKEN = re.compile(
    rf"(?<![A-Za-z0-9_]){re.escape(DIRECT_MAIN_POLICY_ID)}(?![A-Za-z0-9_])"
)
DIRECT_MAIN_COMMON_MARKERS = (
    "activation: explicit-goal-contract-reference",
    "non-activation: ordinary-request",
    "standing-scope: canonical-repository-active-unit",
    "platform-approval: never-bypass",
    "direct-main: issue-scoped-fast-forward-push-post-push-integration",
)
DIRECT_MAIN_GOAL_MARKERS = (
    *DIRECT_MAIN_COMMON_MARKERS,
    "notify-and-continue: reversible-same-contract-issue",
    "fresh-approval: exact-target-effect-after-disclosure",
    "external-blocker: evidence-options-stop",
    "skill-compatibility: prior-goal-authorization-satisfies-generic-approval",
    "pull-request: optional-for-risk-review-or-external-contribution",
    "fresh-approval-reuse: forbidden",
    "revocation: user-anytime",
)
LEGACY_PROTECTED_MAIN_PATTERNS = (
    re.compile(
        r"protected-main:\\s*"
        r"unit-branch-pr-exact-head-review-merge-exact-merged-main"
    ),
    re.compile(r"direct-main:\\s*prohibited"),
)
"""
replace_exact(
    wrapper_path,
    'ISSUE_ID = "#261"\n',
    'ISSUE_ID = "#261"\n' + wrapper_constants,
)

replace_exact(
    wrapper_path,
    '    "aggregate-qualification-owner: exact-merged-main-integration-readiness-release",',
    '    "aggregate-qualification-owner: exact-main-integration-readiness-release",',
)
replace_exact(
    wrapper_path,
    '    "exact merged-main SHA の required checks と fail-closed evidence",',
    '    "exact main SHA の required checks と fail-closed evidence",',
)

direct_validator = r"""
def validate_direct_main_authorization_contract(root: pathlib.Path) -> None:
    decision = root / DIRECT_MAIN_DECISION_ADR
    if not decision.is_file():
        _fail(
            "agent authorization decision ADR is missing: "
            f"{DIRECT_MAIN_DECISION_ADR}"
        )
    if "- Status: Accepted" not in decision.read_text(encoding="utf-8"):
        _fail("agent authorization decision ADR is not accepted")

    documents = {
        DIRECT_MAIN_AGENT_CONTRACT: DIRECT_MAIN_COMMON_MARKERS,
        DIRECT_MAIN_GOAL_CONTRACT: DIRECT_MAIN_GOAL_MARKERS,
    }
    for relative, markers in documents.items():
        path = root / relative
        if not path.is_file():
            _fail(f"agent authorization contract is missing: {relative}")
        text = path.read_text(encoding="utf-8")
        if len(DIRECT_MAIN_POLICY_TOKEN.findall(text)) != 1:
            _fail(
                "agent authorization policy ID must appear exactly once in "
                f"{relative}"
            )
        for marker in markers:
            if text.count(f"`{marker}`") != 1:
                _fail(
                    "agent authorization marker is missing or duplicated in "
                    f"{relative}: {marker}"
                )
        if any(pattern.search(text) for pattern in LEGACY_PROTECTED_MAIN_PATTERNS):
            _fail(f"legacy protected-main workflow is forbidden in {relative}")

    goal = (root / DIRECT_MAIN_GOAL_CONTRACT).read_text(encoding="utf-8")
    goal_example = re.compile(
        rf"(?m)^/goal\s+{re.escape(DIRECT_MAIN_GOAL_CONTRACT.as_posix())}"
        rf".*(?<![A-Za-z0-9_]){re.escape(DIRECT_MAIN_POLICY_ID)}"
        rf"(?![A-Za-z0-9_])"
    )
    if goal_example.search(goal) is None:
        _fail("short goal example does not bind the authorization policy ID")

"""
replace_exact(
    wrapper_path,
    "\ndef validate_bounded_completion_contract(root: pathlib.Path) -> None:\n",
    direct_validator
    + "def validate_bounded_completion_contract(root: pathlib.Path) -> None:\n",
)

replace_exact(
    wrapper_path,
    "_baseline.validate_workflow = validate_workflow\n",
    "_baseline.validate_agent_authorization_contract = "
    "validate_direct_main_authorization_contract\n"
    "_baseline.validate_workflow = validate_workflow\n",
)

test_path = "tests/quality/test_ng_api_development_readiness.py"
override_tests = r"""
    def test_authorization_protected_main_workflow_is_required(self) -> None:
        "The composed contract requires the direct-main workflow marker."
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / readiness.DIRECT_MAIN_GOAL_CONTRACT
            goal.write_text(
                goal.read_text(encoding="utf-8").replace(
                    "`direct-main: issue-scoped-fast-forward-push-post-push-integration`",
                    "direct-main marker removed",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(readiness.ReadinessError, "direct-main"):
                readiness.validate_documents(root)

    def test_authorization_direct_main_prohibition_is_required(self) -> None:
        "The composed contract requires PRs to remain optional."
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / readiness.DIRECT_MAIN_GOAL_CONTRACT
            goal.write_text(
                goal.read_text(encoding="utf-8").replace(
                    "`pull-request: optional-for-risk-review-or-external-contribution`",
                    "pull-request marker removed",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(readiness.ReadinessError, "pull-request"):
                readiness.validate_documents(root)

    def test_legacy_direct_main_workflow_is_rejected(self) -> None:
        "The composed contract rejects restoration of the old PR-only marker."
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            goal = root / readiness.DIRECT_MAIN_GOAL_CONTRACT
            goal.write_text(
                goal.read_text(encoding="utf-8")
                + "\n`protected-main: "
                "unit-branch-pr-exact-head-review-merge-exact-merged-main`\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(readiness.ReadinessError, "protected-main"):
                readiness.validate_documents(root)

"""
replace_exact(
    test_path,
    "    def complete_evidence(\n",
    override_tests + "    def complete_evidence(\n",
)
replace_exact(
    test_path,
    "それらの aggregate gate は exact merged-main SHA の required checks と fail-closed evidence を引き続き検証します。",
    "それらの aggregate gate は exact main SHA の required checks と fail-closed evidence を引き続き検証します。",
)

replace_exact(
    "tools/quality/check_ci_supply_chain.py",
    """WORKFLOWS = (
    pathlib.Path(".github/workflows/quality.yml"),
    pathlib.Path(".github/workflows/nightly.yml"),
    pathlib.Path(".github/workflows/pr-integration.yml"),
)
""",
    """WORKFLOWS = (
    pathlib.Path(".github/workflows/quality.yml"),
    pathlib.Path(".github/workflows/nightly.yml"),
)
""",
)

for relative in (
    ".github/workflows/pr-integration.yml",
    ".github/workflows/apply-direct-main-policy.yml",
    "tools/quality/_apply_direct_main_policy.py",
):
    path = ROOT / relative
    if path.exists():
        path.unlink()

subprocess.run(
    [
        "python",
        str(ROOT / "tools/quality/check_documentation_consistency.py"),
        "generate",
        "--root",
        str(ROOT),
    ],
    check=True,
)

ledger = json.loads(
    (ROOT / "schemas/cxxlens_asset_migration_ledger.json").read_text(
        encoding="utf-8"
    )
)
paths = {row["path"] for row in ledger["assets"]}
if ".github/workflows/pr-integration.yml" in paths:
    raise RuntimeError("deleted PR integration workflow remains in asset ledger")
if ledger["asset_count"] != len(ledger["assets"]):
    raise RuntimeError("asset ledger count differs")

agents = (ROOT / "AGENTS.md").read_text(encoding="utf-8")
goal = (ROOT / "docs/development/agent-api-development-goal.md").read_text(
    encoding="utf-8"
)
for text, label in ((agents, "AGENTS.md"), (goal, "goal")):
    if "direct-main: issue-scoped-fast-forward-push-post-push-integration" not in text:
        raise RuntimeError(f"{label}: direct-main marker missing")
    if "direct-main: prohibited" in text or "protected-main:" in text:
        raise RuntimeError(f"{label}: legacy protected-main policy remains")

print("direct-to-main policy migration applied")
