#pragma once

#include "llvm/clang22/materialization_json.hpp"

/** Return the complete source-free Protocol 2.2 request fixture used by adapter tests. */
[[nodiscard]] cxxlens::detail::clang22::materialization::json_value
cxxlens_test_materialization_request_v2_2_complete_document();
