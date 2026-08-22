# Extending the semantic relation platform

拡張の開始点は API や relation の案ではなく、独立 consumer が答えたい質問です。

1. consumer と use case を一意な ID で宣言する。
2. input capture、relation、provider、analysis、model、query、recipe までの capability path を確認する。
3. 不足を `unknown` の actionable reason と依存順の completion plan にする。
4. 新 surface が既存 use case で使われるか、vertical slice に不可避であることを確認する。

orphan public surface と orphan admitted use case は completion を満たしません。API 数、relation 数、checker 数を増やす
こと自体を成果にしません。

## Product result

recipe/analysis の result は `proved`、`disproved`、`unknown`、`partial`、`conflicting` を区別し、coverage、closure、
unresolved、conflict、differential disagreement、guarantee、provenance、logical/physical explain を保持します。
不足を empty success、safe、no finding、unsupported omission へ畳みません。

## High-risk contract

public semantics、identity、protocol、persistence、不可逆 effect、resource bound を変える場合は、実装前に executable state
machine、field availability by phase、phase-authentic success/failure outcome union、minimal witness、finite resource
witness、crash/effect matrix を用意し、仕様/ADR と positive・negative・fault test を追加します。独立 review は任意です。

## Relation/API

1. semantics、identity、condition/interpretation、coverage/closure、evolution rule を定義する。
2. relation registry、schema、validator の positive/negative vector を更新する。
3. IDL から typed header を生成し、dynamic lookup と descriptor/column ID を一致させる。
4. public API catalog、Doxygen、example、install consumer を更新する。
5. memory/SQLite、typed/dynamic IR、root/jobs/insertion-order の parity を試験する。

central enum/switch、pretty type string identity、opaque custom payload、unordered iteration 由来の ID は追加しません。

## Provider

portable provider は manifest、typed detached value、relation sink、coverage/provenance builder と同じ test harness を使います。
filesystem、process、time、hash は port 越しに扱い、selection、binary identity、toolchain、variant、sandbox outcome を明示します。
native provider の AST/TU pointer は callback-scoped borrowed object とし、保存・所有・別 thread 移送を禁止します。provider の署名、
失効、sandbox、canonical semantic certification は製品安全条件として回帰試験で守ります。

## Validation

```sh
cmake --build --preset dev-clang --target <affected-targets>
ctest --preset dev-clang -R '<affected-tests>' --output-on-failure
ctest --preset dev-clang --output-on-failure
cmake --build --preset dev-clang --target cxxlens-quality
```

開発完了は変更固有試験と main の全決定的 CTest が green になった時点です。運用証跡、issue comment、review receipt、exact-SHA
記録、qualification JSON、Learning checkpoint は生成しません。製品 runtime の provenance、coverage、unknown、materialization
report、SQLite/source-closure receipt は維持します。
