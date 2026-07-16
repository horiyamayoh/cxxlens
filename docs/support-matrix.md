# Support matrix

| Surface | Current state | Qualification authority |
|---|---|---|
| Distribution | pre-alpha / migration | #56, #71 |
| NG0 contract candidate | minimum vertical slice; not sufficient for 1.0 production | #59, R0–R3 |
| Distribution 1.0 | NG0 + NG1; R0–R4 and G0–G5/GR required | #59, #71 |
| NG2 / NG3 | optional 1.x capability trains; not 1.0 blockers | owner issue + #71 |
| Language | C++23 | build and public-header gates |
| Host | Linux primary | acceptance manifest |
| Author SDK (`cxxlens::provider_sdk`) | implemented/installed; LLVM-free build and consumer qualified | #66; release qualification remains #71 |
| Clang 22 native author SDK | callback/detachment helpers implemented; process provider pending | #66, #70, G4 |
| In-memory snapshot | legacy fact-store seed; vNext pending | #68, G2 |
| SQLite snapshot | legacy fact-store seed; vNext pending | #68, G2 |
| Static/dynamic query authoring | common descriptor/IR SDK implemented; production execution backend pending | #61, #66, #69, G3 |
| Provider trust/discovery | exact security contract accepted; production certificate absent | #65, #70, G4 |
| GCC/IR/object/binary provider | not NG0 production scope | future provider entry |
| Stable third-party C++ binary plugin ABI | unsupported | design section 0.4 |
| Source mutation apply | not NG0 scope | NG3 / G8 |

Support は provider ID/version/binary digest/relation/interpretation/toolchain/platform tuple と conformance evidence によって決まり、library version 一つから
推測しません。`contract-pending` や legacy baseline を production-supported と表示してはいけません。
machine-readable authority は [provider support matrix](../schemas/cxxlens_ng_provider_support_matrix.yaml) です。現在の
production certificate は0件であり、planned/conformance-only entry を production support と推測してはいけません。
互換性は [release bundle](../schemas/cxxlens_ng_release_bundle.yaml) の context-specific version tuple で判定し、
未 qualified bundle は `doctor` が structured blocker として報告します。
