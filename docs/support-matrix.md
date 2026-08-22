# Support matrix

製品の対応環境は、試験の長期保存やバイナリ digest ではなく、通常の
[support table](../schemas/cxxlens_support_matrix.yaml) として宣言します。
表にない組合せは `unsupported` です。

| Release version | Surface | OS | Architecture | Compiler/provider major | Linkage |
| --- | --- | --- | --- | --- | --- |
| 1.0.0 | core | Linux | x86_64 | Clang 22 | static |
| 1.0.0 | core | Linux | x86_64 | Clang 22 | shared |
| 1.0.0 | provider-sdk | Linux | x86_64 | Clang 22 | static |
| 1.0.0 | provider-sdk | Linux | x86_64 | Clang 22 | shared |

製品の runtime provenance、claim provenance、coverage、unknown、materialization
report、SQLite/source-closure の安全 receipt、provider の署名・binary identity・失効・sandbox・
canonical semantic certification は機能契約として残ります。これらは release 判定用の運用証跡ではありません。

Windows/MSVC、未掲載 OS、architecture、toolchain/provider major、linkage は引き続き unsupported です。
