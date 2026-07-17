# ADR 0062: sanitizer gate は全 first-party object と process 境界を閉包する

- Status: Accepted
- Date: 2026-07-17
- Decision owner: release-quality
- Decision issue: #119
- Depends on: ADR 0005, ADR 0010, ADR 0015, ADR 0055, ADR 0061

## Context

sanitizer option は compiled library と一部 tool にだけ適用され、test、fixture、example、installed consumer の source は
noninstrumented だった。link された runtime だけでは public inline/template caller、lifetime harness、provider child、TSan の race 相手を
観測できない。ctest green は sanitizer coverage の閉包を意味しなかった。

## Decision

sanitizer target helper を全 first-party executable helper に適用し、tool、worker、test、provider fixture、public-header runtime test、全 SDK
example/negative example を library と同じ exact sanitizer set で compile/link する。install test は sanitizer options と compile database
generation を全 consumer configure へ明示的に渡す。nightly は ctest 後に main と 4 installed consumer の compile database 全 entry を検査し、
flag の不足、余分な sanitizer、normal build への漏出、database/consumer の欠落を fail closed にする。noninstrumented first-party
allowlist は空とする。

ASan/UBSan と TSan は別 build とし、混在は configure generation 前に拒否する。ASan、UBSan、test/provider TSan race canary は
sanitizer runtime の固定 exit code 86 を検証する。runtime options は CTest/nightly environment と各 sanitizer executable の default
options の双方へ同じ値を固定する。後者は provider process の minimal `execve` environment を拡張しないために必要である。
intentional crash test の signal semantics を保持するため ASan/TSan は `handle_segv=0` とする。

provider child の通常 resource budget は sanitizer runtime の shadow virtual address space と helper thread を許容しない。instrumented
test request に限り RSS address-space limit と subprocess/thread limit を sanitizer 用に拡張する。production adapter の host-enforced
limit と通常 test の bounded budget は変更しない。

## Verification

normal、ASan/UBSan、TSan の compile database に exact set checker を適用する。ASan/UBSan の address/undefined canary、TSan の
test/provider race canary、instrumented installed consumers、provider runtime と Clang worker protocol test を実行する。mixed configure
negative test は build-system generation が行われないことを検査する。
