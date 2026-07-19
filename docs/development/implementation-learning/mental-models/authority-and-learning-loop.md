# Authority and learning loop

> Status: Non-normative explanatory model.

## Normative anchors

- [Next-generation integrated design §28](../../../design/cxxlens_next_generation_integrated_design_ja.md#28-governance-and-versioning)
- [ADR 0093](../../../design/adr/0093-implementation-learning-design-feedback.md)
- [Agent-driven API development goal](../../agent-api-development-goal.md)

## Scope

このモデルは、実装中の発見が既存設計と一致しない場合の考え方を説明します。個別 relation、query、provider の意味契約を
定義するものではありません。

## Model

normative authority は「絶対に変更してはいけない予言」ではなく、「現時点で全実装が共有する、反証可能な最良の契約」です。
したがって agent の責務は二つあります。

- 置換されていない契約を守り、独自解釈で drift を作らない。
- evidence が契約を反証したら、その evidence を捨てず、実装より先に共有契約を修正する。

design feedback record はこの二つの責務の間にある staging area です。raw observation を current truth と混同せずに保存し、
investigation、反証 review、disposition を経て authority 更新へ接続します。accepted record は決定そのものではなく、決定へ至った
reasoning と evidence の索引です。

## Known tensions

- record を増やしすぎると重要な signal が埋もれるため、material な発見だけを記録します。
- blocker を軽く扱うと悪い契約を固定しますが、全改善案で実装を止めると delivery が止まります。現在の契約が健全か、変更が可逆かで分けます。
- agent autonomy は速度を上げますが、high-risk change の confirmation bias を強め得ます。そのため著者と異なる independent reviewer を要求します。
- curated mental model は理解を助けますが、更新が遅れる可能性があります。衝突時は常に normative anchor を優先します。
