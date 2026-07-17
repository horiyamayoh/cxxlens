# ADR 0071: host-to-provider transcript は共有 encoder/state validator を必ず通す

- Status: Accepted
- Date: 2026-07-18
- Decision owner: provider-protocol/provider-runtime/clang22-provider
- Decision issue: #140
- Depends on: ADR 0010, ADR 0038, ADR 0040, ADR 0041, ADR 0044, ADR 0069

## Context

provider-to-host response は共有 typed validator を通る一方、host-to-provider の5 frame handshake は process runtime、fixture、
Clang worker が別々に解釈していた。worker は stream/sequence/order/flags、exact manifest、task input/invocation/toolchain/environment
binding を検証せず、旧 delimiter credit parser と手組み failure CBOR は overflow/truncation も起こし得た。

## Decision

public Provider SDK に `encode_host_transcript` と `validate_host_transcript` を追加する。唯一の accepted stream は stream ID 1、sequence
0..4、flags 0 の `hello_ack, schema_negotiate, open_task, credit, close` である。open_task 以外の payload は empty、全 control は typed
canonical CBOR とする。manifest、task ID、task input digest、normalized invocation digest、toolchain digest、environment digest は launcher
が渡す独立 expectation と exact matchし、payload content digest と task input digest も一致しなければならない。schema は negotiated
major/minor、close は task IDへ bindする。credit は CBOR uint64だけを受理し両値 nonzero とする。

process runtime は共有 encoder だけを使い、canonical manifest と task/protocol authority を argv shell ではなく process environment port
へ渡す。Clang worker と process fixture は frame decode 直後に共有 validator を通し、成功前に payload を解釈しない。invalid host
transcript は outputを生成せず fail closed とする。validated host transcript 後の frontend payload failure は共有 typed
`task_failed_metadata` encoder を使用し、長い task ID でも canonical frameを生成する。

## Verification

frame type/order/sequence/stream/flags/control、missing/duplicate frame、forbidden payload、manifest/schema/close、open_task全binding、payload
digest の mutation を rejectする。uint64 max credit は受理し、max+1/極長decimal text は typed decoderで rejectする。runtime encoderの
出力を同じ validator が受理し、process fixture と Clang worker が同じ entry pointを呼ぶことを contract gateで固定する。300 byte超の
task ID と invalid payloadでも host が valid typed task_failedを decodeできることを process testで検証する。
