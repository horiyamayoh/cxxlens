# Phase 1 Simplification Design

## 目的と authority

Phase 1 は、現行の semantic contract を弱めずに、重複 authority、過剰な governance、不要な default build dependency、巨大な shadow validator を縮退させる。新しい relation、provider、analysis、public API、wire field、persisted format は追加しない。

Authority の順序は [次世代統合設計](../../design/cxxlens_next_generation_integrated_design_ja.md)、relation registry、各 machine contract、public API catalog、security profile、accepted ADR、test/implementation とする。旧設計書や `docs/archive/` は履歴であり、現行 contract を変更しない。

Phase 1 の変更は原則 `CH-0`。公開意味論、identity、protocol、persistence、不可逆 effect の変更が必要になった時点で Phase 1 を止め、別の `CH-3` 設計提案へ切り出す。

## 保全対象

- `cxxlens::sdk` を単一の public author surface とする。installed `cxxlens` CLI は SDK の薄い入口として保つ。
- `unknown` と empty、partial と incomplete、conflict と differential disagreement、coverage、closure、unresolved reason、guarantee、provenance を失わない。
- immutable snapshot、failed publication isolation、semantic identity、detached native value、bounded input を全 backend/provider で保つ。
- materialization report、SQLite/source-closure の安全 receipt、provider の署名・binary identity・失効・sandbox・canonical semantic certification は製品 runtime の情報として保持する。
- 先行実装済みの OpenSSL Ed25519 port、正規 `stage → validate → publish` writer、Provider Protocol bounded decoder、installed public-header negative check を基準資産として扱う。

## Workstream A — authority consolidation

1. Relation Registry を relation semantics の一意な source とし、generated C++ API、documentation table、必要な schema projection を決定的に生成する。checked-in projection は generator の `--check` で byte-for-byte に検査する。
2. Query、Provider、Store、Materialization は domain ごとの machine contract を維持し、concept ごとに normative owner を一つだけ定める。一つの巨大 contract へ統合しない。
3. Integrated design は製品境界・層・invariant・profile を説明し、field-level exact contract を重複記載しない。
4. ADR の重複 ID を解消し、active/superseded を index で明示する。意味変更は行わない。
5. archive は authority 探索・生成・CI の入力から除外する。

## Workstream B — governance と build path

- ADR、Issue、fault matrix は public semantic boundary、wire/persisted format、security/trust、native lifetime、不可逆 effect など高リスク変更に限定する。private rename、helper 抽出、test fixture、通常の docs 修正へ儀式を要求しない。
- Phase 完了は設計上の gate と実際の変更固有試験・main の決定的 CTest で判定する。Acceptance Manifest、work-unit、review receipt、exact-SHA memo、checksum、qualification JSON、集約 report、phase checkpoint、独自 tag/milestone は作らない。
- `CXXLENS_BUILD_CLANG22_COMPONENTS` を導入し、compiler-neutral core/query/store/CLI の configure を Clang/LLVM/static ICU/Git metadata から分離する。full native/package/CI は同 option を有効化し、source identity は省略しない。
- `dev-core` は Clang adapter を無効化し、Git metadata のない source tree と native dependency 不在でも構成できる。native capability が必要な request は structured unavailable/error とし、silent fallback しない。
- 既存の `dev-clang` と main/release の exact LLVM/Clang 22.1.0 検証は縮小しない。fast label はローカル反復用であり、main suite の代替にしない。
- `dev-core` を構成・build した後は `ctest --preset fast` で compiler-neutral な SDK、negative admission、relation reproducibility、境界検査だけを反復できる。fast lane は local smoke set であり、main の全決定的 CTest や installed/native 検証を代替しない。

## Workstream C — store profiles

新しい public `store_profile` type は追加しない。既存の behavior/test を、次の責務へ分類する。

