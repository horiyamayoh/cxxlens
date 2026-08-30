# Public / Downstream Surface Census

この census は検索可能な first-party と declared downstream の利用実態を整理する設計
入力である。全世界の consumer 不存在を証明するものではなく、実行ログや運用 receipt を
保存するものでもない。

## cxxlens 内の公開 surface

| Surface | 現行 boundary | 判定 |
| --- | --- | --- |
| Author SDK | `include/cxxlens/sdk.hpp`、`include/cxxlens/sdk/*`、`cxxlens::sdk` | stable author surface |
| Thin CLI | `cxxlens` executable と SDK doctor/scaffold | SDK semantics の入口。独自 semantics を持たない |
| Portable provider | `cxxlens::sdk::provider` と provider protocol/runtime | local trusted / verified / hardened を混同しない |
| Clang 22 provider | `<cxxlens/provider/clang22.hpp>`、`cxxlens::clang22_provider_sdk` | exact native opt-in。core の必須依存ではない |
| Installed package | `cxxlens` core package と明示 native package | header/link closure、relocation、unsupported を検査 |

## 観測できた declared downstream

| Repository | 利用している旧/現行 surface | Phase 1 の扱い |
| --- | --- | --- |
| `/home/dhuru/23_auto-aha/auto-aha` | `cxxlensProviderSDK` / `cxxlensClang22ProviderSDK` の package discovery、`cxxlens::provider_sdk` / `cxxlens::clang22_provider_sdk` | read-only census。consumer は変更しない |
| `/home/dhuru/25_cxxmonster/cxxmonster` | profile/design と discovery contract に同じ旧 provider SDK target を参照 | read-only census。consumer は変更しない |
| `/home/dhuru/26_auto-characterizer/auto-characterizer` | 旧 provider SDK の検索可能な利用は確認されていない | migration 対象なしと断定せず、観測限界を保持 |

旧 provider SDK の declared consumer が残る間は、cxxlens-local の試験が green でも overall
Phase 1 gate を通さない。downstream の migration と installed package 回帰は、別途明示的に
許可された作業でのみ実施する。

## 観測限界

- census は現在参照できる repository、公開 catalog、installed consumer fixture に限定する。
- private fork、未取得 branch、未宣言 consumer、将来の package consumer は対象外である。
- unsupported/unknown を「consumer なし」または「互換」と解釈しない。
