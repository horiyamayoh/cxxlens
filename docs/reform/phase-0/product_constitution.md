# Product Constitution

この文書は、[全体設計書](../../../cxxlens_Refactoring_Program_Overall_Design_2026-08-26.md) の Phase 0 で保全対象を確認するための短い規約である。製品の意味論は [次世代統合設計](../../design/cxxlens_next_generation_integrated_design_ja.md)、各 machine contract、public API catalog、security profile が所有する。この文書はそれらを置き換えない。

## 維持する不変条件

1. Compiler-native object、pointer、reference、ABI handle は provider callback 境界を越えて保存・所有・thread 移送しない。core には detached value だけを渡す。
2. `unknown`、`unsupported`、`unavailable`、`failed`、`truncated`、`stale`、closure 不足を empty success に変換しない。
3. 存在しないこと、exhaustive set、anti-join、unreachable を主張するには適用可能な closure または同等の完全性根拠を要求する。根拠がなければ positive fact は返しても absence は `unknown` とする。
4. 公開済み semantic snapshot は immutable とし、failed/cancelled/rejected materialization が既存 snapshot を破壊しない。
5. Semantic identity は checkout root、pointer、timestamp、PID、thread ID、arrival order、unordered iteration、表示 prose に依存しない。
6. Production compiler、target、ABI、build variant、provider、interpretation domain、fallback を明示する。Clang replay を GCC/MSVC native exact として表示しない。
7. Same-domain conflict と cross-domain differential disagreement を first-wins、priority、arrival order で消さない。
8. Source mutation、artifact publication 等の不可逆 effect は plan、precondition、独立 validator、dry-run、transaction を経由する。
9. Wire、JSON、filesystem、persistent store、build capture は decode/allocation 前に bounded input とする。保存する argv/environment は allowlist と redaction を通し、credential、token、secret を保存しない。
10. 一つの semantics に一つの hand-maintained normative authority を置く。C++ header、schema、documentation、fixture は authority から生成または検査し、期限のない dual authority を残さない。

## 結果と出所

Public result は適用範囲に応じて `proved`、`disproved`、`unknown`、`partial`、`conflicting` を区別する。これらは existing truth support、approximation、coverage、closure、unresolved、conflict を置き換えない。

Product runtime が返す次の情報は、開発・release の運用証跡とは異なる製品機能であり、削除・省略しない。

- claim、producer、basis、provenance
- coverage、closure、unresolved reason、conflict、differential disagreement
- guarantee、interpretation、materialization report
- SQLite/source-closure の安全 receipt
- provider の署名、binary identity、失効、sandbox、canonical semantic certification

`unknown` は actionable な不足理由と観測可能な回復条件を含む。回復条件を確定できない場合、推測した completion plan を返さない。

## 現行 surface と profile

- Public author surface は `cxxlens::sdk` とする。installed `cxxlens` CLI はこの SDK の薄い入口であり、CLI の capability admission は SDK の意味論を置き換えない。
- OpenSSL Ed25519 port は provider trust 境界を支える内部実装であり、鍵や trust anchor を CLI・repository に埋め込まない。
- Memory reference、SQLite standard、SQLite hardened は profile として分類する。hardened の安全機能は Phase 1 で削除せず、standard が必要とする immutable publication、failed publication isolation、migration safety、prior snapshot preservation は共通 invariant として維持する。
- Local trusted、verified binary、sandboxed/hardened provider を区別する。local の結果を verified/hardened に自動昇格せず、exact identity、signature、certification、sandbox の欠落を silent fallback で隠さない。

## 開発・release 判定

開発完了は、変更固有の positive・negative・fault・determinism/resource/error test と、main に登録された全決定的 CTest が成功したことで判定する。レビュー、receipt、checkpoint、qualification report は試験を代替しない。

Release は release workflow の全対象、sanitizer、static analysis、stress/repeat、scale、real-project、relocated-install の終了コードで別途判定する。Phase 0–1 の local/main green を release-qualified と呼ばない。

## Phase 0 の適用範囲

本プログラムの実装対象は cxxlens repository 内に限定する。auto-aha、cxxmonster 等の既知 downstream は read-only census で surface 利用を確認するだけで、Phase 1 中に変更しない。そのため、旧 `cxxlensProviderSDK` / `cxxlens::provider_sdk` consumer が残る場合、cxxlens-local の試験が green でも Phase 1 全体 gate は未達とする。
