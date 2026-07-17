# ADR 0043: provider relation output を typed columnar wire batch にする

- Status: Accepted
- Date: 2026-07-17
- Decision owner: provider-protocol/provider-runtime
- Decision issue: #100
- Depends on: ADR 0010, ADR 0038, ADR 0040, ADR 0041

## Context

portable author API の `relation_sink::push()` は `column_chunk` message を使いながら、control に
`batch|row|ordinal` text、payload に row 全体の canonical text を送っていた。host も row text の外形だけを
確認し、descriptor column ごとの type、validity、offset、length、order、chunk digest を復号・再計算しなかった。
この surface は accepted `streamed-column-chunks` contract と wire-compatible ではなく、1 row 1 frame の overhead と
in-process/out-of-process validation 差も生んでいた。

## Decision

`relation_sink` は author-facing row API を維持しつつ、最大 256 row を bounded に蓄積して descriptor 順の column
bufferへ transpose する。emission order は row window、次に descriptor column とする。各 `column_chunk` は exact
13-key deterministic CBOR control map で task、dependency group、atomic output group、batch、descriptor ID/digest、
column、row offset/count、chunk index、encoding、payload digest、chunk digest を bind する。

payload v1 は `CXCC` magic、version、scalar kind、reserved zero、6 個の little-endian u32 section length の後に、
LSB0 validity、LSB0 unknown、value auxiliary、values、unknown reason offsets、strict UTF-8 reasons を並べる。
canonical SDK encoding は bool u8、signed/unsigned 64-bit little-endian、UTF-8/bytes u32 offsets、open/closed symbol の
byte-order unique dictionary と row-aligned u32 index である。optional absence と unknown は別 state とし、unused bitmap
bit、non-present value、non-unknown reason は canonical zero/zero-width に限定する。

`batch_end` は exact 10-key typed CBOR map と `CXBE` v1 payload を使い、descriptor 順の column ID、payload byte
length、chunk count、emission 順 chunk digest、semantic batch digest を bind する。host runtime は SDK と同じ
native-independent codec を用い、task/group/batch/descriptor binding、descriptor order、contiguous row offset、monotonic
chunk index、全 column の最終 row count、terminal summary と digest を再計算する。どれか一つでも不一致なら batch を
`provider.batch-invalid` として reject し、seal しない。

## Consequences

- portable SDK と external provider は row text に依存せず、同じ typed wire value を交換できる。
- multiple rows は bounded chunk に集約され、frame credit と header overhead は column/window 単位になる。
- malformed validity、offset、dictionary index、column reorder、length/digest mismatch は host で fail closed になる。
- public API には column chunk と batch terminal の value-owned codec surface、および `provider.columnar-invalid` が加わる。

## Verification

`tests/unit/sdk/sdk_test.cpp` は bool、signed integer、UTF-8、bytes、optional absence、unknown、dictionary を含む
round-trip、exact metadata、malformed validity/offset/dictionary index、batch summary/digest を検証する。同 test と
`tools/quality/check_ng_provider_protocol.py` は独立に生成した同一 bool payload reference bytes を照合する。
`tests/unit/sdk/provider_runtime_test.cpp` と process fixture は multiple-row aggregation、portable/process state parity、
column length mismatch、column reorder、payload/chunk/batch digest mismatch、unknown descriptor を host rejection まで検証する。
