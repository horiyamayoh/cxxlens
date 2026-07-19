# ADR 0063: project catalog identity は exact input projection から bottom-up に導出する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: steward.ng-kernel
- Decision issue: #121
- Clarification feedback: DF-0187 / #187
- Depends on: ADR 0012, ADR 0016, ADR 0050

## Context

公開 `project_catalog` は catalog digest が exact input を bind する invariant を宣言していたが、旧 validator は nonempty field と
compile unit string の sort/unique だけを確認していた。このため catalog ID と digest を据え置いたまま root や compile unit 集合を
変更でき、provider task、cache、snapshot provenance が異なる入力を同一 catalog として扱えた。

## Decision

compile unit entry を `compile_unit_id`、`effective_invocation_digest`、`source_digest`、`environment_digest` の value-owned projection とする。
catalog projection は `cxxlens.project-catalog.v1`、logical root、catalog environment digest、compile-unit ID byte order に正規化した全 entry
の canonical binary tuple とする。同一 ID の identical duplicate と conflicting duplicate はともに拒否する。

ここで `catalog_compile_unit::compile_unit_id` は validated catalog input census 内の stable ID であり、Relation Registry が
`build.compile_unit.v1` の authoritative payload から導出する final relation ID の equality alias ではない。catalog ID は project ID より
上流で確定しなければならないため、catalog entry ID に project、variant、または final relation ID を暗黙に要求してはならない。
`build.compile_unit` を materialize する consumer は、catalog entry の invocation/source/environment digest と relation row の
source/variant/toolchain/effective invocation を exact に cross-bind し、catalog entry ID から final relation ID への明示 mapping を保持する。
同名 field を根拠に二つの ID を同一視すること、final relation ID を catalog digest の fixed-point input として合成することを禁止する。

`project_catalog::make` だけが入力を canonicalize して semantic digest v2 を計算し、`catalog_id` を
`catalog:` + exact catalog digest として構築する。`validate` は digest grammar、typed identity、order、duplicate、projection bytes、digest、
catalog ID を bottom-up に再構成し exact compare する。公開 struct は wire/value transport のため value-owned のまま保持するが、全 trust
boundary は `validate` を必須とし、mutation 後の identity 再利用を拒否する。

portable provider task と native worker catalog loader は同じ factory/validator を使う。`run_worker` は `task_accepted` を送る前に検証する。
`build.project` は同じ validated catalog の catalog ID、digest、logical root、environment digest を保存し、別 codec を導入しない。

## Verification

追加・削除・置換・logical root 変更と malformed digest を negative test にする。input permutation、独立 canonical encoder、
duplicate/conflict、`build.project` field mapping を検査する。forged provider task は frame を一つも送らず `provider.task-invalid` になることを
production harness で確認する。catalog-local input ID と final `build.compile_unit` relation ID の暗黙 alias を拒否し、exact entry-to-relation
mapping が欠落または digest 不一致なら materialization を拒否する。machine-readable contract と public API catalog checker は projection field、
identity boundary、consumer ordering を fail closed に固定する。
