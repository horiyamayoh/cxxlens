#pragma once

/**
 * @file worker_parser.hpp
 * @brief Clang 23 parser over one canonical GCC replay input.
 */

#include <cstddef>

#include "sdk/gcc_replay_input_internal.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	enum class parse_terminal
	{
		parsed,
		rejected,
	};

	/** Value-owned parse outcome; it does not contain or imply relation claims. */
	struct parse_result
	{
		parse_terminal terminal{parse_terminal::rejected};
		std::size_t declaration_count{};
		std::size_t warning_count{};
		std::size_t error_count{};
	};

	/** Parse only files bound by the canonical source closure. */
	[[nodiscard]] sdk::result<parse_result>
	parse_replay_input(const sdk::detail::validated_gcc_replay_input& input);
} // namespace cxxlens::detail::clang23_gcc_replay
