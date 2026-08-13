# ADR 0010: provider wire を framed deterministic CBOR と columnar chunk にする

- Status: Accepted
- Date: 2026-07-16
- Decision owner: provider-runtime
- Decision issue: #64
- Tracking issue: #56
- Clarification feedback: DF-0197 / #197

## Context

bootstrap protocol は lifecycle 名だけを列挙し、wire encoding、巨大出力の bounded transfer、backpressure、
resume、cross-relation atomicity、task dependency cycle、failure 後の staging rollback を決めていなかった。
`provider_delta` が全 batch を `vector` で返す案は、大規模 relation と process isolation の双方に適さない。

## Decision

wire は 104 byte fixed header、deterministic CBOR control header、optional binary payload で構成する。fixed header は
magic、protocol major/minor、message type、flags、stream ID、sequence、control/payload length、control/payload の
独立 SHA-256 を持つ。length と negotiated limit は allocation 前に検証する。

CBOR は RFC 8949 deterministic encoding の closed subset とし、indefinite length、float、tag、duplicate map key、
non-shortest integer/length を拒否する。bulk relation data は CBOR row array にせず、validity、fixed-width、offset、
dictionary/blob reference を持つ column chunk payload として送る。外部 CBOR implementation の採用は可能だが、
semantic protocol と canonical vector に適合しなければならない。仕様自体に runtime license dependency はない。

flow control は host が bytes/frames credit を付与し、provider が ACK 済み contiguous sequence までだけ resume
できる方式とする。NG0 は同一 connection/task transaction 内 resume を必須とし、process restart 後は atomic
output group の先頭から再送する。NG1 hardening は durable resume token、heartbeat、progress-rate enforcement、
spill staging を必須にする。

provider output の最小検証単位は batch、最小不可分単位は atomic output group、最小 adoption/rollback 単位は
dependency group とする。hard reference が staged output を跨ぐ atomic group は同一 dependency group に属し、
group 全体を seal/validate してから一括 adopt する。partial result は task 開始前に宣言した dependency group 境界
だけで許可する。adopt は snapshot draft への staging であり、published series head は Issue #63 の transaction が
commit するまで変化しない。

provider task graph は relation requirement、normalizer/deriver stage、explicit dependency から canonical order を作る。
cycle を provider ID の tie-break で隠さない。NG0 は cycle/fixed point を reject する。NG1 で fixed point を許す場合は、
monotone lattice、join、convergence predicate、iteration/budget bound を manifest に明示する。

timeout、cancel、output limit、crash、malformed frame、checksum、truncation、schema/hard-reference failure は別の stable
terminal reason とする。unsealed/invalid dependency group は rollback し、宣言済み partial policy が許す場合だけ
既に sealed/validated group を partial task result に残せる。prior published snapshot は一切変更しない。

reuse/invalidation key は provider semantic contract digest と provider binary digest の両方を含む。同じ provider
ID/version の rebuild を同一 binary と推定しない。将来の binary equivalence relaxation は Issue #65 の署名付き
certification contract と別 ADR を必要とする。

DF-0197 / Issue #197 により Provider Protocol current は 1.1.0 とする。minor 0 の host input は従来どおり
`hello_ack, schema_negotiate, open_task, credit, close` の exact 5 frame で、payload は `open_task` だけに許す。
minor 1 は `required_features: [task-input-chunks-v1]` による negotiation を必須とし、`open_task` の payload を空にして、
続く `input_descriptor` と 0 件以上の `input_chunk` で同じ logical task input bytes を運ぶ。chunk payload は最大 1 MiB、
logical input は最大 64 MiB、chunk は最大 64 件とし、既存の 16 MiB frame payload limit は変更しない。

descriptor は task/input digest、total bytes、chunk bytes、chunk count、各 chunk は task/input digest、zero-based index、
contiguous offset、byte count を exact deterministic CBOR control へ bind する。各 frame payload digest と全 chunk の streaming
SHA-256 を検証し、exact length/digest を seal して task decoder が成功するまで `task_accepted` を送らない。zero input は
total/count 0 の descriptor と SHA-256(empty) を持ち chunk を送らない。provider output credit は input budget と分離し、
ambient path/FD/environment/shared-memory input を authority にしない。

trusted in-process provider と out-of-process provider は同じ logical stream state machine、batch/group validator、
coverage/unresolved/failure contract を通る。in-process path が wire bytes の encode/decode を省略しても、semantic
frame と conformance decision を bypass してはならない。

## Consequences

- provider は全出力を memory vector に保持せず、credit 内の column chunk だけを保持すればよい。
- 1.1 task input も全体 vector を production transport に要求せず、共有 incremental state/digest/budget core を使用する。
- Arrow/Protobuf 等の大規模 mandatory runtime dependency は NG0 kernel に入らない。
- wire major、relation schema、provider semantics、binary identity は独立 version axis のまま保たれる。
- compression/encryption、durable cross-process resume、remote transport は NG1 profile で negotiation する。

## License review

RFC 8949 と本プロジェクトの encoder contract は仕様であり、追加 runtime library を要求しない。将来 CBOR library
を取り込む場合は permissive license、security response、canonical-mode conformance、fuzz corpus 通過を個別に確認する。
現時点の executable oracle は Python standard library のみで wire subset を実装する。

## Verification

`tools/quality/check_ng_provider_protocol.py` が canonical CBOR/frame、version/feature negotiation、credit/ACK/resume、
atomic/dependency group、hard reference staging、task graph、failure/reuse、in/out process parity を検査する。
`schemas/cxxlens_ng_provider_fuzz_corpus.yaml` は truncation、oversize、checksum、noncanonical CBOR、sequence/credit、
unknown required message の mutation corpus を固定し、全 case が crash せず stable rejection になることを確認する。
