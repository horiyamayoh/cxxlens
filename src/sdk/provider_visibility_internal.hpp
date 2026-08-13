#pragma once

// Source-private provider runtime/adoption types must never enter an installed DSO's dynamic
// export surface.  GCC and Clang both honor this on ELF and Mach-O; other toolchains retain the
// source-only header boundary without inventing an unsupported attribute spelling.
#if defined(__GNUC__) || defined(__clang__)
#define CXXLENS_PROVIDER_DETAIL_HIDDEN __attribute__((visibility("hidden")))
#else
#define CXXLENS_PROVIDER_DETAIL_HIDDEN
#endif
