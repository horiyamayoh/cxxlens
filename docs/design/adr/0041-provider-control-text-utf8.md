# ADR 0041: provider control text を strict UTF-8 scalar sequence に限定する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: provider-protocol/provider-runtime
- Decision issue: #98
- Depends on: ADR 0010, ADR 0030, ADR 0040

## Context

C++ `decode_control_text()` は deterministic CBOR text の major type、shortest length、全体長だけを検証し、text bytes を
UTF-8 として検証しなかった。isolated continuation、truncated sequence、overlong encoding、UTF-16 surrogate、Unicode
range 超過が identity、task terminal、batch field、JSON report、digest projection へ到達できた。一方、valid Unicode scalar
U+0000 は codec 層で一律 reject され、typed field policy と wire validity が混同されていた。

## Decision

CBOR text の唯一の in-memory representation は strict UTF-8 byte string とする。decode は length と canonical CBOR を検証後、
各 sequence の shortest form、continuation、scalar range、surrogate exclusion、truncation を検証し、違反を stable
`provider.malformed-frame` / `control-utf8` とする。ASCII、BMP、non-BMP は同じ validated byte representation を保持し、
Unicode normalization や replacement character への変換は行わない。

public `encode_control_text()` も同じ validator を通過した string だけを deterministic shortest-length CBOR text にする。
provider SDK と process host transcript の string encoder はこの一つの入口を共有し、invalid UTF-8 から wire bytes を生成しない。

U+0000 は Unicode と CBOR text として valid なので codec は保持する。provider runtime の delimiter-based typed control schema は
NUL を許可しないため、decode 後かつ identity comparison/digest/report publication 前に `provider.protocol-state-invalid` とする。
将来 NUL を許す typed field は、その schema が length-aware parsing と canonical representation を明示しなければならない。

## Consequences

- C++ codec と標準 Unicode decoder を使う independent Python backend の accept/reject 集合が一致する。
- invalid bytes を JSON、identity comparison、semantic digest へ流さない。
- wire Unicode validity と typed field lexical policy が分離される。
- valid UTF-8 bytes は正規化せず byte-preserving なので、暗黙の identity collapse を導入しない。

## Verification

`tests/unit/sdk/sdk_test.cpp` は valid ASCII/BMP/non-BMP/NUL の encode/decode と、7 class の invalid UTF-8 に対する encoder/
decoder parity を検証する。`tests/unit/sdk/provider_runtime_test.cpp` は process transcript の invalid UTF-8 と typed NUL を reject
する。`schemas/cxxlens_ng_provider_fuzz_corpus.yaml` と `tests/quality/test_ng_provider_protocol.py` は Python Unicode decoder との
19-case stable differential corpus を検証する。
