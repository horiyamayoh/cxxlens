#pragma once

/**
 * @file bounded_canonical_binary_internal.hpp
 * @brief Source-private structural limits for canonical binary ingress.
 */

#include <cstddef>
#include <span>
#include <string_view>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk::detail
{
	/**
	 * Validate one complete canonical value before the allocating decoder runs.
	 *
	 * The caller supplies its product-specific error domains while this function remains the
	 * single authority for canonical tag, length, tuple, and nesting validation.
	 */
	[[nodiscard]] result<canonical_value>
	decode_bounded_canonical_binary(std::span<const std::byte> input,
									std::size_t initial_depth,
									std::size_t maximum_nesting_depth,
									std::string_view invalid_error_code,
									std::string_view limit_error_code);
} // namespace cxxlens::sdk::detail
