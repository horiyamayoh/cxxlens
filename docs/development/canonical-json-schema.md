# Canonical encoding and schema evolution

JSON/YAML は registry、manifest、debug export の表現であり、claim identity の authority ではありません。
semantic identity は relation descriptor が定める canonical binary tuple から生成します。text projection は
duplicate key、invalid UTF-8、non-finite number、size/depth budget 違反を拒否し、object key と semantic set を
canonical order へ正規化します。

version axis は distribution、kernel semantics、relation descriptor、identity、condition、snapshot format、
provider protocol、native SDK、Logical Query IR、recipe/model ごとに独立です。library version だけから互換性を
推測しません。

- patch: accepted semantic value を変えない文書・test metadata 修正
- minor: optional column、open symbol、unknown-preserving metadata
- major: key、required column、cardinality、condition/closure/identity/source semantics の変更

validation tightening/looseningで row acceptance が変わる場合は patch にしません。unknown required ID/version、
descriptor digest mismatch、unsafe persistent migration は fail-closed です。
