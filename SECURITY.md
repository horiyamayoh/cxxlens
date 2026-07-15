# Security Policy

`cxxlens` は pre-alpha であり、安定版のセキュリティサポート期間はまだ定義していません。

脆弱性を発見した場合は公開 Issue に機密情報、exploit、個人情報を記載せず、GitHub の
Private vulnerability reporting が利用できる場合はそれを使用してください。利用できない場合は、
リポジトリ所有者へ非公開の連絡経路を確認してください。

解析対象の source、compile database、設定、diff、外部ツール出力は信頼できない入力として扱います。
報告には再現に必要な最小情報だけを含め、secret や絶対 home path を除去してください。

## Trust boundary

- Clang/GCC frontend、third-party native provider、binary/IR parser は out-of-process で実行する。
- provider binary digest、protocol range、input/environment digest、sandbox assurance を execution evidence に
  bindする。
- command string を shell へ渡さず、argv と environment allowlist を構造化する。
- source excerpt、compile flag、environment、provider diagnostic は privacy policy 下で redaction する。
- crash、timeout、malformed/oversized output、validation rejection は prior published snapshot を破壊しない。

現在の qualified/unsupported state は [Security Profile](schemas/cxxlens_ng_security_profile.yaml) と
[Support matrix](docs/support-matrix.md) を参照してください。bootstrap profile は security certification を
意味しません。
