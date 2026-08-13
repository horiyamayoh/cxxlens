# ADR 0071: host-to-provider transcript は共有 encoder/state validator を必ず通す

- Status: Accepted
- Date: 2026-07-18
- Decision owner: provider-protocol/provider-runtime/clang22-provider
- Decision issue: #140
- Clarification feedback: DF-0197 / #197
- Depends on: ADR 0010, ADR 0038, ADR 0040, ADR 0041, ADR 0044, ADR 0069

## Context

provider-to-host response は共有 typed validator を通る一方、host-to-provider の5 frame handshake は process runtime、fixture、
Clang worker が別々に解釈していた。worker は stream/sequence/order/flags、exact manifest、task input/invocation/toolchain/environment
binding を検証せず、旧 delimiter credit parser と手組み failure CBOR は overflow/truncation も起こし得た。

## Decision

public Provider SDK の既存 `encode_host_transcript` と `validate_host_transcript` signature は変更しない。host input の authority は
host encoder、worker decoder、process runtime、conformance validator が共有する一つの incremental transition/digest/length/budget core とする。
既存 vector API は bounded wrapper としてこの core を呼ぶ。

Protocol minor 0 の accepted stream は stream ID 1、sequence 0..4、flags 0 の
`hello_ack, schema_negotiate, open_task, credit, close` exact 5 frame のままとする。`open_task` 以外の payload は empty、全 control は
typed canonical CBOR とする。manifest、task ID、task input digest、normalized invocation digest、toolchain digest、environment digest は
launcher が渡す独立 expectation と exact matchし、`open_task` payload content digest と task input digest も一致しなければならない。

Protocol minor 1 は `required_features: [task-input-chunks-v1]` が双方で negotiation された場合だけ使用する。accepted pattern は
`hello_ack, schema_negotiate, open_task, input_descriptor, input_chunk[0..N-1], credit, close` で、stream 1、flags 0、sequence は 0 から
連続する。`open_task` payload は empty とする。descriptor control schema
`cxxlens.provider-control.input-descriptor.v1` は `{task_id, input_digest, total_bytes, chunk_bytes, chunk_count}`、chunk control schema
`cxxlens.provider-control.input-chunk.v1` は `{task_id, input_digest, chunk_index, offset, byte_count}` の exact field set とする。両
`input_digest` は既存 `task_input_digest` そのものである。

chunk payload は最大 1,048,576 bytes、logical input は最大 67,108,864 bytes、chunk count は最大 64 とし、frame payload limit
16,777,216 bytes は変更しない。index は 0 から連続、offset は prior length の和、非 final chunk は exact `chunk_bytes`、final は exact
remainder とする。zero input は total/count 0、chunk 0 件、SHA-256(empty) とする。各 frame header payload SHA-256、descriptor total、
全 payload の streaming SHA-256 が一致して seal され、task decoder と bottom-up binding が完了するまで provider は `task_accepted` を送らない。
missing、duplicate、reordered、overlap、extra、short、truncated、digest mismatch、limit 超過はすべて acceptance 前に拒否する。

`credit` は provider output の bytes/frames だけを認可し、input byte/frame budget や process の stdout/stderr `transport_bytes` に流用しない。
raw host frame と input spool は diagnostic/transport occurrence に限り、ambient path、FD、environment、shared memory を logical input
authority として使用しない。schema は negotiated major/minor、close は task IDへ bindし、credit は CBOR uint64だけを受理して両値
nonzero とする。

process runtime は共有 incremental encoder だけを使い、canonical manifest と task/protocol authority を argv shell ではなく process
environment port へ渡す。Clang worker と process fixture は frame decode 直後から共有 validator を逐次通し、seal 前に payload を
semantic task として解釈しない。invalid host transcript は outputを生成せず fail closed とする。validated host transcript 後の frontend
payload failure は共有 typed `task_failed_metadata` encoder を使用し、長い task ID でも canonical frameを生成する。

## Verification

minor 0 の exact 5 frame と minor 1 required feature を別々に固定する。minor 1 は zero/one byte、chunk boundary ±1、exact 64 MiB と、
descriptor/chunk schema、count/index/offset/length、missing/duplicate/reorder/extra、per-chunk/final digest、task/input cross-splice、minor/feature
mismatch、64 MiB+1 を検査する。frame type/order/sequence/stream/flags/control、forbidden payload、manifest/schema/close、open_task全bindingも
rejectする。uint64 max credit は受理し、max+1/極長decimal text は typed decoderで rejectする。runtime encoderの
出力を同じ validator が受理し、process fixture と Clang worker が同じ entry pointを呼ぶことを contract gateで固定する。300 byte超の
task ID と invalid payloadでも host が valid typed task_failedを decodeできることを process testで検証する。
