# ADR 0038: process provider transcript を typed protocol state machine で検証する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: provider-runtime/protocol
- Decision issue: #95
- Depends on: ADR 0010, ADR 0012, ADR 0015, ADR 0017

## Context

process runtime は wire checksum、sequence、`hello` identity、terminal の存在だけを確認し、schema negotiation、task binding、
message direction、credit、batch seal/digest/schema、coverage side channel を検証せず `provider.success` を返していた。
portable `run_worker` が生成する logical stream に invariant があっても、out-of-process provider はその validator を迂回できた。

## Decision

`process_provider_runtime::execute()` は decoded provider output を一つの typed state machine に通す。success は次をすべて満たす
transcript に限る。

- exact manifest identity の `hello`、exact protocol schema response、provider ID/version/task ID に bind した
  `task_accepted` をこの順で一度だけ受理する。
- provider-to-host direction でない message、terminal 後の output、非連続 sequence、host が付与した byte/frame credit 超過を拒否する。
- `process_task_request::output_descriptors` を task が許可する exact relation schema set とする。batch descriptor ID と
  descriptor digest は manifest offer とこの set の双方へ一致しなければならない。
- batch は一つの dependency/atomic group と batch ID に bind し、row ordinal、canonical detached-row shape、column type、
  required/unknown column、row count、rolling semantic digest を独立検証してから seal する。重複 batch ID と unsealed terminal は拒否する。
- coverage、unresolved、progress は canonical structured side channel として一度ずつ完結させる。task coverage が
  `covered` でない transcript は adoptable success としない。
- cancel/resume/ack など現在の一回実行 session で host だけが送信できる、または negotiation されていない transition を
  provider output で受けた場合は fail closed とする。closure candidate がない session は empty closure として完結する。
- validation failure は frame を semantic output として採用せず、stable machine-readable terminal と diagnostic を report に保持する。

`relation_sink::begin()` は exact descriptor digest を batch begin control に含める。これにより wire validator は SDK の
rolling batch digest を同じ初期値から再計算できる。in-process logical stream と out-of-process wire stream は同じ
accepted/batch/side-channel/terminal transition 列を持つ。

## Consequences

- `succeeded()` は完全に schema-validated、sealed、coverage-balanced な transcript を意味する。
- manifest の relation 名だけでは schema authority にならず、呼出側は exact output descriptor value を task に渡す。
- malformed provider は deterministic な `provider.protocol-state-invalid`、`provider.credit-exceeded`、
  `provider.task-binding-mismatch`、`provider.batch-invalid`、`provider.relation-incompatible`、または
  `provider.coverage-incomplete` で拒否される。
- wire framing が正しいだけの provider output は prior published snapshot を変更しない。

## Verification

`tests/unit/sdk/provider_runtime_test.cpp` は hello+terminal、逆 direction、missing/wrong acceptance、credit 超過、unsealed batch、
unknown descriptor/column、incomplete coverage を拒否し、完全 transcript を成功させる。また同じ provider の
`run_worker` logical stream と process wire stream の transition parity を検証する。
`tests/adapter/clang22/provider_worker_protocol_test.cpp` は Clang worker が schema response を含む structured failure transcript を
返すことを検証する。
