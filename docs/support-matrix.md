# Support matrix

| Surface | Current state | Qualification authority |
|---|---|---|
| Distribution | pre-alpha / migration | #56, #71 |
| NG0 contract candidate | minimum vertical slice; not sufficient for 1.0 production | #59, R0–R3 |
| Distribution 1.0 | NG0 + NG1; R0–R4 and G0–G5/GR required | #59, #71 |
| NG2 / NG3 | optional 1.x capability trains; not 1.0 blockers | owner issue + #71 |
| Language | C++23 | build and public-header gates |
| Host | Linux primary | acceptance manifest |
| Public semantic headers | LLVM-free invariant; vNext contract pending | #66, G0, GR |
| Clang 22 source provider | legacy adapter baseline; process provider pending | #70, G4 |
| In-memory snapshot | legacy fact-store seed; vNext pending | #68, G2 |
| SQLite snapshot | legacy fact-store seed; vNext pending | #68, G2 |
| Static/dynamic query | contract pending | #61, #69, G3 |
| GCC/IR/object/binary provider | not NG0 production scope | future provider entry |
| Stable third-party C++ binary plugin ABI | unsupported | design section 0.4 |
| Source mutation apply | not NG0 scope | NG3 / G8 |

Support は provider/relation/toolchain tuple と conformance evidence によって決まり、library version 一つから
推測しません。`contract-pending` や legacy baseline を production-supported と表示してはいけません。
互換性は [release bundle](../schemas/cxxlens_ng_release_bundle.yaml) の context-specific version tuple で判定し、
未 qualified bundle は `doctor` が structured blocker として報告します。