| profile | 保持する責務 | Phase 1 の扱い |
| --- | --- | --- |
| Memory reference | semantic correctness、query oracle、deterministic result | 常時実行する共通検証 |
| SQLite standard | 一般的な永続、integrity、migration | 製品の通常 backend として維持 |
| SQLite hardened | adversarial/crash-sensitive threat model、VFS/WAL/SHM fault | 即削除せず hardened lane へ分類 |

SQLite 固有実装は、共通 semantic invariant、SQLite standard の integrity、hardened threat-specific safety、speculative/test-only に分ける。SQLite 自身の標準 transaction/recovery が保証する範囲を重複 state machine として再実装しない。ただし immutable publication、failed publication isolation、semantic digest、migration incompatibility、prior snapshot preservation、runtime safety receipt は backend 共通に残す。

## Workstream D — provider profiles

新しい public enum は追加せず、既存の provider selection/runtime/security contract を次の profile として明文化する。

- `local trusted`: in-process/portable provider。signature、certification、sandbox を要求しないが、結果を verified/hardened と表示しない。
- `verified binary`: exact binary identity/digest と structured failure を要求する。identity 不明時に local fallback しない。
- `sandboxed/hardened`: signature、certification、revocation、sandbox の threat model 条件をすべて満たす。receipt/provenance を保持する。

Provider profile の境界は runtime の trust/security 判定にだけ適用し、claim provenance、coverage、unknown、materialization report を省略する理由にはしない。

## Workstream E — shadow validator の縮退

Python の `check_ng_*` は、次の順序で縮退する。

1. schema validity、cross-document ownership、generated reproducibility、public/install boundary だけを残す。
2. Python が再実装している query evaluator、wire codec、provider state machine、store recovery、materialization identity を、対応する C++ direct test へ移す。
3. positive、negative、fault、determinism、resource/error vector の C++ test が揃った領域から、古い simulation を削除する。
4. source path、内部 target 名、diagnostic prose substring に依存する検査は、実際の architecture/public boundary を守るものだけ残す。

縮退は relation/API、query、provider/source-closure、store/SQLite、Clang materialization の縦切りで行う。各縦切りでは一時的に old/new の比較を許すが、primary authority、divergence 時の fail policy、retirement 条件、期限を同じ変更内に明記し、期限のない dual authority を残さない。LOC 削減率や checker 行数は完了条件にしない。

## Golden Journey と受入

既存の typed/dynamic query、relation/claim batch、portable/native provider、memory/SQLite、query execution、R2 vertical slice、installed consumer、CLI admission を `golden` label で選択可能にする。comparator は semantic identity、truth/approximation、condition、interpretation、coverage、closure、unresolved、conflict、保証された ordering だけを比較し、temporary path、PID、timestamp、diagnostic prose、timing、unordered iteration を比較しない。

各縦切りの受入条件は次のとおり。

- 変更固有の positive・negative・fault・determinism/resource/error test が green。
- fresh exact LLVM/Clang 22.1.0 static/shared build、全決定的 CTest、`cxxlens-quality`、installed consumer が green（該当する変更の場合）。
- `dev-core` の Git/Clang/ICU 非依存 configure と structured unavailable/error が確認できる。
- Golden Journey に semantic regression がない。
- product runtime の provenance、coverage、unknown、safety receipt が失われていない。
- public API、relation、wire、persisted format、security trust semantics を変更していない。必要な場合は別提案へ停止する。

## Phase 1 の最終判定

cxxlens repository 内の workstream と試験が成立しても、auto-aha と cxxmonster の旧 `cxxlensProviderSDK` / `cxxlens::provider_sdk` consumer はこの Phase では変更しない。したがって read-only census でこれらの依存が残ることを確認した時点で、cxxlens-local は完了、overall Phase 1 gate は未達とする。未達を waiver して Phase 2A production 実装へ進まない。downstream migration が別途許可された場合のみ、consumer の更新と installed package 回帰を追加して再判定する。

Phase 2A の target architecture、dependency/package boundary、migration map は文書として準備できるが、Phase 1 gate 未達のまま production code の再設計を開始しない。
