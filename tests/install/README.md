# Installed consumer tests

このディレクトリの試験は、configured prefix と relocated prefix の consumer、provider、
Clang 22 materializer、real-project が同じ通常の install contract を使えることを確認します。

```sh
cmake --preset install-check -DCXXLENS_CLANG_ADAPTER=ON
cmake --build --preset install-check
ctest --preset install-check --label-regex '^install\\.' --output-on-failure
```

materialization report や SQLite/source-closure receipt は実行中の製品結果として維持します。
運用 qualification report、exact-SHA manifest、集約 report は作成しません。Windows/MSVC は
support table に掲載されていないため unsupported です。
