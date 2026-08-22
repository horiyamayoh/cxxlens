# ADR 0106: 開発・リリース判定を試験結果だけにする

- Status: Accepted
- Date: 2026-08-22
- Supersedes: ADR 0091, ADR 0093, ADR 0095, ADR 0105

## Decision

開発完了は、変更固有の試験と `main` に登録された全決定的回帰試験が成功したことだけで判定する。独立 review、issue コメント、exact SHA の複製、review receipt、work-unit、checksum、qualification JSON、集約 report、Learning checkpoint は repository の完了条件にしない。review は任意である。

release は、手動実行または `v*` tag の `.github/workflows/release.yml` が実行する main 全件、sanitizer、static analysis、stress/repeat、scale、real-project、relocated-install の終了コードだけで判定する。重検査が一件でも失敗した場合は package job を実行しない。

## Boundary

ここで廃止するのは開発・配布を管理するための複製証跡である。Git の履歴と GitHub Actions の job log は通常のプラットフォーム機能として残す。一方、次のデータは製品の実行結果そのものであり、削除しない。

- claim/provenance、coverage、unknown、conflict、materialization report
- SQLite/source-closure の安全 receipt
- provider の署名、binary identity、失効、sandbox、canonical semantic certification

これらは利用者へ返す意味情報・安全判定であり、release qualification の証明書ではない。

## Compatibility and support

Compatibility request/report は v2 の `os`、`architecture`、`toolchain`、`linkage` を使う。v1 の `runtime_qualified`、`evidence_refs`、`qualification_state`、`compat.release-not-qualified` は存在しない。対応環境は `schemas/cxxlens_support_matrix.yaml` の単純な version/surface/environment 表で照合し、未掲載環境と Windows/MSVC は unsupported とする。

## Consequences

試験以外の運用資料を作成・保存・再検証する処理は新しく追加しない。新しい製品 runtime receipt や provenance field は、この ADR の廃止対象ではなく、通常の API・schema・positive/negative/fault test として変更する。
