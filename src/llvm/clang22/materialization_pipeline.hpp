#pragma once

#include <cxxlens/sdk/common.hpp>

#include "materialization_claims.hpp"
#include "materialization_store.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/** Build the one Store transaction exclusively from validated request and sealed claims. */
	[[nodiscard]] sdk::result<prepared_store_transaction>
	make_materialization_store_transaction(const validated_materialization_request& request,
										   const sealed_materialization_claims& claims);
} // namespace cxxlens::detail::clang22::materialization
