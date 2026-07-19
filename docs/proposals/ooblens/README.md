# ooblens design proposal

この directory は、Issue #188 で検討する `ooblens` の非規範設計提案です。

`ooblens` は局所的な unchecked-subscript lint ではありません。宣言された bounds precondition が、
一つの product realization に属する全 entrypoint、全到達 call context、全 build condition で守られるかを
証明または反証する whole-product OOB contract-closure verifier です。

## Status

| Field | Value |
| --- | --- |
| Proposal ID | `OOBLENS-SRAD-001` |
| Status | proposal / non-normative |
| Owner issue | `#188` |
| Base cxxlens revision | `cf4d55d291a9869f7f1efcccfed4d6593898ea8d` |
| Language | C++23 |
| Initial frontend | exact Clang 22 |
| Initial host | Linux |
| Initial property | spatial out-of-bounds access |
| Mutation | outside scope |

この proposal は既存の cxxlens authority、Relation Registry、Public API Catalog、provider protocol、
release qualification を変更しません。cxxlens 側の変更候補は設計上の proposal として分離し、
独立 consumer evidence と accepted authority を得るまで stable public surface としません。

## Reading order

1. [Integrated design](ooblens_integrated_design_ja.md)
2. [Contract, class and function catalog](ooblens_contract_catalog_ja.md)
3. [Implementation and qualification plan](ooblens_implementation_plan_ja.md)

## Product sentence

> 宣言された product entrypoint と environment contract から到達可能な全 memory access を会計し、
> caller/callee contract、dynamic dispatch、object extent、pointer provenance、control/value flow を
> 一つの versioned whole-product model に合成して、OOB の存在または不在を証拠付きで判定する。

## Essential distinction

次の関数単体が不正な引数で OOB になり得ることは finding ではありません。

```cpp
int get_hoge(int index)
{
    // pre: 0 <= index && index < m_hoge.size()
    return m_hoge[index];
}
```

`ooblens` が解く問いは次です。

```text
全 product entrypoint から到達するすべての get_hoge call context について、
caller state は get_hoge の precondition を含意するか。
```

- 含意する場合、callee 内の bounds obligation は precondition により discharge される。
- 含意しない到達経路がある場合、entry input から call site、callee access までの反例を提出する。
- call target、external behavior、loop invariant、input domain 等が閉じない場合、安全とは言わず unresolved にする。

## Related current cxxlens work

- `#181`: actual-source installed Clang 22 worker-to-snapshot path
- `#182`: installed Clang 22 task/output-adoption boundary
- `#184`: production incremental materialization coordinator
- `#187`: project catalog / build identity cycle

初期 prototype はこれらを迂回して意味を弱めません。app-owned materialization boundary を明示し、
production claim は上記 blocker の解決後に別途 qualification します。
