# Security Policy

`cxxlens` は pre-alpha であり、安定版のセキュリティサポート期間はまだ定義していません。

脆弱性を発見した場合は公開 Issue に機密情報、exploit、個人情報を記載せず、GitHub の
Private vulnerability reporting が利用できる場合はそれを使用してください。利用できない場合は、
リポジトリ所有者へ非公開の連絡経路を確認してください。

解析対象の source、compile database、設定、diff、外部ツール出力は信頼できない入力として扱います。
報告には再現に必要な最小情報だけを含め、secret や絶対 home path を除去してください。
