# コーディング規約

- C++23、UTF-8、100 columns を基準とする。
- namespace、type、function、variable は lower snake case、macro は `CXXLENS_` prefix とする。
- pointer/reference 記号は型側へ置き、Allman braces を使用する。
- include は標準ライブラリ、外部ライブラリ、cxxlens、同一 component の順に group 化する。
- ownership は value、reference、smart pointer で明示し、raw owning pointer を使用しない。
- accessor/observer のみ安易に `noexcept` とし、allocation や user callback の例外を隠さない。
- policy を意味不明な bool で表さず enum/class を使用する。
- serialization 前に canonical sort し、absolute checkout root や時刻を identity に含めない。

`clang-format` は機械的なレイアウトの正本、`clang-tidy` と compiler warnings は静的規則の正本とする。
